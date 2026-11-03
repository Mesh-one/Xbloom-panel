#include "panel_storage.h"

#include <LittleFS.h>

namespace storage {

bool begin() { return LittleFS.begin(true, "/fs", 5, "spiffs"); }

bool read(const char *path, std::string &out) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  out.resize(f.size());
  const size_t n = f.read((uint8_t *)&out[0], out.size());
  f.close();
  return n == out.size();
}

bool write(const char *path, const std::string &data) {
  std::string tmp = std::string(path) + ".tmp";
  File f = LittleFS.open(tmp.c_str(), "w");
  if (!f) return false;
  const bool ok = f.write((const uint8_t *)data.data(), data.size()) == data.size();
  f.close();
  if (!ok) return false;
  LittleFS.remove(path);
  return LittleFS.rename(tmp.c_str(), path);
}

}  // namespace storage
