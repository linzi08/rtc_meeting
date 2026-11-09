// =============================================================================
// 极简日志：[HH:MM:SS][tag] message（控制台 + stdout，避免引第三方日志库）
// =============================================================================
#pragma once
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <string>

namespace vm {

inline void Logf(const char* tag, const char* fmt, ...) {
  char ts[32];
  time_t     t  = time(nullptr);
  struct tm  lt;
  localtime_s(&lt, &t);
  strftime(ts, sizeof(ts), "%H:%M:%S", &lt);

  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  std::fprintf(stdout, "[%s][%s] %s\n", ts, tag, buf);
  std::fflush(stdout);
}

// 常用标签：ws / protoo / room / codec / media / render / turn / audio
#define LOGT(tag, ...) ::vm::Logf(tag, __VA_ARGS__)

}  // namespace vm
