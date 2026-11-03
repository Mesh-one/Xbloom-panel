// Your xBloom account: sign in, download recipes (Created + Shared), save
// edits back. Runs in its own task; the UI talks to it through jobs and polls
// for results. Unofficial API (worked out in xbloom-py); may change.
#pragma once
#include <stdint.h>
#include <time.h>

#include <vector>

#include "library.h"

namespace cloud {

enum class Acct : uint8_t { SignedOut, SigningIn, SignedIn, BadLogin };

void begin();                      // loads the saved sign-in; syncs once Wi-Fi is up
Acct state();
const char *email();
const char *message();             // last problem, "" if none
bool busy();                       // a sync / save is running
time_t last_sync();                // 0 = never (this session)

void sign_in(const char *email, const char *password);  // saved on success
void sign_out();
void sync_now();
void push(const PanelRecipe &p);   // create (no id yet) or update
void remove(const char *id);

enum class Ev : uint8_t { Synced, SyncFailed, Pushed, PushFailed, Removed, RemoveFailed, Auth };
struct Event {
  Ev type;
  uint32_t uid = 0;                     // Pushed / PushFailed
  char id[24] = "";                     // Pushed: the account's id for it; Removed: what was removed
  char msg[256] = "";
  std::vector<PanelRecipe> *list = nullptr;  // Synced: the account's recipes; UI takes ownership
};
bool poll(Event &e);

}  // namespace cloud
