#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

inline long long RevarTraceNowUs() {
  static LARGE_INTEGER freq = []() {
    LARGE_INTEGER f = {};
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now = {};
  QueryPerformanceCounter(&now);
  if (freq.QuadPart <= 0)
    return GetTickCount64() * 1000LL;
  return static_cast<long long>((now.QuadPart * 1000000LL) / freq.QuadPart);
}

inline std::wstring RevarTraceRect(const RECT& rc) {
  std::wstringstream ss;
  ss << L"(" << rc.left << L"," << rc.top << L"," << rc.right << L","
     << rc.bottom << L")";
  return ss.str();
}

inline std::wstring RevarTraceLogPath() {
  WCHAR temp[MAX_PATH] = {0};
  DWORD len = GetTempPathW(ARRAYSIZE(temp), temp);
  if (len == 0 || len >= ARRAYSIZE(temp))
    return L"";
  std::wstring path(temp);
  path += L"revar_input_dev_trace.log";
  return path;
}

inline std::wstring RevarTraceConfigPath() {
  WCHAR appdata[MAX_PATH] = {0};
  DWORD len = GetEnvironmentVariableW(L"APPDATA", appdata, ARRAYSIZE(appdata));
  if (len == 0 || len >= ARRAYSIZE(appdata))
    return L"";
  std::wstring path(appdata);
  if (!path.empty() && path.back() != L'\\')
    path += L"\\";
  path += L"Rime\\revar_input.yaml";
  return path;
}

inline std::string RevarTraceTrim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

inline std::string RevarTraceToLower(std::string s) {
  s = RevarTraceTrim(s);
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

inline std::string RevarTraceValueAfterColon(const std::string& s) {
  auto pos = s.find(':');
  if (pos == std::string::npos)
    return "";
  return RevarTraceTrim(s.substr(pos + 1));
}

inline bool RevarTraceParseBool(const std::string& value, bool fallback) {
  std::string lower = RevarTraceToLower(value);
  if (lower == "true" || lower == "yes" || lower == "on" || lower == "1")
    return true;
  if (lower == "false" || lower == "no" || lower == "off" || lower == "0")
    return false;
  return fallback;
}

inline bool RevarTraceLoadEnabledFromConfig() {
  std::wstring path = RevarTraceConfigPath();
  if (path.empty())
    return false;
  std::ifstream in(path);
  if (!in)
    return false;

  bool in_debug = false;
  int debug_indent = -1;
  std::string line;
  while (std::getline(in, line)) {
    auto comment = line.find('#');
    if (comment != std::string::npos)
      line = line.substr(0, comment);
    std::string trimmed = RevarTraceTrim(line);
    if (trimmed.empty())
      continue;

    int indent = 0;
    while (indent < static_cast<int>(line.size()) &&
           (line[static_cast<size_t>(indent)] == ' ' ||
            line[static_cast<size_t>(indent)] == '\t')) {
      ++indent;
    }

    if (trimmed == "debug:") {
      in_debug = true;
      debug_indent = indent;
      continue;
    }
    if (in_debug && trimmed.back() == ':' && indent <= debug_indent) {
      in_debug = false;
      debug_indent = -1;
    }
    if (!in_debug)
      continue;

    if (trimmed.rfind("trace_enabled:", 0) == 0 ||
        trimmed.rfind("trace:", 0) == 0 ||
        trimmed.rfind("enabled:", 0) == 0) {
      return RevarTraceParseBool(RevarTraceValueAfterColon(trimmed), false);
    }
  }
  return false;
}

inline std::atomic<int>& RevarTraceEnabledState() {
  // -1 means not initialized; 0 disabled; 1 enabled.
  static std::atomic<int> enabled{-1};
  return enabled;
}

inline void RevarTraceSetEnabled(bool enabled) {
  RevarTraceEnabledState().store(enabled ? 1 : 0, std::memory_order_relaxed);
}

inline bool RevarTraceEnabled() {
  int value = RevarTraceEnabledState().load(std::memory_order_relaxed);
  if (value >= 0)
    return value != 0;
  bool enabled = RevarTraceLoadEnabledFromConfig();
  int expected = -1;
  RevarTraceEnabledState().compare_exchange_strong(
      expected, enabled ? 1 : 0, std::memory_order_relaxed);
  return RevarTraceEnabledState().load(std::memory_order_relaxed) != 0;
}

inline bool RevarTraceClearLogFile() {
  std::wstring path = RevarTraceLogPath();
  if (path.empty())
    return false;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  CloseHandle(h);
  return true;
}

inline std::wstring RevarTraceLogDirectory() {
  WCHAR temp[MAX_PATH] = {0};
  DWORD len = GetTempPathW(ARRAYSIZE(temp), temp);
  if (len == 0 || len >= ARRAYSIZE(temp))
    return L"";
  return std::wstring(temp);
}

inline void RevarTraceLog(const wchar_t* component, const std::wstring& msg) {
  if (!RevarTraceEnabled())
    return;

  std::wstring path = RevarTraceLogPath();
  if (path.empty())
    return;

  HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return;

  std::wstringstream line;
  line << RevarTraceNowUs() << L" pid=" << GetCurrentProcessId()
       << L" tid=" << GetCurrentThreadId() << L" "
       << (component ? component : L"trace") << L" " << msg << L"\r\n";
  std::wstring wide = line.str();
  int bytes_needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                         static_cast<int>(wide.size()), nullptr,
                                         0, nullptr, nullptr);
  if (bytes_needed > 0) {
    std::vector<char> bytes(static_cast<size_t>(bytes_needed));
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()), bytes.data(),
                        bytes_needed, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
              nullptr);
  }
  CloseHandle(h);
}

#define REVAR_TRACE_STREAM(component, expr)                                      \
  do {                                                                           \
    if (RevarTraceEnabled()) {                                                   \
      std::wstringstream _revar_trace_stream;                                    \
      _revar_trace_stream << expr;                                               \
      RevarTraceLog((component), _revar_trace_stream.str());                     \
    }                                                                            \
  } while (0)
