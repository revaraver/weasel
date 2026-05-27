#include "stdafx.h"
#include "WeaselIPC.h"
#include "WeaselTSF.h"
#include <KeyEvent.h>
#include "CandidateList.h"
#include <RevarDevTrace.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <shellapi.h>
#include <sstream>
#include <string>
#include <vector>

static weasel::KeyEvent prevKeyEvent;
static BOOL prevfEaten = FALSE;
static int keyCountToSimulate = 0;

namespace {
struct RevarHotkeySpec {
  UINT keycode = 0;
  UINT mask = 0;
};

void WriteRevarDebugLog(const std::wstring& line) {
  RevarTraceLog(L"key", line);
}

void SendRevarUnicodeTextForRaw(const std::wstring& text) {
  std::vector<INPUT> inputs;
  inputs.reserve(text.length() * 2);
  for (wchar_t ch : text) {
    INPUT down = {};
    down.type = INPUT_KEYBOARD;
    down.ki.wScan = ch;
    down.ki.dwFlags = KEYEVENTF_UNICODE;
    inputs.push_back(down);

    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    inputs.push_back(up);
  }
  if (!inputs.empty())
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

static constexpr ULONG_PTR REVAR_IME_COPYDATA_REPLACE_BEFORE_CARET =
    0x52564952;  // "RVIR" / ReVar IME Replace.

struct RevarImeReplaceBeforeCaretPayload {
  uint32_t version;
  uint32_t raw_length;
  uint32_t text_utf16_length;
};

bool SendRevarGodotDirectTextForKeySink(LONG rawLength,
                                        const std::wstring& text) {
  HWND hwnd = GetForegroundWindow();
  if (!hwnd)
    return false;

  std::vector<BYTE> payload(sizeof(RevarImeReplaceBeforeCaretPayload) +
                            text.length() * sizeof(wchar_t));
  auto* header =
      reinterpret_cast<RevarImeReplaceBeforeCaretPayload*>(payload.data());
  header->version = 1;
  header->raw_length = static_cast<uint32_t>(rawLength < 0 ? 0 : rawLength);
  header->text_utf16_length = static_cast<uint32_t>(text.length());
  if (!text.empty()) {
    memcpy(payload.data() + sizeof(RevarImeReplaceBeforeCaretPayload),
           text.data(), text.length() * sizeof(wchar_t));
  }

  COPYDATASTRUCT copy_data = {};
  copy_data.dwData = REVAR_IME_COPYDATA_REPLACE_BEFORE_CARET;
  copy_data.cbData = static_cast<DWORD>(payload.size());
  copy_data.lpData = payload.data();

  const long long t0 = RevarTraceNowUs();
  DWORD_PTR result = 0;
  LRESULT sent = SendMessageTimeoutW(hwnd, WM_COPYDATA, 0,
                                     reinterpret_cast<LPARAM>(&copy_data),
                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 50,
                                     &result);
  const long long t1 = RevarTraceNowUs();
  if (RevarTraceEnabled()) {
    std::wstringstream dbg;
    dbg << L"godot_direct_raw send sent=" << sent << L" result=" << result
        << L" hwnd=0x" << std::hex << reinterpret_cast<uintptr_t>(hwnd)
        << std::dec << L" raw_len=" << rawLength << L" text=" << text
        << L" us_send=" << (t1 - t0);
    WriteRevarDebugLog(dbg.str());
  }
  return sent != 0 && result == TRUE;
}

std::string Trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string NormalizeToken(std::string s) {
  s = Trim(s);
  s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
            return std::isspace(c);
          }),
          s.end());
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return s;
}

std::string ToLower(std::string s) {
  s = Trim(s);
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

bool ParseBool(std::string s, bool fallback = false) {
  s = ToLower(s);
  if (s == "true" || s == "yes" || s == "on" || s == "1")
    return true;
  if (s == "false" || s == "no" || s == "off" || s == "0")
    return false;
  return fallback;
}

std::string ValueAfterColon(const std::string& s) {
  auto pos = s.find(':');
  if (pos == std::string::npos)
    return "";
  return Trim(s.substr(pos + 1));
}

bool ParseHotkeySpec(const std::string& text, RevarHotkeySpec* out) {
  if (!out)
    return false;
  RevarHotkeySpec spec;
  std::stringstream ss(text);
  std::string part;
  bool has_key = false;
  while (std::getline(ss, part, '+')) {
    part = NormalizeToken(part);
    if (part.empty())
      continue;
    if (part == "CTRL" || part == "CONTROL") {
      spec.mask |= ibus::CONTROL_MASK;
    } else if (part == "SHIFT") {
      spec.mask |= ibus::SHIFT_MASK;
    } else if (part == "ALT" || part == "OPTION") {
      spec.mask |= ibus::ALT_MASK;
    } else if (part.size() >= 2 && part[0] == 'F') {
      int n = std::atoi(part.c_str() + 1);
      if (n < 1 || n > 35)
        return false;
      spec.keycode = ibus::F1 + static_cast<UINT>(n - 1);
      has_key = true;
    } else {
      return false;
    }
  }
  if (!has_key)
    return false;
  *out = spec;
  return true;
}

std::filesystem::path RevarInputConfigPath() {
  WCHAR appdata[MAX_PATH] = {0};
  DWORD len = GetEnvironmentVariableW(L"APPDATA", appdata, ARRAYSIZE(appdata));
  if (len == 0 || len >= ARRAYSIZE(appdata))
    return std::filesystem::path();
  return std::filesystem::path(appdata) / L"Rime" / L"revar_input.yaml";
}

std::string StripQuotes(std::string s) {
  s = Trim(s);
  if (s.length() >= 2 &&
      ((s.front() == '"' && s.back() == '"') ||
       (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.length() - 2);
  }
  return s;
}

std::wstring ToLowerWide(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return s;
}

std::wstring NormalizeHostPath(std::wstring path) {
  std::replace(path.begin(), path.end(), L'/', L'\\');
  std::wstring collapsed;
  collapsed.reserve(path.size());
  bool last_backslash = false;
  for (wchar_t ch : path) {
    if (ch == L'\\') {
      if (!last_backslash)
        collapsed.push_back(ch);
      last_backslash = true;
    } else {
      collapsed.push_back(ch);
      last_backslash = false;
    }
  }
  return ToLowerWide(collapsed);
}

std::wstring Utf8ishToWide(const std::string& s) {
  if (s.empty())
    return L"";
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (len <= 0)
    return std::wstring(s.begin(), s.end());
  std::wstring out(static_cast<size_t>(len - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
  return out;
}

std::vector<std::wstring> LoadExactHostPathList(const std::string& list_name) {
  std::vector<std::wstring> result;
  std::ifstream in(RevarInputConfigPath());
  bool in_list = false;
  std::string line;
  while (std::getline(in, line)) {
    auto comment = line.find('#');
    if (comment != std::string::npos)
      line = line.substr(0, comment);
    std::string trimmed = Trim(line);
    if (trimmed.empty())
      continue;
    if (trimmed == list_name + ":") {
      in_list = true;
      continue;
    }
    if (in_list && trimmed[0] == '-') {
      std::string item = StripQuotes(Trim(trimmed.substr(1)));
      if (!item.empty())
        result.push_back(NormalizeHostPath(Utf8ishToWide(item)));
      continue;
    }
    if (in_list && trimmed.find(':') != std::string::npos &&
        trimmed[0] != '-') {
      in_list = false;
    }
  }
  return result;
}

bool IsPathInList(const std::wstring& path,
                  const std::vector<std::wstring>& paths) {
  for (const auto& item : paths) {
    if (path == item)
      return true;
  }
  return false;
}

std::wstring GetForegroundProcessPathLowerForKeySink() {
  HWND hwnd = GetForegroundWindow();
  if (!hwnd)
    return L"";
  static HWND cached_hwnd = nullptr;
  static DWORD cached_pid = 0;
  static std::wstring cached_path;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid)
    return L"";
  if (hwnd == cached_hwnd && pid == cached_pid)
    return cached_path;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return L"";
  std::vector<WCHAR> path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  BOOL ok = QueryFullProcessImageNameW(process, 0, path.data(), &size);
  CloseHandle(process);
  if (!ok)
    return L"";
  cached_hwnd = hwnd;
  cached_pid = pid;
  cached_path.assign(path.data(), size);
  cached_path = NormalizeHostPath(cached_path);
  return cached_path;
}

bool ShouldUseSendInputRawForForegroundHost(std::wstring* path_out) {
  std::wstring path = GetForegroundProcessPathLowerForKeySink();
  if (path_out)
    *path_out = path;
  if (path.empty())
    return false;
  const auto raw_paths = LoadExactHostPathList("raw_unicode_exact_paths");
  const auto backspace_paths = LoadExactHostPathList("backspace_unicode_exact_paths");
  return IsPathInList(path, raw_paths) || IsPathInList(path, backspace_paths);
}

bool ShouldUseDirectRawForForegroundHost(std::wstring* path_out) {
  std::wstring path = GetForegroundProcessPathLowerForKeySink();
  if (path_out)
    *path_out = path;
  if (path.empty())
    return false;
  const auto direct_paths = LoadExactHostPathList("direct_replace_exact_paths");
  return IsPathInList(path, direct_paths);
}

std::vector<RevarHotkeySpec> LoadDetachHotkeys() {
  std::vector<RevarHotkeySpec> result;
  auto path = RevarInputConfigPath();
  std::ifstream in(path);
  bool in_detach = false;
  std::string line;
  while (std::getline(in, line)) {
    auto comment = line.find('#');
    if (comment != std::string::npos)
      line = line.substr(0, comment);
    std::string trimmed = Trim(line);
    if (trimmed.empty())
      continue;
    if (trimmed == "detach_shadow_buffer:" ||
        trimmed == "clear_shadow_buffer_keys:") {
      in_detach = true;
      continue;
    }
    if (!in_detach)
      continue;
    if (trimmed[0] == '-') {
      std::string key = Trim(trimmed.substr(1));
      RevarHotkeySpec spec;
      if (ParseHotkeySpec(key, &spec))
        result.push_back(spec);
      continue;
    }
    if (trimmed.find(':') != std::string::npos)
      in_detach = false;
  }

  if (result.empty()) {
    RevarHotkeySpec fallback;
    ParseHotkeySpec("F1", &fallback);
    result.push_back(fallback);
  }
  return result;
}

const std::vector<RevarHotkeySpec>& DetachHotkeys() {
  static const std::vector<RevarHotkeySpec> hotkeys = LoadDetachHotkeys();
  return hotkeys;
}

bool IsDetachHotkey(const weasel::KeyEvent& ke) {
  constexpr UINT kRelevantMask =
      ibus::CONTROL_MASK | ibus::SHIFT_MASK | ibus::ALT_MASK;
  for (const auto& hotkey : DetachHotkeys()) {
    if (ke.keycode == hotkey.keycode &&
        (ke.mask & kRelevantMask) == hotkey.mask) {
      return true;
    }
  }
  return false;
}

bool LoadTransparentModeEnabled() {
  auto path = RevarInputConfigPath();
  std::ifstream in(path);
  bool default_mode_transparent = false;
  bool transparent_feature_enabled = true;
  bool in_transparent_mode = false;
  std::string line;
  while (std::getline(in, line)) {
    auto comment = line.find('#');
    if (comment != std::string::npos)
      line = line.substr(0, comment);
    std::string trimmed = Trim(line);
    if (trimmed.empty())
      continue;

    if (trimmed.rfind("default_mode:", 0) == 0) {
      default_mode_transparent = ToLower(ValueAfterColon(trimmed)) == "transparent";
      continue;
    }
    if (trimmed == "transparent_mode:") {
      in_transparent_mode = true;
      continue;
    }
    if (trimmed.find(':') != std::string::npos && trimmed[0] != '-' &&
        trimmed != "enabled:" && trimmed.rfind("enabled:", 0) != 0 &&
        trimmed.find(" ") == std::string::npos) {
      in_transparent_mode = false;
    }
    if (in_transparent_mode && trimmed.rfind("enabled:", 0) == 0) {
      transparent_feature_enabled = ParseBool(ValueAfterColon(trimmed), true);
      continue;
    }
  }
  return transparent_feature_enabled && default_mode_transparent;
}

bool IsCodeModeEnabledForForegroundHost(std::wstring* path_out = nullptr) {
  std::wstring path = GetForegroundProcessPathLowerForKeySink();
  if (path_out)
    *path_out = path;
  if (path.empty())
    return false;
  const auto disabled_paths = LoadExactHostPathList("disabled_exact_paths");
  if (IsPathInList(path, disabled_paths))
    return false;
  const auto enabled_paths = LoadExactHostPathList("enabled_exact_paths");
  return IsPathInList(path, enabled_paths);
}

bool IsPlainAsciiLetterKey(const weasel::KeyEvent& ke) {
  constexpr UINT kBlockMask = ibus::CONTROL_MASK | ibus::ALT_MASK |
                              ibus::META_MASK | ibus::SUPER_MASK |
                              ibus::HYPER_MASK | ibus::RELEASE_MASK;
  if (ke.mask & kBlockMask)
    return false;
  return (ke.keycode >= 'a' && ke.keycode <= 'z') ||
         (ke.keycode >= 'A' && ke.keycode <= 'Z');
}

bool IsAsciiDigitKey(const weasel::KeyEvent& ke) {
  return ke.keycode >= '0' && ke.keycode <= '9';
}

bool IsTransparentCandidateKey(const weasel::KeyEvent& ke) {
  if (ke.mask & ibus::RELEASE_MASK)
    return false;
  return ke.keycode == ibus::space || ke.keycode == ibus::Return ||
         ke.keycode == ibus::BackSpace || IsAsciiDigitKey(ke) ||
         ke.keycode == ibus::Escape || ke.keycode == ibus::Tab ||
         ke.keycode == ibus::Left || ke.keycode == ibus::Right ||
         ke.keycode == ibus::Up || ke.keycode == ibus::Down ||
         ke.keycode == ibus::Prior || ke.keycode == ibus::Next;
}

bool IsRevarMenuHotkey(const weasel::KeyEvent& ke) {
  constexpr UINT kRelevantMask =
      ibus::CONTROL_MASK | ibus::SHIFT_MASK | ibus::ALT_MASK;
  return ke.keycode == ibus::F10 &&
         (ke.mask & kRelevantMask) == ibus::CONTROL_MASK;
}

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty())
    return "";
  int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
  if (len <= 0)
    return "";
  std::string out(static_cast<size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), len, nullptr,
                      nullptr);
  return out;
}

std::string EscapeYamlDoubleQuotedPath(const std::wstring& path) {
  std::string utf8 = WideToUtf8(path);
  std::string out;
  out.reserve(utf8.size() + 16);
  for (char ch : utf8) {
    if (ch == '\\')
      out += "\\\\";
    else if (ch == '"')
      out += "\\\"";
    else
      out.push_back(ch);
  }
  return out;
}

std::wstring RevarMenuPathLabel(const std::wstring& path) {
  constexpr size_t kMax = 96;
  if (path.length() <= kMax)
    return path;
  return path.substr(0, 20) + L"..." + path.substr(path.length() - 70);
}

struct RevarCandidateOverrideConfig {
  std::wstring path;
  std::string position = "auto";
  int gap = 18;
  int x_offset = 0;
  int y_offset = 0;
};

struct RevarMenuConfig {
  std::vector<std::wstring> code_enabled;
  std::vector<std::wstring> code_disabled;
  std::vector<std::wstring> tsf_replace;
  std::vector<std::wstring> raw_unicode;
  std::vector<std::wstring> direct_replace;
  std::vector<std::wstring> backspace_unicode;
  std::vector<RevarCandidateOverrideConfig> candidate_overrides;
  bool debug_trace_enabled = false;
};

bool SaveRevarMenuConfig(const RevarMenuConfig& cfg);

bool PathInVector(const std::vector<std::wstring>& paths,
                  const std::wstring& path) {
  for (const auto& item : paths) {
    if (item == path)
      return true;
  }
  return false;
}

void RemovePathFromVector(std::vector<std::wstring>* paths,
                          const std::wstring& path) {
  if (!paths)
    return;
  paths->erase(std::remove(paths->begin(), paths->end(), path), paths->end());
}

void AddPathToVector(std::vector<std::wstring>* paths,
                     const std::wstring& path) {
  if (!paths || path.empty())
    return;
  if (!PathInVector(*paths, path))
    paths->push_back(path);
}

std::string NormalizePositionName(const std::string& value) {
  std::string lower = ToLower(value);
  if (lower == "above" || lower == "top" || lower == "上" || lower == "上方")
    return "above";
  if (lower == "below" || lower == "bottom" || lower == "下" ||
      lower == "下方")
    return "below";
  return "auto";
}

std::vector<RevarCandidateOverrideConfig> LoadCandidateOverrides() {
  std::vector<RevarCandidateOverrideConfig> result;
  std::ifstream in(RevarInputConfigPath());
  bool in_candidate_window = false;
  bool in_overrides = false;
  bool have_current = false;
  RevarCandidateOverrideConfig current;
  std::string line;
  while (std::getline(in, line)) {
    auto comment = line.find('#');
    if (comment != std::string::npos)
      line = line.substr(0, comment);
    std::string trimmed = Trim(line);
    if (trimmed.empty())
      continue;
    if (trimmed == "candidate_window:") {
      in_candidate_window = true;
      in_overrides = false;
      continue;
    }
    if (!in_candidate_window)
      continue;
    if (trimmed == "exact_path_overrides:" || trimmed == "per_exe:") {
      in_overrides = true;
      continue;
    }
    if (in_overrides && trimmed.rfind("- path:", 0) == 0) {
      if (have_current && !current.path.empty())
        result.push_back(current);
      current = RevarCandidateOverrideConfig();
      have_current = true;
      current.path = NormalizeHostPath(
          Utf8ishToWide(StripQuotes(ValueAfterColon(trimmed.substr(1)))));
      continue;
    }
    if (!in_overrides || !have_current)
      continue;
    if (trimmed.rfind("position:", 0) == 0) {
      current.position = NormalizePositionName(ValueAfterColon(trimmed));
      continue;
    }
    if (trimmed.rfind("gap:", 0) == 0) {
      current.gap = std::atoi(ValueAfterColon(trimmed).c_str());
      continue;
    }
    if (trimmed.rfind("x_offset:", 0) == 0) {
      current.x_offset = std::atoi(ValueAfterColon(trimmed).c_str());
      continue;
    }
    if (trimmed.rfind("y_offset:", 0) == 0) {
      current.y_offset = std::atoi(ValueAfterColon(trimmed).c_str());
      continue;
    }
  }
  if (have_current && !current.path.empty())
    result.push_back(current);
  return result;
}

RevarMenuConfig LoadRevarMenuConfig() {
  RevarMenuConfig cfg;
  cfg.code_enabled = LoadExactHostPathList("enabled_exact_paths");
  cfg.code_disabled = LoadExactHostPathList("disabled_exact_paths");
  cfg.tsf_replace = LoadExactHostPathList("tsf_replace_exact_paths");
  cfg.raw_unicode = LoadExactHostPathList("raw_unicode_exact_paths");
  cfg.direct_replace = LoadExactHostPathList("direct_replace_exact_paths");
  cfg.backspace_unicode = LoadExactHostPathList("backspace_unicode_exact_paths");
  // 兼容旧配置：以前“TSF 模式”是隐式的，即 enabled 但不在特殊 host policy 里。
  for (const auto& path : cfg.code_enabled) {
    if (!PathInVector(cfg.code_disabled, path) &&
        !PathInVector(cfg.raw_unicode, path) &&
        !PathInVector(cfg.direct_replace, path) &&
        !PathInVector(cfg.backspace_unicode, path)) {
      AddPathToVector(&cfg.tsf_replace, path);
    }
  }
  cfg.candidate_overrides = LoadCandidateOverrides();
  cfg.debug_trace_enabled = RevarTraceLoadEnabledFromConfig();
  RevarTraceSetEnabled(cfg.debug_trace_enabled);
  return cfg;
}

RevarCandidateOverrideConfig* FindCandidateOverride(
    std::vector<RevarCandidateOverrideConfig>* overrides,
    const std::wstring& path) {
  if (!overrides)
    return nullptr;
  for (auto& item : *overrides) {
    if (item.path == path)
      return &item;
  }
  return nullptr;
}

void RemoveCandidateOverride(std::vector<RevarCandidateOverrideConfig>* overrides,
                             const std::wstring& path) {
  if (!overrides)
    return;
  overrides->erase(
      std::remove_if(overrides->begin(), overrides->end(),
                     [&](const RevarCandidateOverrideConfig& item) {
                       return item.path == path;
                     }),
      overrides->end());
}

RevarCandidateOverrideConfig& EnsureCandidateOverride(RevarMenuConfig* cfg,
                                                       const std::wstring& path) {
  auto* found = FindCandidateOverride(&cfg->candidate_overrides, path);
  if (found)
    return *found;
  RevarCandidateOverrideConfig item;
  item.path = path;
  cfg->candidate_overrides.push_back(item);
  return cfg->candidate_overrides.back();
}

int PositionNameToAdjustInt(const std::string& position) {
  std::string normalized = NormalizePositionName(position);
  if (normalized == "above")
    return 1;
  if (normalized == "below")
    return 2;
  return 0;
}

std::string AdjustIntToPositionName(int position) {
  if (position == 1)
    return "above";
  if (position == 2)
    return "below";
  return "auto";
}

RevarCandidateOverrideConfig GetEffectiveCandidateOverride(
    RevarMenuConfig* cfg,
    const std::wstring& path) {
  RevarCandidateOverrideConfig result;
  result.path = path;
  auto* found = FindCandidateOverride(&cfg->candidate_overrides, path);
  if (found)
    result = *found;
  result.position = NormalizePositionName(result.position);
  return result;
}

bool SaveCandidateAdjustValues(const std::wstring& path,
                               int position,
                               int gap,
                               int x_offset,
                               int y_offset) {
  RevarMenuConfig cfg = LoadRevarMenuConfig();
  auto& item = EnsureCandidateOverride(&cfg, path);
  item.position = AdjustIntToPositionName(position);
  item.gap = gap;
  item.x_offset = x_offset;
  item.y_offset = y_offset;
  return SaveRevarMenuConfig(cfg);
}

void WriteYamlPathList(std::ofstream& out,
                       const std::string& name,
                       const std::vector<std::wstring>& paths) {
  out << "    " << name << ":\n";
  for (const auto& path : paths) {
    out << "      - \"" << EscapeYamlDoubleQuotedPath(path) << "\"\n";
  }
}

bool SaveRevarMenuConfig(const RevarMenuConfig& cfg) {
  std::ofstream out(RevarInputConfigPath(), std::ios::trunc);
  if (!out)
    return false;
  out << "# revar 输入法专属配置\n";
  out << "# 路径：%APPDATA%\\Rime\\revar_input.yaml\n";
  out << "# Ctrl+F10 可按当前 EXE 写入/更新下面的 code_mode、candidate_window、host_policies。\n\n";
  out << "revar_input:\n";
  out << "  default_mode: compatible\n\n";
  out << "  hotkeys:\n";
  out << "    detach_shadow_buffer:\n";
  out << "      - F1\n";
  out << "    toggle_compatible_mode:\n";
  out << "      - F2\n";
  out << "    open_revar_menu:\n";
  out << "      - Control+F10\n\n";
  out << "  code_mode:\n";
  WriteYamlPathList(out, "enabled_exact_paths", cfg.code_enabled);
  WriteYamlPathList(out, "disabled_exact_paths", cfg.code_disabled);
  out << "\n";
  out << "  transparent_mode:\n";
  out << "    enabled: true\n";
  out << "    clear_shadow_buffer_keys:\n";
  out << "      - F1\n";
  out << "    left_space_behavior: confirm_candidate\n";
  out << "    right_space_behavior: commit_raw_and_space\n\n";
  out << "  candidate_window:\n";
  out << "    # 只在 code/transparent mode 生效；compatible mode 下保持小狼毫原版候选窗位置。\n";
  out << "    position: auto\n";
  out << "    gap: 18\n";
  out << "    x_offset: 0\n";
  out << "    y_offset: 0\n";
  out << "    exact_path_overrides:\n";
  for (const auto& item : cfg.candidate_overrides) {
    out << "      - path: \"" << EscapeYamlDoubleQuotedPath(item.path) << "\"\n";
    out << "        position: " << NormalizePositionName(item.position) << "\n";
    out << "        gap: " << item.gap << "\n";
    out << "        x_offset: " << item.x_offset << "\n";
    out << "        y_offset: " << item.y_offset << "\n";
  }
  out << "\n";
  out << "  host_policies:\n";
  out << "    # tsf_replace_exact_paths 是显式展示用；实际 TSF 是 code mode 的默认替换策略。\n";
  WriteYamlPathList(out, "tsf_replace_exact_paths", cfg.tsf_replace);
  WriteYamlPathList(out, "raw_unicode_exact_paths", cfg.raw_unicode);
  WriteYamlPathList(out, "direct_replace_exact_paths", cfg.direct_replace);
  WriteYamlPathList(out, "backspace_unicode_exact_paths", cfg.backspace_unicode);
  out << "\n";
  out << "  debug:\n";
  out << "    # Ctrl+F10 可切换；关闭时热路径不写日志。\n";
  out << "    trace_enabled: " << (cfg.debug_trace_enabled ? "true" : "false") << "\n";
  out << "\n";
  out << "  compatible_mode:\n";
  out << "    enabled: true\n";
  return true;
}

void SetCurrentExeCompatible(RevarMenuConfig* cfg, const std::wstring& path) {
  RemovePathFromVector(&cfg->code_enabled, path);
  AddPathToVector(&cfg->code_disabled, path);
  RemovePathFromVector(&cfg->tsf_replace, path);
  RemovePathFromVector(&cfg->raw_unicode, path);
  RemovePathFromVector(&cfg->direct_replace, path);
  RemovePathFromVector(&cfg->backspace_unicode, path);
}

void SetCurrentExeCodeTsf(RevarMenuConfig* cfg, const std::wstring& path) {
  AddPathToVector(&cfg->code_enabled, path);
  RemovePathFromVector(&cfg->code_disabled, path);
  AddPathToVector(&cfg->tsf_replace, path);
  RemovePathFromVector(&cfg->raw_unicode, path);
  RemovePathFromVector(&cfg->direct_replace, path);
  RemovePathFromVector(&cfg->backspace_unicode, path);
}

void SetCurrentExeCodeRaw(RevarMenuConfig* cfg, const std::wstring& path) {
  SetCurrentExeCodeTsf(cfg, path);
  RemovePathFromVector(&cfg->tsf_replace, path);
  AddPathToVector(&cfg->raw_unicode, path);
}

void SetCurrentExeCodeDirect(RevarMenuConfig* cfg, const std::wstring& path) {
  SetCurrentExeCodeTsf(cfg, path);
  RemovePathFromVector(&cfg->tsf_replace, path);
  AddPathToVector(&cfg->direct_replace, path);
}

void SetCurrentExeCodeBackspace(RevarMenuConfig* cfg, const std::wstring& path) {
  SetCurrentExeCodeTsf(cfg, path);
  RemovePathFromVector(&cfg->tsf_replace, path);
  AddPathToVector(&cfg->backspace_unicode, path);
}

void ClearCurrentExeMemory(RevarMenuConfig* cfg, const std::wstring& path) {
  RemovePathFromVector(&cfg->code_enabled, path);
  RemovePathFromVector(&cfg->code_disabled, path);
  RemovePathFromVector(&cfg->tsf_replace, path);
  RemovePathFromVector(&cfg->raw_unicode, path);
  RemovePathFromVector(&cfg->direct_replace, path);
  RemovePathFromVector(&cfg->backspace_unicode, path);
  RemoveCandidateOverride(&cfg->candidate_overrides, path);
}

POINT GetRevarMenuPopupPoint(HWND hwnd) {
  POINT pt = {0, 0};
  DWORD tid = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : 0;
  GUITHREADINFO gi = {sizeof(GUITHREADINFO)};
  if (tid && GetGUIThreadInfo(tid, &gi) && gi.hwndCaret) {
    pt.x = gi.rcCaret.left;
    pt.y = gi.rcCaret.bottom;
    ClientToScreen(gi.hwndCaret, &pt);
    return pt;
  }
  if (GetCursorPos(&pt))
    return pt;
  RECT rc = {0, 0, 0, 0};
  if (hwnd && GetWindowRect(hwnd, &rc)) {
    pt.x = rc.left + 32;
    pt.y = rc.top + 32;
  }
  return pt;
}

constexpr UINT IDM_REVAR_COMPATIBLE = 9101;
constexpr UINT IDM_REVAR_CODE_TSF = 9102;
constexpr UINT IDM_REVAR_CODE_RAW = 9103;
constexpr UINT IDM_REVAR_CODE_DIRECT = 9104;
constexpr UINT IDM_REVAR_CODE_BACKSPACE = 9105;
constexpr UINT IDM_REVAR_POS_AUTO = 9121;
constexpr UINT IDM_REVAR_POS_ABOVE = 9122;
constexpr UINT IDM_REVAR_POS_BELOW = 9123;
constexpr UINT IDM_REVAR_POS_ADJUST = 9124;
constexpr UINT IDM_REVAR_POS_RESET = 9125;
constexpr UINT IDM_REVAR_DEBUG_TRACE_TOGGLE = 9181;
constexpr UINT IDM_REVAR_DEBUG_CLEAR_LOG = 9182;
constexpr UINT IDM_REVAR_DEBUG_OPEN_LOG_DIR = 9183;
constexpr UINT IDM_REVAR_CLEAR = 9199;

constexpr int IDC_REVAR_ADJUST_X = 9301;
constexpr int IDC_REVAR_ADJUST_Y = 9302;
constexpr int IDC_REVAR_ADJUST_GAP = 9303;
constexpr int IDC_REVAR_ADJUST_DEFAULT = 9304;

struct RevarCandidateAdjustDialogState {
  std::wstring path;
  int x_offset = 0;
  int y_offset = 0;
  int gap = 18;
  bool accepted = false;
  bool done = false;
};

void SetControlFont(HWND hwnd) {
  HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  if (font)
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND CreateAdjustChild(HWND parent,
                       const wchar_t* cls,
                       const wchar_t* text,
                       DWORD style,
                       int x,
                       int y,
                       int w,
                       int h,
                       int id) {
  HWND child = CreateWindowExW(0,
                               cls,
                               text,
                               WS_CHILD | WS_VISIBLE | style,
                               x,
                               y,
                               w,
                               h,
                               parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr),
                               nullptr);
  SetControlFont(child);
  return child;
}

bool ParseIntEdit(HWND hwnd, int id, int* out) {
  wchar_t buf[64] = {};
  GetDlgItemTextW(hwnd, id, buf, ARRAYSIZE(buf));
  wchar_t* begin = buf;
  while (*begin && iswspace(*begin))
    ++begin;
  wchar_t* end = nullptr;
  long value = wcstol(begin, &end, 10);
  while (end && *end && iswspace(*end))
    ++end;
  if (!begin[0] || !end || *end)
    return false;
  *out = static_cast<int>(value);
  return true;
}

LRESULT CALLBACK RevarCandidateAdjustDialogProc(HWND hwnd,
                                                UINT msg,
                                                WPARAM wParam,
                                                LPARAM lParam) {
  auto* state = reinterpret_cast<RevarCandidateAdjustDialogState*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_CREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
      state = reinterpret_cast<RevarCandidateAdjustDialogState*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
      SetControlFont(hwnd);

      CreateAdjustChild(hwnd, L"STATIC", L"当前 EXE：", 0, 24, 20, 100, 24, -1);
      CreateAdjustChild(hwnd,
                        L"STATIC",
                        state->path.c_str(),
                        0,
                        24,
                        48,
                        650,
                        26,
                        -1);

      CreateAdjustChild(hwnd, L"STATIC", L"x_offset", 0, 48, 98, 110, 26, -1);
      CreateAdjustChild(hwnd, L"STATIC", L"y_offset", 0, 48, 138, 110, 26, -1);
      CreateAdjustChild(hwnd, L"STATIC", L"gap", 0, 48, 178, 110, 26, -1);

      HWND x_edit = CreateAdjustChild(hwnd,
                                      L"EDIT",
                                      std::to_wstring(state->x_offset).c_str(),
                                      WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                                      170,
                                      94,
                                      180,
                                      28,
                                      IDC_REVAR_ADJUST_X);
      CreateAdjustChild(hwnd,
                        L"EDIT",
                        std::to_wstring(state->y_offset).c_str(),
                        WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                        170,
                        134,
                        180,
                        28,
                        IDC_REVAR_ADJUST_Y);
      CreateAdjustChild(hwnd,
                        L"EDIT",
                        std::to_wstring(state->gap).c_str(),
                        WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                        170,
                        174,
                        180,
                        28,
                        IDC_REVAR_ADJUST_GAP);

      CreateAdjustChild(hwnd,
                        L"STATIC",
                        L"提示：修改后点“确定”写入当前 EXE；“恢复默认”只填默认值，不会自动保存。",
                        0,
                        24,
                        222,
                        650,
                        26,
                        -1);
      CreateAdjustChild(hwnd,
                        L"BUTTON",
                        L"确定",
                        WS_TABSTOP | BS_DEFPUSHBUTTON,
                        368,
                        270,
                        86,
                        32,
                        IDOK);
      CreateAdjustChild(hwnd,
                        L"BUTTON",
                        L"取消",
                        WS_TABSTOP,
                        466,
                        270,
                        86,
                        32,
                        IDCANCEL);
      CreateAdjustChild(hwnd,
                        L"BUTTON",
                        L"恢复默认",
                        WS_TABSTOP,
                        564,
                        270,
                        108,
                        32,
                        IDC_REVAR_ADJUST_DEFAULT);
      SendMessageW(x_edit, EM_SETSEL, 0, -1);
      SetFocus(x_edit);
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      if (id == IDC_REVAR_ADJUST_DEFAULT) {
        SetDlgItemInt(hwnd, IDC_REVAR_ADJUST_X, 0, TRUE);
        SetDlgItemInt(hwnd, IDC_REVAR_ADJUST_Y, 0, TRUE);
        SetDlgItemInt(hwnd, IDC_REVAR_ADJUST_GAP, 18, TRUE);
        SetFocus(GetDlgItem(hwnd, IDC_REVAR_ADJUST_X));
        return 0;
      }
      if (id == IDOK) {
        int x = 0, y = 0, gap = 18;
        if (!ParseIntEdit(hwnd, IDC_REVAR_ADJUST_X, &x) ||
            !ParseIntEdit(hwnd, IDC_REVAR_ADJUST_Y, &y) ||
            !ParseIntEdit(hwnd, IDC_REVAR_ADJUST_GAP, &gap)) {
          MessageBoxW(hwnd,
                      L"x_offset / y_offset / gap 都必须是整数。",
                      L"ReVar 候选窗位置",
                      MB_ICONWARNING | MB_OK);
          return 0;
        }
        if (gap < 0)
          gap = 0;
        state->x_offset = x;
        state->y_offset = y;
        state->gap = gap;
        state->accepted = true;
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
      }
      if (id == IDCANCEL) {
        state->accepted = false;
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    }
    case WM_CLOSE:
      if (state) {
        state->accepted = false;
        state->done = true;
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (state)
        state->done = true;
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowRevarCandidateAdjustDialog(HWND owner,
                                    const std::wstring& path,
                                    int* x_offset,
                                    int* y_offset,
                                    int* gap) {
  static ATOM cls = 0;
  if (!cls) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = RevarCandidateAdjustDialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"RevarCandidateAdjustDialog";
    cls = RegisterClassExW(&wc);
  }
  if (!cls)
    return false;

  RevarCandidateAdjustDialogState state;
  state.path = path;
  state.x_offset = x_offset ? *x_offset : 0;
  state.y_offset = y_offset ? *y_offset : 0;
  state.gap = gap ? *gap : 18;

  const int width = 720;
  const int height = 360;
  RECT area = {0, 0, 0, 0};
  HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof(mi)};
  if (monitor && GetMonitorInfoW(monitor, &mi)) {
    area = mi.rcWork;
  } else {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
  }
  int x = area.left + ((area.right - area.left) - width) / 2;
  int y = area.top + ((area.bottom - area.top) - height) / 2;

  HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
                              L"RevarCandidateAdjustDialog",
                              L"ReVar 候选窗位置",
                              WS_CAPTION | WS_SYSMENU | WS_POPUP,
                              x,
                              y,
                              width,
                              height,
                              owner,
                              nullptr,
                              GetModuleHandleW(nullptr),
                              &state);
  if (!hwnd)
    return false;

  if (owner)
    EnableWindow(owner, FALSE);
  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);

  MSG msg;
  while (!state.done && IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  if (owner) {
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
  }
  if (state.accepted) {
    if (x_offset)
      *x_offset = state.x_offset;
    if (y_offset)
      *y_offset = state.y_offset;
    if (gap)
      *gap = state.gap;
  }
  return state.accepted;
}

void AppendCheckedMenuItem(HMENU menu,
                           UINT id,
                           const wchar_t* label,
                           bool checked,
                           bool enabled = true) {
  AppendMenuW(menu,
              MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED) |
                  (enabled ? 0 : MF_DISABLED),
              id, label);
}

UINT ShowRevarPopupMenu(const std::wstring& path, const RevarMenuConfig& cfg) {
  HWND hwnd = GetForegroundWindow();
  HMENU root = CreatePopupMenu();
  HMENU mode = CreatePopupMenu();
  HMENU position = CreatePopupMenu();
  HMENU debug = CreatePopupMenu();
  if (!root || !mode || !position || !debug)
    return 0;

  const bool code_enabled = PathInVector(cfg.code_enabled, path) &&
                            !PathInVector(cfg.code_disabled, path);
  const bool direct = PathInVector(cfg.direct_replace, path);
  const bool backspace = PathInVector(cfg.backspace_unicode, path);
  const bool raw = PathInVector(cfg.raw_unicode, path);
  const auto* override_config =
      FindCandidateOverride(const_cast<std::vector<RevarCandidateOverrideConfig>*>(
                                &cfg.candidate_overrides),
                            path);
  std::string pos = override_config ? NormalizePositionName(override_config->position)
                                    : "auto";

  std::wstring title = L"当前 EXE: " + RevarMenuPathLabel(path);
  AppendMenuW(root, MF_STRING | MF_DISABLED, 0, title.c_str());
  AppendMenuW(root, MF_SEPARATOR, 0, nullptr);

  AppendCheckedMenuItem(mode, IDM_REVAR_COMPATIBLE, L"正常模式 / 小狼毫兼容", !code_enabled);
  AppendCheckedMenuItem(mode, IDM_REVAR_CODE_TSF, L"代码模式：TSF 插入/替换", code_enabled && !raw && !direct && !backspace);
  AppendCheckedMenuItem(mode, IDM_REVAR_CODE_RAW, L"代码模式：SendInput raw + TSF 替换", code_enabled && raw);
  AppendCheckedMenuItem(mode, IDM_REVAR_CODE_DIRECT, L"代码模式：RVIR/direct replace", code_enabled && direct);
  AppendCheckedMenuItem(mode, IDM_REVAR_CODE_BACKSPACE, L"代码模式：模拟 Backspace 替换", code_enabled && backspace);
  AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(mode), L"当前 EXE 模式/替换策略");

  AppendCheckedMenuItem(position, IDM_REVAR_POS_AUTO, L"候选窗：auto", pos == "auto", code_enabled);
  AppendCheckedMenuItem(position, IDM_REVAR_POS_ABOVE, L"候选窗：上方", pos == "above", code_enabled);
  AppendCheckedMenuItem(position, IDM_REVAR_POS_BELOW, L"候选窗：下方", pos == "below", code_enabled);
  AppendMenuW(position, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(position,
              MF_STRING | (code_enabled ? 0 : MF_DISABLED),
              IDM_REVAR_POS_ADJUST,
              code_enabled
                  ? L"输入数值调整位置... x/y/gap"
                  : L"输入数值调整位置...（需先把当前软件设为代码模式）");
  AppendMenuW(position,
              MF_STRING | (code_enabled ? 0 : MF_DISABLED),
              IDM_REVAR_POS_RESET,
              code_enabled ? L"恢复默认位置：auto / gap=18 / offset=0"
                           : L"恢复默认位置：auto / gap=18 / offset=0（代码模式有效）");
  AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(position), L"代码模式候选窗位置");

  AppendCheckedMenuItem(debug,
                        IDM_REVAR_DEBUG_TRACE_TOGGLE,
                        L"调试日志：写入 revar_input_dev_trace.log",
                        cfg.debug_trace_enabled);
  AppendMenuW(debug, MF_STRING, IDM_REVAR_DEBUG_CLEAR_LOG, L"清空调试日志");
  AppendMenuW(debug, MF_STRING, IDM_REVAR_DEBUG_OPEN_LOG_DIR, L"打开日志目录");
  AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(debug), L"调试");

  AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(root, MF_STRING, IDM_REVAR_CLEAR, L"清除当前 EXE 的 ReVar 记忆");

  POINT pt = GetRevarMenuPopupPoint(hwnd);
  if (hwnd)
    SetForegroundWindow(hwnd);
  UINT cmd = TrackPopupMenuEx(root, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y,
                              hwnd ? hwnd : GetDesktopWindow(), nullptr);
  DestroyMenu(root);
  return cmd;
}

bool ApplyRevarMenuCommand(UINT cmd, const std::wstring& path) {
  if (!cmd || path.empty())
    return false;
  RevarMenuConfig cfg = LoadRevarMenuConfig();
  switch (cmd) {
    case IDM_REVAR_COMPATIBLE:
      SetCurrentExeCompatible(&cfg, path);
      break;
    case IDM_REVAR_CODE_TSF:
      SetCurrentExeCodeTsf(&cfg, path);
      break;
    case IDM_REVAR_CODE_RAW:
      SetCurrentExeCodeRaw(&cfg, path);
      break;
    case IDM_REVAR_CODE_DIRECT:
      SetCurrentExeCodeDirect(&cfg, path);
      break;
    case IDM_REVAR_CODE_BACKSPACE:
      SetCurrentExeCodeBackspace(&cfg, path);
      break;
    case IDM_REVAR_POS_AUTO:
      EnsureCandidateOverride(&cfg, path).position = "auto";
      break;
    case IDM_REVAR_POS_ABOVE:
      EnsureCandidateOverride(&cfg, path).position = "above";
      break;
    case IDM_REVAR_POS_BELOW:
      EnsureCandidateOverride(&cfg, path).position = "below";
      break;
    case IDM_REVAR_POS_RESET: {
      auto& item = EnsureCandidateOverride(&cfg, path);
      item.position = "auto";
      item.gap = 18;
      item.x_offset = 0;
      item.y_offset = 0;
      break;
    }
    case IDM_REVAR_POS_ADJUST:
      return false;
    case IDM_REVAR_DEBUG_TRACE_TOGGLE:
      cfg.debug_trace_enabled = !cfg.debug_trace_enabled;
      RevarTraceSetEnabled(cfg.debug_trace_enabled);
      break;
    case IDM_REVAR_DEBUG_CLEAR_LOG:
      RevarTraceClearLogFile();
      return true;
    case IDM_REVAR_DEBUG_OPEN_LOG_DIR: {
      std::wstring dir = RevarTraceLogDirectory();
      if (!dir.empty())
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      return true;
    }
    case IDM_REVAR_CLEAR:
      ClearCurrentExeMemory(&cfg, path);
      break;
    default:
      return false;
  }
  return SaveRevarMenuConfig(cfg);
}

}  // namespace

void WeaselTSF::_DetachShadowBuffer(com_ptr<ITfContext> pContext) {
  // revar 输入法: 只清 shadow/Rime 候选态，不删除应用中已经输入的 raw text。
  // transparent mode 里 raw text 不在 TSF composition 内；compatible mode 里这相当于 cancel 当前 composition。
  _pEditSessionContext = pContext;
  std::wstring shadow_before = _revarShadowBuffer;
  _revarShadowBuffer.clear();
  _fRevarHasLastNonEmptyContext = FALSE;
  _revarTransparentPendingKeyUps.clear();
  _fRevarTransparentKeyDownPending = FALSE;
  _fRevarTransparentUIActive = FALSE;
  _LogRevarShadowBufferChange(L"detach_shadow_buffer", shadow_before);
  _AbortComposition(true);
}

BOOL WeaselTSF::_IsRevarTransparentModeEnabled() {
  static const BOOL default_enabled = LoadTransparentModeEnabled() ? TRUE : FALSE;
  if (default_enabled)
    return TRUE;
  std::wstring host_path;
  BOOL code_mode = IsCodeModeEnabledForForegroundHost(&host_path) ? TRUE : FALSE;
  static std::wstring last_logged_path;
  static BOOL last_logged_code_mode = FALSE;
  if (host_path != last_logged_path || code_mode != last_logged_code_mode) {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"revar mode host path=" << host_path
          << L" default_transparent=" << default_enabled
          << L" code_mode=" << code_mode;
      WriteRevarDebugLog(dbg.str());
    }
    last_logged_path = host_path;
    last_logged_code_mode = code_mode;
  }
  return code_mode;
}

void WeaselTSF::_LogRevarShadowBufferChange(const wchar_t* reason,
                                             const std::wstring& before) {
  if (RevarTraceEnabled()) {
    std::wstringstream dbg;
    dbg << L"shadow change reason=" << (reason ? reason : L"(null)")
        << L" before_len=" << before.length() << L" before=" << before
        << L" after_len=" << _revarShadowBuffer.length()
        << L" after=" << _revarShadowBuffer
        << L" cache=" << _fRevarHasLastNonEmptyContext
        << L" ui_active=" << _fRevarTransparentUIActive
        << L" pending_keyups=" << _revarTransparentPendingKeyUps.size();
    WriteRevarDebugLog(dbg.str());
  }
  if (_revarShadowBuffer.empty()) {
    _ClearRevarTransparentAnchorPosition();
    _ClearRevarRawRange();
  }
}

BOOL WeaselTSF::_TryHandleRevarTransparentKey(ITfContext* pContext,
                                             WPARAM wParam,
                                             LPARAM lParam,
                                             BOOL keyDown,
                                             BOOL* pfEaten) {
  *pfEaten = FALSE;
  if (!_IsRevarTransparentModeEnabled())
    return FALSE;

  weasel::KeyEvent ke;
  GetKeyboardState(_lpbKeyState);
  if (!ConvertKeyEvent(static_cast<UINT>(wParam), lParam, _lpbKeyState, ke))
    return FALSE;

  if (!keyDown) {
    auto pending = std::find(_revarTransparentPendingKeyUps.begin(),
                             _revarTransparentPendingKeyUps.end(), ke.keycode);
    if (pending != _revarTransparentPendingKeyUps.end()) {
      _revarTransparentPendingKeyUps.erase(pending);
      _fRevarTransparentKeyDownPending =
          _revarTransparentPendingKeyUps.empty() ? FALSE : TRUE;
      *pfEaten = TRUE;
      return TRUE;
    }
    return FALSE;
  }

  if ((_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled())
    return FALSE;
  if (!_EnsureServerConnected())
    return FALSE;

  const bool plain_letter = IsPlainAsciiLetterKey(ke);
  if (plain_letter) {
    const long long t_key0 = RevarTraceNowUs();
    {
      if (RevarTraceEnabled()) {
        std::wstringstream dbg;
        dbg << L"key transparent letter begin keycode=" << ke.keycode
            << L" shadow_before=" << _revarShadowBuffer
            << L" keyDown=" << keyDown;
        WriteRevarDebugLog(dbg.str());
      }
    }
    if (_revarShadowBuffer.empty()) {
      _CaptureRevarTransparentAnchorPosition(pContext);
    }
    const long long t_rime0 = RevarTraceNowUs();
    m_client.ProcessKeyEvent(ke);
    const long long t_rime1 = RevarTraceNowUs();
    std::wstring raw(1, static_cast<wchar_t>(ke.keycode));
    std::wstring host_path;
    const long long t_raw0 = RevarTraceNowUs();
    const bool direct_raw_host = ShouldUseDirectRawForForegroundHost(&host_path);
    const bool sendinput_raw_host = !direct_raw_host &&
                                    ShouldUseSendInputRawForForegroundHost(&host_path);
    if (direct_raw_host) {
      if (RevarTraceEnabled()) {
        std::wstringstream dbg;
        dbg << L"key transparent raw via SendInput unicode direct_async path=" << host_path;
        WriteRevarDebugLog(dbg.str());
      }
      SendRevarUnicodeTextForRaw(raw);
    } else if (sendinput_raw_host) {
      if (RevarTraceEnabled()) {
        std::wstringstream dbg;
        dbg << L"key transparent raw via SendInput unicode path=" << host_path;
        WriteRevarDebugLog(dbg.str());
      }
      SendRevarUnicodeTextForRaw(raw);
    } else {
      if (RevarTraceEnabled()) {
        std::wstringstream dbg;
        dbg << L"key transparent raw via TSF insert path=" << host_path;
        WriteRevarDebugLog(dbg.str());
      }
      _InsertRevarRawText(pContext, raw);
    }
    const long long t_raw1 = RevarTraceNowUs();
    std::wstring shadow_before = _revarShadowBuffer;
    _revarShadowBuffer.push_back(static_cast<wchar_t>(ke.keycode));
    _LogRevarShadowBufferChange(L"append_plain_letter", shadow_before);
    _revarTransparentPendingKeyUps.push_back(ke.keycode);
    const long long t_update0 = RevarTraceNowUs();
    if (direct_raw_host) {
      _UpdateCompositionAsyncOnly(pContext);
    } else {
      _UpdateComposition(pContext);
    }
    const long long t_update1 = RevarTraceNowUs();
    {
      if (RevarTraceEnabled()) {
        std::wstringstream dbg;
        dbg << L"key transparent letter done shadow_after=" << _revarShadowBuffer
            << L" pending_keyups=" << _revarTransparentPendingKeyUps.size()
            << L" us_rime=" << (t_rime1 - t_rime0)
            << L" us_raw=" << (t_raw1 - t_raw0)
            << L" us_update=" << (t_update1 - t_update0)
            << L" us_total=" << (t_update1 - t_key0)
            << L" path=" << host_path;
        WriteRevarDebugLog(dbg.str());
      }
    }
    _fRevarTransparentKeyDownPending = TRUE;
    *pfEaten = TRUE;
    return TRUE;
  }

  if (_revarShadowBuffer.empty())
    return FALSE;

  if (ke.keycode == ibus::BackSpace) {
    m_client.ProcessKeyEvent(ke);
    std::wstring shadow_before = _revarShadowBuffer;
    _revarShadowBuffer.pop_back();
    _LogRevarShadowBufferChange(L"backspace_pop", shadow_before);
    _UpdateComposition(pContext);
    _fRevarTransparentKeyDownPending = TRUE;
    *pfEaten = FALSE;
    return TRUE;
  }

  if (IsTransparentCandidateKey(ke)) {
    BOOL eaten = (BOOL)m_client.ProcessKeyEvent(ke);
    _UpdateComposition(pContext);
    if (!eaten) {
      if (RevarTraceEnabled()) {
        std::wstringstream dbg;
        dbg << L"transparent boundary key clears raw shadow keycode="
            << ke.keycode << L" mask=" << ke.mask << L" shadow="
            << _revarShadowBuffer;
        WriteRevarDebugLog(dbg.str());
      }
      _DetachShadowBuffer(pContext);
      *pfEaten = FALSE;
      return TRUE;
    }
    *pfEaten = TRUE;
    return TRUE;
  }

  // Symbols and other native-editing boundary keys keep the already typed raw
  // text, clear the Rime/shadow state, and pass through to the host app.
  _DetachShadowBuffer(pContext);
  *pfEaten = FALSE;
  return TRUE;
}

RECT WeaselTSF::_GetRevarCandidateAdjustAnchorRect() {
  RECT rc = {0, 0, 0, 0};
  if (_fRevarHasLastCompositionRect &&
      !IsRectEmpty(&_revarLastCompositionRect) &&
      _revarLastCompositionRect.bottom > _revarLastCompositionRect.top) {
    rc = _revarLastCompositionRect;
    if (rc.right <= rc.left)
      rc.right = rc.left + 1;
    if (rc.bottom <= rc.top)
      rc.bottom = rc.top + 18;
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"adjust anchor source=last_composition rc="
          << RevarTraceRect(rc);
      WriteRevarDebugLog(dbg.str());
    }
    return rc;
  }
  HWND foreground = GetForegroundWindow();
  DWORD thread_id = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
  GUITHREADINFO info = {};
  info.cbSize = sizeof(info);
  if (thread_id && GetGUIThreadInfo(thread_id, &info) && info.hwndCaret) {
    rc = info.rcCaret;
    POINT lt = {rc.left, rc.top};
    POINT rb = {rc.right, rc.bottom};
    ClientToScreen(info.hwndCaret, &lt);
    ClientToScreen(info.hwndCaret, &rb);
    rc.left = lt.x;
    rc.top = lt.y;
    rc.right = rb.x;
    rc.bottom = rb.y;
    if (rc.right <= rc.left)
      rc.right = rc.left + 1;
    if (rc.bottom <= rc.top)
      rc.bottom = rc.top + 18;
    {
      if (RevarTraceEnabled()) {
        std::wstringstream dbg;
        dbg << L"adjust anchor source=gui_caret foreground=0x" << std::hex
            << reinterpret_cast<uintptr_t>(foreground) << L" hwndCaret=0x"
            << reinterpret_cast<uintptr_t>(info.hwndCaret) << std::dec
            << L" rc=" << RevarTraceRect(rc);
        WriteRevarDebugLog(dbg.str());
      }
    }
    return rc;
  }

  if (foreground) {
    RECT wr = {};
    if (GetWindowRect(foreground, &wr)) {
      LONG x = wr.left + (wr.right - wr.left) / 3;
      LONG y = wr.top + (wr.bottom - wr.top) / 3;
      rc = {x, y, x + 1, y + 18};
      {
        if (RevarTraceEnabled()) {
          std::wstringstream dbg;
          dbg << L"adjust anchor source=foreground_third foreground=0x" << std::hex
              << reinterpret_cast<uintptr_t>(foreground) << std::dec
              << L" wr=" << RevarTraceRect(wr)
              << L" rc=" << RevarTraceRect(rc);
          WriteRevarDebugLog(dbg.str());
        }
      }
      return rc;
    }
  }

  POINT p = {0, 0};
  GetCursorPos(&p);
  rc = {p.x, p.y, p.x + 1, p.y + 18};
  {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"adjust anchor source=cursor rc=" << RevarTraceRect(rc);
      WriteRevarDebugLog(dbg.str());
    }
  }
  return rc;
}

void WeaselTSF::_ShowRevarCandidateAdjustOverlay() {
  if (IsRectEmpty(&_revarCandidateAdjustAnchorRect))
    _revarCandidateAdjustAnchorRect = _GetRevarCandidateAdjustAnchorRect();

  weasel::Context ctx;
  const wchar_t* position_label = L"auto";
  if (_revarCandidateAdjustPosition == 1)
    position_label = L"above";
  else if (_revarCandidateAdjustPosition == 2)
    position_label = L"below";
  std::wstringstream candidate;
  candidate << L"ReVar 位置调整  pos=" << position_label
            << L"  gap=" << _revarCandidateAdjustGap
            << L"  x=" << _revarCandidateAdjustXOffset
            << L"  y=" << _revarCandidateAdjustYOffset;
  ctx.cinfo.currentPage = 0;
  ctx.cinfo.totalPages = 1;
  ctx.cinfo.highlighted = 0;
  ctx.cinfo.is_last_page = true;
  ctx.cinfo.labels.push_back(weasel::Text(L"1"));
  ctx.cinfo.candies.push_back(weasel::Text(candidate.str()));
  ctx.cinfo.comments.push_back(
      weasel::Text(L"方向键移动 / Enter 保存 / Esc 取消 / R 默认"));

  weasel::Status status = _status;
  status.composing = true;
  status.ascii_mode = false;
  _cand->UpdateUIAtPosition(ctx, status, _revarCandidateAdjustAnchorRect);

  if (RevarTraceEnabled()) {
    std::wstringstream dbg;
    dbg << L"revar candidate adjust overlay show rc=("
        << _revarCandidateAdjustAnchorRect.left << L","
        << _revarCandidateAdjustAnchorRect.top << L","
        << _revarCandidateAdjustAnchorRect.right << L","
        << _revarCandidateAdjustAnchorRect.bottom << L") pos="
        << _revarCandidateAdjustPosition << L" gap=" << _revarCandidateAdjustGap
        << L" x=" << _revarCandidateAdjustXOffset
        << L" y=" << _revarCandidateAdjustYOffset;
    WriteRevarDebugLog(dbg.str());
  }
}

void WeaselTSF::_HideRevarCandidateAdjustOverlay(com_ptr<ITfContext> pContext) {
  // Hide is not enough for this calibration overlay: it is a layered topmost
  // window that may have been moved many times. End the UI element so the old
  // HWND is destroyed and stale translucent pixels cannot remain.
  _cand->Show(FALSE);
  _cand->EndUI();
  SetRectEmpty(&_revarCandidateAdjustAnchorRect);
  WriteRevarDebugLog(L"revar candidate adjust overlay hide destroy");
}

BOOL WeaselTSF::_TryHandleRevarCandidateAdjustKey(ITfContext* pContext,
                                                  WPARAM wParam,
                                                  LPARAM lParam,
                                                  BOOL keyDown,
                                                  BOOL* pfEaten) {
  *pfEaten = FALSE;
  if (!_fRevarCandidateAdjustMode)
    return FALSE;

  weasel::KeyEvent ke;
  GetKeyboardState(_lpbKeyState);
  if (!ConvertKeyEvent(static_cast<UINT>(wParam), lParam, _lpbKeyState, ke)) {
    // In visual adjustment mode, do not leak unknown keys to Rime/the host.
    // A leaked Ctrl/Shift key can hide or reset the candidate panel.
    *pfEaten = TRUE;
    return TRUE;
  }

  const bool control_key = ke.keycode == ibus::Control_L ||
                           ke.keycode == ibus::Control_R;
  const bool shift_key = ke.keycode == ibus::Shift_L ||
                         ke.keycode == ibus::Shift_R;
  const bool alt_key = ke.keycode == ibus::Alt_L || ke.keycode == ibus::Alt_R ||
                       ke.keycode == ibus::Meta_L || ke.keycode == ibus::Meta_R;
  const bool handled_key =
      ke.keycode == ibus::Left || ke.keycode == ibus::Right ||
      ke.keycode == ibus::Up || ke.keycode == ibus::Down ||
      ke.keycode == ibus::Return || ke.keycode == ibus::Escape ||
      ke.keycode == 'r' || ke.keycode == 'R';

  // This is a modal adjustment state. Swallow modifier-only and unrelated keys
  // so they do not close the candidate window or trigger host shortcuts.
  if (!handled_key || control_key || shift_key || alt_key) {
    *pfEaten = TRUE;
    return TRUE;
  }

  if (!keyDown) {
    if (_fRevarCandidateAdjustKeyPending) {
      _fRevarCandidateAdjustKeyPending = FALSE;
      *pfEaten = TRUE;
      return TRUE;
    }
    *pfEaten = TRUE;
    return TRUE;
  }

  const int step = 2;
  bool changed = false;
  bool finish = false;
  bool cancel = false;
  if (ke.keycode == ibus::Left) {
    _revarCandidateAdjustXOffset -= step;
    changed = true;
  } else if (ke.keycode == ibus::Right) {
    _revarCandidateAdjustXOffset += step;
    changed = true;
  } else if (ke.keycode == ibus::Up) {
    _revarCandidateAdjustYOffset -= step;
    changed = true;
  } else if (ke.keycode == ibus::Down) {
    _revarCandidateAdjustYOffset += step;
    changed = true;
  } else if (ke.keycode == 'r' || ke.keycode == 'R') {
    _revarCandidateAdjustPosition = 0;
    _revarCandidateAdjustGap = 18;
    _revarCandidateAdjustXOffset = 0;
    _revarCandidateAdjustYOffset = 0;
    changed = true;
  } else if (ke.keycode == ibus::Return) {
    finish = true;
  } else if (ke.keycode == ibus::Escape) {
    cancel = true;
  }

  if (changed || finish) {
    SaveCandidateAdjustValues(_revarCandidateAdjustPath,
                              _revarCandidateAdjustPosition,
                              _revarCandidateAdjustGap,
                              _revarCandidateAdjustXOffset,
                              _revarCandidateAdjustYOffset);
    if (finish) {
      _fRevarCandidateAdjustMode = FALSE;
      _HideRevarCandidateAdjustOverlay(pContext);
    } else {
      _ShowRevarCandidateAdjustOverlay();
    }
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"revar candidate adjust preview path=" << _revarCandidateAdjustPath
          << L" pos=" << _revarCandidateAdjustPosition
          << L" gap=" << _revarCandidateAdjustGap
          << L" x=" << _revarCandidateAdjustXOffset
          << L" y=" << _revarCandidateAdjustYOffset
          << L" finish=" << finish;
      WriteRevarDebugLog(dbg.str());
    }
  }
  if (cancel) {
    SaveCandidateAdjustValues(_revarCandidateAdjustPath,
                              _revarCandidateAdjustOriginalPosition,
                              _revarCandidateAdjustOriginalGap,
                              _revarCandidateAdjustOriginalXOffset,
                              _revarCandidateAdjustOriginalYOffset);
    _fRevarCandidateAdjustMode = FALSE;
    _HideRevarCandidateAdjustOverlay(pContext);
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"revar candidate adjust cancel restore path="
          << _revarCandidateAdjustPath;
      WriteRevarDebugLog(dbg.str());
    }
  }
  if (finish || cancel) {
    _fRevarCandidateAdjustMode = FALSE;
    _revarCandidateAdjustPath.clear();
  }

  _fRevarCandidateAdjustKeyPending = TRUE;
  *pfEaten = TRUE;
  return TRUE;
}

BOOL WeaselTSF::_TryHandleRevarMenuKey(ITfContext* pContext,
                                       WPARAM wParam,
                                       LPARAM lParam,
                                       BOOL keyDown,
                                       BOOL* pfEaten) {
  *pfEaten = FALSE;
  weasel::KeyEvent ke;
  GetKeyboardState(_lpbKeyState);
  if (!ConvertKeyEvent(static_cast<UINT>(wParam), lParam, _lpbKeyState, ke))
    return FALSE;
  if (!IsRevarMenuHotkey(ke))
    return FALSE;

  if (!keyDown) {
    if (_fRevarMenuKeyPending) {
      _fRevarMenuKeyPending = FALSE;
      *pfEaten = TRUE;
      return TRUE;
    }
    return FALSE;
  }

  std::wstring host_path = GetForegroundProcessPathLowerForKeySink();
  if (!host_path.empty()) {
    RevarMenuConfig cfg = LoadRevarMenuConfig();
    UINT cmd = ShowRevarPopupMenu(host_path, cfg);
    if (cmd) {
      if (cmd == IDM_REVAR_POS_ADJUST) {
        const bool code_enabled = PathInVector(cfg.code_enabled, host_path) &&
                                  !PathInVector(cfg.code_disabled, host_path);
        if (!code_enabled) {
          MessageBeep(MB_ICONWARNING);
          if (RevarTraceEnabled()) {
            std::wstringstream dbg;
            dbg << L"revar candidate adjust rejected path=" << host_path
                << L" code_enabled=" << code_enabled;
            WriteRevarDebugLog(dbg.str());
          }
        } else {
          RevarCandidateOverrideConfig current =
              GetEffectiveCandidateOverride(&cfg, host_path);
          int x_offset = current.x_offset;
          int y_offset = current.y_offset;
          int gap = current.gap;
          HWND owner = GetForegroundWindow();
          if (ShowRevarCandidateAdjustDialog(owner, host_path, &x_offset, &y_offset, &gap)) {
            bool ok = SaveCandidateAdjustValues(host_path,
                                                PositionNameToAdjustInt(current.position),
                                                gap,
                                                x_offset,
                                                y_offset);
            _UpdateCompositionWindow(pContext);
            if (RevarTraceEnabled()) {
              std::wstringstream dbg;
              dbg << L"revar candidate adjust dialog save path=" << host_path
                  << L" pos=" << PositionNameToAdjustInt(current.position)
                  << L" gap=" << gap << L" x=" << x_offset << L" y=" << y_offset
                  << L" ok=" << ok;
              WriteRevarDebugLog(dbg.str());
            }
          } else {
            if (RevarTraceEnabled()) {
              std::wstringstream dbg;
              dbg << L"revar candidate adjust dialog cancel path=" << host_path;
              WriteRevarDebugLog(dbg.str());
            }
          }
        }
      } else {
        bool ok = ApplyRevarMenuCommand(cmd, host_path);
        if (cmd == IDM_REVAR_POS_AUTO || cmd == IDM_REVAR_POS_ABOVE ||
            cmd == IDM_REVAR_POS_BELOW || cmd == IDM_REVAR_POS_RESET) {
          _UpdateCompositionWindow(pContext);
        }
        if (RevarTraceEnabled()) {
          std::wstringstream dbg;
          dbg << L"revar menu cmd=" << cmd << L" ok=" << ok
              << L" path=" << host_path;
          WriteRevarDebugLog(dbg.str());
        }
      }
    }
  }

  _fRevarMenuKeyPending = TRUE;
  *pfEaten = TRUE;
  return TRUE;
}

BOOL WeaselTSF::_TryHandleRevarDetachKey(ITfContext* pContext,
                                         WPARAM wParam,
                                         LPARAM lParam,
                                         BOOL keyDown,
                                         BOOL* pfEaten) {
  *pfEaten = FALSE;
  weasel::KeyEvent ke;
  GetKeyboardState(_lpbKeyState);
  if (!ConvertKeyEvent(static_cast<UINT>(wParam), lParam, _lpbKeyState, ke))
    return FALSE;
  if (!IsDetachHotkey(ke))
    return FALSE;

  if (!keyDown) {
    if (_fRevarDetachKeyPending) {
      _fRevarDetachKeyPending = FALSE;
      *pfEaten = TRUE;
      return TRUE;
    }
    return FALSE;
  }

  // 空闲状态不抢 F1，保留宿主应用自己的 Help 等行为。
  if (!_status.composing && !_IsComposing() && _revarShadowBuffer.empty())
    return FALSE;

  _DetachShadowBuffer(pContext);
  _fRevarDetachKeyPending = TRUE;
  *pfEaten = TRUE;
  return TRUE;
}

void WeaselTSF::_ProcessKeyEvent(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
  // when _IsKeyboardDisabled don't eat the key,
  // when keyboard closable and keyboard closed, don't eat the key
  if ((_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled()) {
    *pfEaten = FALSE;
    return;
  }

  // if server connection is Not OK, don't eat it.
  if (!_EnsureServerConnected()) {
    *pfEaten = FALSE;
    return;
  }
  weasel::KeyEvent ke;
  GetKeyboardState(_lpbKeyState);
  if (!ConvertKeyEvent(static_cast<UINT>(wParam), lParam, _lpbKeyState, ke)) {
    /* Unknown key event */
    *pfEaten = FALSE;
  } else {
    // cheet key code when vertical auto reverse happened, swap up and down
    if (_cand->GetIsReposition()) {
      if (ke.keycode == ibus::Up)
        ke.keycode = ibus::Down;
      else if (ke.keycode == ibus::Down)
        ke.keycode = ibus::Up;
    }
    if (!keyCountToSimulate)
      *pfEaten = (BOOL)m_client.ProcessKeyEvent(ke);

    if (ke.keycode == ibus::Caps_Lock) {
      if (prevKeyEvent.keycode == ibus::Caps_Lock && prevfEaten == TRUE &&
          (ke.mask & ibus::RELEASE_MASK) && (!keyCountToSimulate)) {
        if ((GetKeyState(VK_CAPITAL) & 0x01)) {
          if (_committed || (!*pfEaten && _status.composing)) {
            keyCountToSimulate = 2;
            INPUT inputs[2];
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki = {VK_CAPITAL, 0, 0, 0, 0};
            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].ki = {VK_CAPITAL, 0, KEYEVENTF_KEYUP, 0, 0};
            ::SendInput(sizeof(inputs) / sizeof(INPUT), inputs, sizeof(INPUT));
          }
        }
        *pfEaten = TRUE;
      }
      if (keyCountToSimulate)
        keyCountToSimulate--;
    }

    prevfEaten = *pfEaten;
    prevKeyEvent = ke;
  }
}

STDAPI WeaselTSF::OnSetFocus(BOOL fForeground) {
  {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"key OnSetFocus foreground=" << fForeground
          << L" shadow_len=" << _revarShadowBuffer.length()
          << L" shadow=" << _revarShadowBuffer;
      WriteRevarDebugLog(dbg.str());
    }
  }
  if (fForeground)
    m_client.FocusIn();
  else {
    m_client.FocusOut();
    std::wstring shadow_before = _revarShadowBuffer;
    _revarShadowBuffer.clear();
    _fRevarHasLastNonEmptyContext = FALSE;
    _revarTransparentPendingKeyUps.clear();
    _fRevarTransparentKeyDownPending = FALSE;
    _fRevarTransparentUIActive = FALSE;
    _fRevarHasLastCompositionRect = FALSE;
    SetRectEmpty(&_revarLastCompositionRect);
    _LogRevarShadowBufferChange(L"focus_lost_clear", shadow_before);
    _AbortComposition();
  }

  return S_OK;
}

/* Some apps sends strange OnTestKeyDown/OnKeyDown combinations:
 *  Some sends OnKeyDown() only. (QQ2012)
 *  Some sends multiple OnTestKeyDown() for a single key event. (MS WORD 2010
 * x64)
 *
 * We assume every key event will eventually cause a OnKeyDown() call.
 * We use _fTestKeyDownPending to omit multiple OnTestKeyDown() calls,
 *  and for OnKeyDown() to check if the key has already been sent to the server.
 */

STDAPI WeaselTSF::OnTestKeyDown(ITfContext* pContext,
                                WPARAM wParam,
                                LPARAM lParam,
                                BOOL* pfEaten) {
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
  if (_TryHandleRevarCandidateAdjustKey(pContext, wParam, lParam, TRUE,
                                        pfEaten)) {
    if (*pfEaten)
      _fTestKeyDownPending = TRUE;
    return S_OK;
  }
  if (_TryHandleRevarMenuKey(pContext, wParam, lParam, TRUE, pfEaten)) {
    if (*pfEaten)
      _fTestKeyDownPending = TRUE;
    return S_OK;
  }
  if (_TryHandleRevarDetachKey(pContext, wParam, lParam, TRUE, pfEaten)) {
    if (*pfEaten)
      _fTestKeyDownPending = TRUE;
    return S_OK;
  }
  if (_TryHandleRevarTransparentKey(pContext, wParam, lParam, TRUE,
                                    pfEaten)) {
    if (*pfEaten)
      _fTestKeyDownPending = TRUE;
    return S_OK;
  }
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  if (*pfEaten)
    _fTestKeyDownPending = TRUE;
  return S_OK;
}

STDAPI WeaselTSF::OnKeyDown(ITfContext* pContext,
                            WPARAM wParam,
                            LPARAM lParam,
                            BOOL* pfEaten) {
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    _fTestKeyDownPending = FALSE;
    *pfEaten = TRUE;
  } else {
    if (_TryHandleRevarCandidateAdjustKey(pContext, wParam, lParam, TRUE,
                                          pfEaten))
      return S_OK;
    if (_TryHandleRevarMenuKey(pContext, wParam, lParam, TRUE, pfEaten))
      return S_OK;
    if (_TryHandleRevarDetachKey(pContext, wParam, lParam, TRUE, pfEaten))
      return S_OK;
    if (_TryHandleRevarTransparentKey(pContext, wParam, lParam, TRUE,
                                      pfEaten))
      return S_OK;
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    _UpdateComposition(pContext);
  }
  return S_OK;
}

STDAPI WeaselTSF::OnTestKeyUp(ITfContext* pContext,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL* pfEaten) {
  _fTestKeyDownPending = FALSE;
  if (_fTestKeyUpPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
  if (_TryHandleRevarCandidateAdjustKey(pContext, wParam, lParam, FALSE,
                                        pfEaten)) {
    if (*pfEaten)
      _fTestKeyUpPending = TRUE;
    return S_OK;
  }
  if (_TryHandleRevarMenuKey(pContext, wParam, lParam, FALSE, pfEaten)) {
    if (*pfEaten)
      _fTestKeyUpPending = TRUE;
    return S_OK;
  }
  if (_TryHandleRevarDetachKey(pContext, wParam, lParam, FALSE, pfEaten)) {
    if (*pfEaten)
      _fTestKeyUpPending = TRUE;
    return S_OK;
  }
  if (_TryHandleRevarTransparentKey(pContext, wParam, lParam, FALSE,
                                    pfEaten)) {
    if (*pfEaten)
      _fTestKeyUpPending = TRUE;
    return S_OK;
  }
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  if (*pfEaten)
    _fTestKeyUpPending = TRUE;
  return S_OK;
}

STDAPI WeaselTSF::OnKeyUp(ITfContext* pContext,
                          WPARAM wParam,
                          LPARAM lParam,
                          BOOL* pfEaten) {
  _fTestKeyDownPending = FALSE;
  if (_fTestKeyUpPending) {
    _fTestKeyUpPending = FALSE;
    *pfEaten = TRUE;
  } else {
    if (_TryHandleRevarCandidateAdjustKey(pContext, wParam, lParam, FALSE,
                                          pfEaten))
      return S_OK;
    if (_TryHandleRevarMenuKey(pContext, wParam, lParam, FALSE, pfEaten))
      return S_OK;
    if (_TryHandleRevarDetachKey(pContext, wParam, lParam, FALSE, pfEaten))
      return S_OK;
    if (_TryHandleRevarTransparentKey(pContext, wParam, lParam, FALSE,
                                      pfEaten))
      return S_OK;
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    if (!_async_edit)
      _UpdateComposition(pContext);
  }
  return S_OK;
}

STDAPI WeaselTSF::OnPreservedKey(ITfContext* pContext,
                                 REFGUID rguid,
                                 BOOL* pfEaten) {
  *pfEaten = FALSE;
  return S_OK;
}

BOOL WeaselTSF::_InitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
  HRESULT hr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return FALSE;

  hr = pKeystrokeMgr->AdviseKeyEventSink(_tfClientId, (ITfKeyEventSink*)this,
                                         TRUE);

  return (hr == S_OK);
}

void WeaselTSF::_UninitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return;

  pKeystrokeMgr->UnadviseKeyEventSink(_tfClientId);
}

BOOL WeaselTSF::_InitPreservedKey() {
  return TRUE;
#if 0
	com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (_pThreadMgr->QueryInterface(pKeystrokeMgr.GetAddressOf()) != S_OK)
	{
		return FALSE;
	}
	TF_PRESERVEDKEY preservedKeyImeMode;

	/* Define SHIFT ONLY for now */
	preservedKeyImeMode.uVKey = VK_SHIFT;
	preservedKeyImeMode.uModifiers = TF_MOD_ON_KEYUP;

	auto hr = pKeystrokeMgr->PreserveKey(
		_tfClientId,
		GUID_IME_MODE_PRESERVED_KEY,
		&preservedKeyImeMode, L"", 0);
	
	return SUCCEEDED(hr);
#endif
}

void WeaselTSF::_UninitPreservedKey() {}
