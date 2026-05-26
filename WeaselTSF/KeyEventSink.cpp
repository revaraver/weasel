#include "stdafx.h"
#include "WeaselIPC.h"
#include "WeaselTSF.h"
#include <KeyEvent.h>
#include "CandidateList.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

static weasel::KeyEvent prevKeyEvent;
static BOOL prevfEaten = FALSE;
static int keyCountToSimulate = 0;

namespace {
struct RevarHotkeySpec {
  UINT keycode = 0;
  UINT mask = 0;
};

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
}  // namespace

void WeaselTSF::_DetachShadowBuffer(com_ptr<ITfContext> pContext) {
  // revar 输入法: 只清 shadow/Rime 候选态，不删除应用中已经输入的 raw text。
  // transparent mode 里 raw text 不在 TSF composition 内；compatible mode 里这相当于 cancel 当前 composition。
  _pEditSessionContext = pContext;
  _AbortComposition(true);
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
  if (!_status.composing && !_IsComposing())
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
