#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>

inline void logEvent(const char *tag, const char *fmt, ...) {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);

  char body[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  std::fprintf(stderr, "%s [%s] %s\n", ts, tag ? tag : "?", body);
  std::fflush(stderr);
}
