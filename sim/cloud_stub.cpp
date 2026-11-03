// Desktop preview stand-in for the account module: pretends to be signed in
// and "syncs" two recipes shaped like the real account ones.
#include <string.h>
#include "cloud.h"
#include "recipes.h"
namespace cloud {
static Acct st = Acct::SignedIn;
static bool sent = false;
void begin() {}
Acct state() { return st; }
const char *email() { return "name@email.com"; }
const char *message() { return ""; }
bool busy() { return false; }
time_t last_sync() { return 1793279520; }
void sign_in(const char *, const char *) {}
void sign_out() {}
void sync_now() {}
void push(const PanelRecipe &) {}
void remove(const char *) {}
bool poll(Event &e) {
  if (sent) return false;
  sent = true;
  auto *list = new std::vector<PanelRecipe>(recipes::seeds());
  int n = 803560;
  for (auto &p : *list) { p.kind = KIND_MINE; snprintf(p.id, sizeof p.id, "%d", n++); p.favorite = false; }
  PanelRecipe sh = recipes::starters()[1];
  strcpy(sh.r.name, "Yaye [Anaerobic] {Filter}"); sh.kind = KIND_SHARED; strcpy(sh.id, "900001"); sh.color = 0xE2DBA6;
  list->push_back(sh);
  e = Event();
  e.type = Ev::Synced;
  e.list = list;
  return true;
}
}  // namespace cloud
