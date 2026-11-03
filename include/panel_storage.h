// Small files in the panel's flash (LittleFS on the "spiffs" partition).
#pragma once
#include <string>

namespace storage {
bool begin();                                       // mounts, formatting on first use
bool read(const char *path, std::string &out);
bool write(const char *path, const std::string &data);  // atomic: temp file + rename
}  // namespace storage
