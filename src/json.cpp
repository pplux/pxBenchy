#include "json.h"
#include <cstdarg>
#include <cstdio>

void str_appendf(std::string* o, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) { return; }
  if ((size_t)n < sizeof(buf)) {
    o->append(buf, (size_t)n);
    return;
  }
  size_t at = o->size();
  o->resize(at + (size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(&(*o)[at], (size_t)n + 1, fmt, ap);
  va_end(ap);
  o->resize(at + (size_t)n);
}

std::string json_string(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if ((unsigned char)c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
      o += buf;
    } else {
      o += c;
    }
  }
  return o + "\"";
}
