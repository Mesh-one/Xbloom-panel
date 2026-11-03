#include <map>
#include "panel_storage.h"
static std::map<std::string, std::string> files;
namespace storage {
bool begin() { return true; }
bool read(const char *p, std::string &out) { auto it = files.find(p); if (it == files.end()) return false; out = it->second; return true; }
bool write(const char *p, const std::string &d) { files[p] = d; return true; }
}
