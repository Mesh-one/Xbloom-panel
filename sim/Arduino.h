#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint32_t millis();
struct SerialStub {
  void println(const char *s) { puts(s); }
  template <typename... A> void printf(const char *f, A... a) { ::printf(f, a...); }
};
static SerialStub Serial;
