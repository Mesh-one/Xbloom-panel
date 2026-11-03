#include "cloud.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

#include <string>

#define ARDUINOJSON_ENABLE_ARDUINO_STRING 0
#include <ArduinoJson.h>

#include "cloud_crypto.h"
#include "panel_net.h"

namespace {

const char *API = "https://client-api.xbloom.com/";
const uint32_t AUTO_SYNC_MS = 15 * 60 * 1000;

enum class JobType : uint8_t { SignIn, SignOut, Sync, Push, Remove };
struct Job {
  JobType type;
  PanelRecipe *recipe = nullptr;  // Push (heap copy, freed by the task)
  char a[96] = "", b[96] = "";    // SignIn: email/password; Remove: id
};

QueueHandle_t q_jobs, q_events;
Preferences prefs;
volatile cloud::Acct g_state = cloud::Acct::SignedOut;
volatile bool g_busy = false;
volatile time_t g_last_sync = 0;
char g_email[96] = "", g_pass[96] = "", g_msg[256] = "";
// xBloom's session token is a long signed string (hundreds of characters); it must be kept whole.
char g_token[2048] = "";
long long g_member = 0;
uint32_t g_last_sync_ms = 0;
bool g_sync_after_wifi = true;

void emit(cloud::Event &e) {
  if (xQueueSend(q_events, &e, pdMS_TO_TICKS(100)) != pdTRUE && e.list) delete e.list;
}
void emit_simple(cloud::Ev t, const char *msg = "", uint32_t uid = 0, const char *id = "") {
  cloud::Event e;
  e.type = t;
  e.uid = uid;
  strlcpy(e.id, id, sizeof e.id);
  strlcpy(e.msg, msg, sizeof e.msg);
  emit(e);
}
void set_msg(const char *m) { strlcpy(g_msg, m, sizeof g_msg); }

// ---- transport -------------------------------------------------------------------
int rng(void *, unsigned char *out, size_t n) {
  esp_fill_random(out, n);
  return 0;
}

bool http_post_once(const char *endpoint, const std::string &body, std::string &resp, std::string &err) {
  std::string url = std::string(API) + endpoint;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = 30000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;  // verify xBloom's certificate
  cfg.buffer_size = 4096;
  cfg.buffer_size_tx = 2048;
  cfg.keep_alive_enable = false;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) { err = "out of memory"; return false; }
  esp_http_client_set_header(c, "Content-Type", "application/json");
  esp_http_client_set_header(c, "Referer", "https://share-h5.xbloom.com/");
  esp_http_client_set_header(c, "User-Agent", "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X)");
  bool ok = false;
  const uint32_t t0 = millis();
  const unsigned ram_k = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
  const esp_err_t oe = esp_http_client_open(c, body.size());
  char why[128];
  if (oe != ESP_OK) {
    // Say why, so a screenshot is enough to diagnose it.
    int tls_code = 0, tls_flags = 0;
    esp_http_client_get_and_clear_last_tls_error(c, &tls_code, &tls_flags);
    // tls_flags carries the socket errno when the TCP connection itself failed
    snprintf(why, sizeof why, "can't reach xBloom (tls -0x%04X, errno %d, ram %uk)", (unsigned)-tls_code, tls_flags,
             ram_k);
    err = why;
  } else if (esp_http_client_write(c, body.data(), body.size()) != (int)body.size()) {
    snprintf(why, sizeof why, "send failed (errno %d, ram %uk)", esp_http_client_get_errno(c), ram_k);
    err = why;
  } else {
    const int64_t hdr = esp_http_client_fetch_headers(c);
    const int status = esp_http_client_get_status_code(c);
    resp.clear();
    char buf[1024];
    int n;
    while ((n = esp_http_client_read(c, buf, sizeof buf)) > 0) resp.append(buf, n);
    if (status == 200 && !resp.empty()) {
      ok = true;
    } else {
      snprintf(why, sizeof why, "xBloom said %d (%u B, %us, ram %uk): ", status, (unsigned)resp.size(),
               (unsigned)((millis() - t0) / 1000), ram_k);
      err = why;
      // Show what the server said, so a photo of the screen is enough to diagnose it.
      std::string body = resp.substr(0, 160);
      for (char &ch : body)
        if (ch == '\n' || ch == '\r') ch = ' ';
      err += body.empty() ? "(empty)" : body;
      (void)hdr;
    }
  }
  if (!ok) Serial.printf("[cloud] %s: %s\n", endpoint, err.c_str());
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return ok;
}

// One retry: a dropped connection mid-download shouldn't fail a whole sync.
bool http_post(const char *endpoint, const std::string &body, std::string &resp, std::string &err) {
  if (http_post_once(endpoint, body, resp, err)) return true;
  vTaskDelay(pdMS_TO_TICKS(1500));
  return http_post_once(endpoint, body, resp, err);
}

std::string envelope(bool with_auth, const char *extra_json = nullptr) {
  JsonDocument d;
  d["interfaceVersion"] = 20240918;
  d["skey"] = "testskey";
  d["clientType"] = 2;
  d["phoneType"] = "Android";
  d["languageType"] = 0;
  if (with_auth) {
    d["memberId"] = g_member;
    d["token"] = g_token;
  }
  if (extra_json) {
    JsonDocument x;
    deserializeJson(x, extra_json);
    for (JsonPairConst kv : x.as<JsonObjectConst>()) d[kv.key()] = kv.value();
  }
  std::string s;
  serializeJson(d, s);
  return s;
}

// POST an encrypted (tu*) request: body is the base64 cipher text as a JSON string.
bool post_encrypted(const char *endpoint, const std::string &plain, std::string &resp, std::string &err) {
  std::string b64;
  if (!cloudcrypto::encrypt_body(cloudcrypto::XBLOOM_PUBLIC_KEY_PEM, plain, b64, rng, nullptr)) {
    err = "encryption failed";
    return false;
  }
  return http_post(endpoint, "\"" + b64 + "\"", resp, err);
}

// Generic reply check for non-list calls.
bool reply_ok(const std::string &resp, std::string &err, bool &expired, std::string *table_id = nullptr) {
  expired = false;
  JsonDocument d;
  if (deserializeJson(d, resp)) { err = "bad reply from xBloom"; return false; }
  if (strcmp(d["result"] | "", "success") == 0) {
    if (table_id) {
      JsonVariantConst t = d["tableId"];
      *table_id = t.is<const char *>() ? std::string(t.as<const char *>()) : std::to_string(t.as<long long>());
    }
    return true;
  }
  for (const char *k : {"code", "errorCode", "resultCode", "status"}) {
    long long c = d[k].is<const char *>() ? atoll(d[k].as<const char *>()) : d[k].as<long long>();
    if (c == 10001 || c == 20003) expired = true;
  }
  for (const char *k : {"msg", "message", "errorMsg", "errorMessage", "error", "reason", "tips", "info"})
    if (d[k].is<const char *>() && *d[k].as<const char *>()) { err = d[k].as<const char *>(); return false; }
  err = "request failed";
  return false;
}

// ---- account -----------------------------------------------------------------------
bool login(const char *email, const char *pass, std::string &err) {
  JsonDocument d;
  deserializeJson(d, envelope(false));
  d["email"] = email;
  d["password"] = pass;
  std::string body, resp;
  serializeJson(d, body);
  if (!http_post("tMemberLogin.thtml", body, resp, err)) return false;
  JsonDocument r;
  if (deserializeJson(r, resp)) { err = "bad reply from xBloom"; return false; }
  if (strcmp(r["result"] | "", "success") != 0) {
    err = "Wrong email or password";
    for (const char *k : {"msg", "message", "info", "error"})
      if (r[k].is<const char *>() && *r[k].as<const char *>()) { err = r[k].as<const char *>(); break; }
    return false;
  }
  JsonVariantConst id = r["member"]["tableId"];
  const char *tok = r["token"] | "";
  if (id.isNull() || !*tok) { err = "xBloom didn't return a session"; return false; }
  if (strlen(tok) >= sizeof g_token) { err = "xBloom's session is too long for the panel"; return false; }
  Serial.printf("[cloud] signed in, token %u chars\n", (unsigned)strlen(tok));
  g_member = id.is<const char *>() ? atoll(id.as<const char *>()) : id.as<long long>();
  strlcpy(g_token, tok, sizeof g_token);
  prefs.putLong64("mid", g_member);
  prefs.putString("tok", g_token);
  return true;
}

// Run `call`; if xBloom says the session expired, sign in again with the saved
// password and retry once.
template <typename F>
bool with_relogin(F call, std::string &err) {
  bool expired = false;
  if (call(expired, err)) return true;
  if (!expired || !g_pass[0]) return false;
  if (!login(g_email, g_pass, err)) {
    g_state = cloud::Acct::BadLogin;
    emit_simple(cloud::Ev::Auth, err.c_str());
    return false;
  }
  return call(expired, err);
}

bool fetch_list(const char *endpoint, RecipeKind kind, std::vector<PanelRecipe> &out, bool &expired,
                std::string &err) {
  std::string resp;
  if (!post_encrypted(endpoint, envelope(true, "{\"pageNumber\":1,\"countPerPage\":100,\"adaptedModel\":1}"), resp,
                      err))
    return false;
  return library::parse_cloud_list(resp.data(), resp.size(), kind, out, err, expired);
}

void do_sync() {
  std::string err;
  if (!g_token[0] && g_pass[0] && !login(g_email, g_pass, err)) {
    set_msg(err.c_str());
    emit_simple(cloud::Ev::SyncFailed, err.c_str());
    return;
  }
  auto *list = new std::vector<PanelRecipe>();
  const bool ok = with_relogin(
      [&](bool &expired, std::string &e) {
        list->clear();
        if (!fetch_list("tuMyTeaRecipeCreated.tuhtml", KIND_MINE, *list, expired, e)) return false;
        std::vector<PanelRecipe> shared;
        bool exp2 = false;
        std::string e2;
        if (fetch_list("tuMyRecipeShared.tuhtml", KIND_SHARED, shared, exp2, e2)) {
          for (auto &s : shared) {
            bool dup = false;
            for (auto &m : *list) dup |= strcmp(m.id, s.id) == 0;
            if (!dup) list->push_back(s);
          }
        }  // a failed Shared list alone isn't fatal (same as xbloom-py)
        return true;
      },
      err);
  g_last_sync_ms = millis();
  if (ok) {
    g_last_sync = time(nullptr);
    set_msg("");
    cloud::Event e;
    e.type = cloud::Ev::Synced;
    e.list = list;
    emit(e);
    Serial.printf("[cloud] synced %u recipes\n", (unsigned)list->size());
  } else {
    delete list;
    set_msg(err.c_str());
    emit_simple(cloud::Ev::SyncFailed, err.c_str());
    Serial.printf("[cloud] sync failed: %s\n", err.c_str());
  }
}

void do_push(PanelRecipe *p) {
  std::string err, new_id;
  const bool create = p->id[0] == 0;
  if (create && p->created_ms == 0) p->created_ms = (int64_t)time(nullptr) * 1000;
  const bool ok = with_relogin(
      [&](bool &expired, std::string &e) {
        std::string resp;
        const std::string body = library::cloud_body(*p, envelope(true), create ? nullptr : p->id);
        if (!post_encrypted(create ? "tuRecipeAdd.tuhtml" : "tuRecipeUpdate.tuhtml", body, resp, e)) return false;
        return reply_ok(resp, e, expired, create ? &new_id : nullptr);
      },
      err);
  if (ok) emit_simple(cloud::Ev::Pushed, "", p->uid, create ? new_id.c_str() : p->id);
  else emit_simple(cloud::Ev::PushFailed, err.c_str(), p->uid, p->id);
  delete p;
}

void do_remove(const char *id) {
  std::string err;
  const bool ok = with_relogin(
      [&](bool &expired, std::string &e) {
        JsonDocument x;
        x["tableId"] = id;
        std::string extra, resp;
        serializeJson(x, extra);
        if (!post_encrypted("tuRecipeDelete.tuhtml", envelope(true, extra.c_str()), resp, e)) return false;
        return reply_ok(resp, e, expired);
      },
      err);
  emit_simple(ok ? cloud::Ev::Removed : cloud::Ev::RemoveFailed, err.c_str(), 0, id);
}

void task(void *) {
  Job j;
  for (;;) {
    const bool online = net::state() == net::Wifi::Connected;
    if (!online) g_sync_after_wifi = true;
    if (online && g_state == cloud::Acct::SignedIn &&
        (g_sync_after_wifi || millis() - g_last_sync_ms > AUTO_SYNC_MS)) {
      g_sync_after_wifi = false;
      g_busy = true;
      do_sync();
      g_busy = false;
    }
    if (xQueueReceive(q_jobs, &j, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
    if (!online && j.type != JobType::SignOut) {
      if (j.type == JobType::Push) {
        emit_simple(cloud::Ev::PushFailed, "No Wi-Fi", j.recipe->uid, j.recipe->id);
        delete j.recipe;
      } else if (j.type == JobType::Remove) {
        emit_simple(cloud::Ev::RemoveFailed, "No Wi-Fi", 0, j.a);
      } else if (j.type == JobType::SignIn) {
        g_state = cloud::Acct::SignedOut;
        set_msg("Connect to Wi-Fi first");
        emit_simple(cloud::Ev::Auth, g_msg);
      } else {
        emit_simple(cloud::Ev::SyncFailed, "No Wi-Fi");
      }
      continue;
    }
    g_busy = true;
    switch (j.type) {
      case JobType::SignIn: {
        std::string err;
        if (login(j.a, j.b, err)) {
          strlcpy(g_email, j.a, sizeof g_email);
          strlcpy(g_pass, j.b, sizeof g_pass);
          prefs.putString("email", g_email);
          prefs.putString("pass", g_pass);
          g_state = cloud::Acct::SignedIn;
          set_msg("");
          emit_simple(cloud::Ev::Auth);
          do_sync();
        } else {
          g_state = cloud::Acct::BadLogin;
          set_msg(err.c_str());
          emit_simple(cloud::Ev::Auth, err.c_str());
        }
        break;
      }
      case JobType::SignOut:
        prefs.clear();
        g_email[0] = g_pass[0] = g_token[0] = 0;
        g_member = 0;
        g_last_sync = 0;
        g_state = cloud::Acct::SignedOut;
        set_msg("");
        emit_simple(cloud::Ev::Auth);
        break;
      case JobType::Sync: do_sync(); break;
      case JobType::Push: do_push(j.recipe); break;
      case JobType::Remove: do_remove(j.a); break;
    }
    g_busy = false;
  }
}

void post_job(const Job &j) {
  if (xQueueSend(q_jobs, &j, pdMS_TO_TICKS(50)) != pdTRUE && j.recipe) delete j.recipe;
}

}  // namespace

namespace cloud {

void begin() {
  q_jobs = xQueueCreate(16, sizeof(Job));
  q_events = xQueueCreate(16, sizeof(Event));
  prefs.begin("acct", false);
  strlcpy(g_email, prefs.getString("email", "").c_str(), sizeof g_email);
  strlcpy(g_pass, prefs.getString("pass", "").c_str(), sizeof g_pass);
  strlcpy(g_token, prefs.getString("tok", "").c_str(), sizeof g_token);
  g_member = prefs.getLong64("mid", 0);
  // Versions before 3.1.4 cut the token to 95 characters, which xBloom rejects
  // with a server error. Drop such a token; the next sync signs in again.
  if (strlen(g_token) == 95) g_token[0] = 0;
  if (g_email[0] && (g_token[0] || g_pass[0])) g_state = Acct::SignedIn;
  // TLS + JSON need a roomy stack; the task's buffers mostly live in PSRAM.
  xTaskCreatePinnedToCore(task, "cloud", 12288, nullptr, 1, nullptr, 0);
}

Acct state() { return g_state; }
const char *email() { return g_email; }
const char *message() { return g_msg; }
bool busy() { return g_busy; }
time_t last_sync() { return g_last_sync; }

void sign_in(const char *email, const char *password) {
  Job j;
  j.type = JobType::SignIn;
  strlcpy(j.a, email, sizeof j.a);
  strlcpy(j.b, password, sizeof j.b);
  g_state = Acct::SigningIn;
  set_msg("");
  post_job(j);
}
void sign_out() {
  Job j;
  j.type = JobType::SignOut;
  post_job(j);
}
void sync_now() {
  Job j;
  j.type = JobType::Sync;
  post_job(j);
}
void push(const PanelRecipe &p) {
  Job j;
  j.type = JobType::Push;
  j.recipe = new PanelRecipe(p);
  post_job(j);
}
void remove(const char *id) {
  Job j;
  j.type = JobType::Remove;
  strlcpy(j.a, id, sizeof j.a);
  post_job(j);
}

bool poll(Event &e) { return xQueueReceive(q_events, &e, 0) == pdTRUE; }

}  // namespace cloud
