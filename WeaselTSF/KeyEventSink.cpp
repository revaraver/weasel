#include "stdafx.h"
#include "WeaselIPC.h"
#include "WeaselTSF.h"
#include <KeyEvent.h>
#include "CandidateList.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
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
  WCHAR temp[MAX_PATH] = {0};
  if (!GetTempPathW(ARRAYSIZE(temp), temp))
    return;
  std::wstring path = std::wstring(temp) + L"revar_input_tsf_debug.log";
  std::wofstream out(path, std::ios::app);
  if (!out)
    return;
  out << GetTickCount64() << L" " << line << std::endl;
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
  bool transparent_enabled = false;
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
      transparent_enabled = ParseBool(ValueAfterColon(trimmed));
      continue;
    }
  }
  return default_mode_transparent || transparent_enabled;
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

std::wstring ToLowerWide(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return s;
}

bool IsForegroundGodotHost() {
  HWND hwnd = GetForegroundWindow();
  if (!hwnd)
    return false;

  static HWND cached_hwnd = nullptr;
  static DWORD cached_pid = 0;
  static bool cached_is_godot = false;

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid)
    return false;
  if (hwnd == cached_hwnd && pid == cached_pid)
    return cached_is_godot;

  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return false;

  WCHAR path[MAX_PATH] = {0};
  DWORD size = ARRAYSIZE(path);
  BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
  CloseHandle(process);
  if (!ok)
    return false;

  cached_hwnd = hwnd;
  cached_pid = pid;
  cached_is_godot = ToLowerWide(path).find(L"godot") != std::wstring::npos;
  return cached_is_godot;
}

static constexpr ULONG_PTR REVAR_IME_COPYDATA_REPLACE_BEFORE_CARET = 0x52564952; // "RVIR" / ReVar IME Replace.

struct RevarImeReplaceBeforeCaretPayload {
  uint32_t version;
  uint32_t raw_length;
  uint32_t text_utf16_length;
};

bool SendRevarGodotDirectText(LONG rawLength, const std::wstring& text) {
  HWND hwnd = GetForegroundWindow();
  if (!hwnd)
    return false;

  std::vector<BYTE> payload(sizeof(RevarImeReplaceBeforeCaretPayload) +
                            text.length() * sizeof(wchar_t));
  auto* header = reinterpret_cast<RevarImeReplaceBeforeCaretPayload*>(payload.data());
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

  DWORD_PTR result = 0;
  LRESULT sent = SendMessageTimeoutW(hwnd, WM_COPYDATA, 0,
                                     reinterpret_cast<LPARAM>(&copy_data),
                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 200,
                                     &result);
  return sent != 0 && result == TRUE;
}

void SendRevarUnicodeText(const std::wstring& text) {
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
}  // namespace

void WeaselTSF::_DetachShadowBuffer(com_ptr<ITfContext> pContext) {
  // revar 输入法: 只清 shadow/Rime 候选态，不删除应用中已经输入的 raw text。
  // transparent mode 里 raw text 不在 TSF composition 内；compatible mode 里这相当于 cancel 当前 composition。
  _pEditSessionContext = pContext;
  _revarShadowBuffer.clear();
  _revarTransparentPendingKeyUps.clear();
  _fRevarTransparentKeyDownPending = FALSE;
  _fRevarTransparentUIActive = FALSE;
  _AbortComposition(true);
}

BOOL WeaselTSF::_IsRevarTransparentModeEnabled() {
  static const BOOL enabled = LoadTransparentModeEnabled() ? TRUE : FALSE;
  return enabled;
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
    m_client.ProcessKeyEvent(ke);
    std::wstring raw(1, static_cast<wchar_t>(ke.keycode));
    if (IsForegroundGodotHost()) {
      // Keep raw letters on Godot's normal keyboard input path so CodeEdit can
      // update syntax services and show completion while the token is typed.
      // The direct Godot protocol is only for candidate commits/replacements.
      SendRevarUnicodeText(raw);
    } else {
      _InsertRevarRawText(pContext, raw);
    }
    _revarShadowBuffer.push_back(static_cast<wchar_t>(ke.keycode));
    _revarTransparentPendingKeyUps.push_back(ke.keycode);
    _UpdateComposition(pContext);
    _fRevarTransparentKeyDownPending = TRUE;
    *pfEaten = TRUE;
    return TRUE;
  }

  if (_revarShadowBuffer.empty())
    return FALSE;

  if (ke.keycode == ibus::BackSpace) {
    m_client.ProcessKeyEvent(ke);
    _revarShadowBuffer.pop_back();
    _UpdateComposition(pContext);
    _fRevarTransparentKeyDownPending = TRUE;
    *pfEaten = FALSE;
    return TRUE;
  }

  if (IsTransparentCandidateKey(ke)) {
    BOOL eaten = (BOOL)m_client.ProcessKeyEvent(ke);
    _UpdateComposition(pContext);
    if (!eaten) {
      std::wstringstream dbg;
      dbg << L"transparent boundary key clears raw shadow keycode="
          << ke.keycode << L" mask=" << ke.mask << L" shadow="
          << _revarShadowBuffer;
      WriteRevarDebugLog(dbg.str());
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
  if (fForeground)
    m_client.FocusIn();
  else {
    m_client.FocusOut();
    _revarShadowBuffer.clear();
    _revarTransparentPendingKeyUps.clear();
    _fRevarTransparentKeyDownPending = FALSE;
    _fRevarTransparentUIActive = FALSE;
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
