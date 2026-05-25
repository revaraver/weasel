#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace weasel_timing {

inline unsigned long long QpcUs() {
  LARGE_INTEGER freq;
  LARGE_INTEGER now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  return static_cast<unsigned long long>(now.QuadPart * 1000000ull / freq.QuadPart);
}

inline bool Enabled() {
  static int enabled = -1;
  if (enabled != -1) return enabled != 0;

  wchar_t value[8] = {0};
  DWORD n = GetEnvironmentVariableW(L"WEASEL_IME_TIMING", value, 8);
  if (n > 0) {
    enabled = (value[0] != L'0');
    return enabled != 0;
  }

  // 默认开启：这是诊断 build。若需临时关闭，在启动应用前设置 WEASEL_IME_TIMING=0。
  enabled = 1;
  return true;
}

inline void AppendLine(const char* line) {
  if (!Enabled() || !line) return;

  wchar_t path[MAX_PATH] = {0};
  DWORD n = GetTempPathW(MAX_PATH, path);
  if (n == 0 || n >= MAX_PATH) return;
  if (wcscat_s(path, MAX_PATH, L"weasel_ime_timing.log") != 0) return;

  HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return;

  DWORD written = 0;
  WriteFile(h, line, static_cast<DWORD>(strlen(line)), &written, NULL);
  CloseHandle(h);
}

inline void Logf(const char* tag, unsigned long long elapsed_us,
                 const char* fmt, ...) {
  if (!Enabled()) return;
  // 阈值过滤放在调用点：key_event 需要每次都记，子阶段只记慢调用。
  SYSTEMTIME st;
  GetLocalTime(&st);

  char detail[768] = {0};
  if (fmt) {
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(detail, sizeof(detail), _TRUNCATE, fmt, args);
    va_end(args);
  }

  char line[1024] = {0};
  sprintf_s(line, sizeof(line),
            "[%02u:%02u:%02u.%03u] pid=%lu tid=%lu tag=%s elapsed=%.3fms %s\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentProcessId(), GetCurrentThreadId(), tag ? tag : "",
            elapsed_us / 1000.0, detail);
  AppendLine(line);
}

class Scope {
 public:
  Scope(const char* tag, unsigned long long threshold_us, const char* detail = nullptr)
      : tag_(tag), threshold_us_(threshold_us), detail_(detail), start_us_(QpcUs()) {}
  ~Scope() {
    unsigned long long elapsed = QpcUs() - start_us_;
    if (elapsed >= threshold_us_) {
      Logf(tag_, elapsed, "%s", detail_ ? detail_ : "");
    }
  }

 private:
  const char* tag_;
  unsigned long long threshold_us_;
  const char* detail_;
  unsigned long long start_us_;
};

}  // namespace weasel_timing
