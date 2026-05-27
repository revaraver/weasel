#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "ResponseParser.h"
#include "CandidateList.h"
#include <RevarDevTrace.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
void WriteRevarDebugLog(const std::wstring& line) {
  RevarTraceLog(L"comp", line);
}

static constexpr ULONG_PTR REVAR_IME_COPYDATA_REPLACE_BEFORE_CARET =
    0x52564952;  // "RVIR" / ReVar IME Replace.

struct RevarImeReplaceBeforeCaretPayload {
  uint32_t version;
  uint32_t raw_length;
  uint32_t text_utf16_length;
};

struct RevarImeReplaceBeforeCaretPayloadV2 {
  uint32_t version;
  uint32_t raw_length;
  uint32_t text_utf16_length;
  uint32_t expected_raw_utf16_length;
};

bool SendRevarGodotDirectText(LONG rawLength,
                              const std::wstring& text,
                              const std::wstring& expectedRaw) {
  HWND hwnd = GetForegroundWindow();
  if (!hwnd)
    return false;

  const bool use_v2 = rawLength > 0 && !expectedRaw.empty();
  const size_t header_size = use_v2 ? sizeof(RevarImeReplaceBeforeCaretPayloadV2)
                                    : sizeof(RevarImeReplaceBeforeCaretPayload);
  std::vector<BYTE> payload(header_size +
                            (text.length() + (use_v2 ? expectedRaw.length() : 0)) * sizeof(wchar_t));
  if (use_v2) {
    auto* header = reinterpret_cast<RevarImeReplaceBeforeCaretPayloadV2*>(payload.data());
    header->version = 2;
    header->raw_length = static_cast<uint32_t>(rawLength < 0 ? 0 : rawLength);
    header->text_utf16_length = static_cast<uint32_t>(text.length());
    header->expected_raw_utf16_length = static_cast<uint32_t>(expectedRaw.length());
    BYTE* cursor = payload.data() + sizeof(RevarImeReplaceBeforeCaretPayloadV2);
    if (!text.empty()) {
      memcpy(cursor, text.data(), text.length() * sizeof(wchar_t));
      cursor += text.length() * sizeof(wchar_t);
    }
    if (!expectedRaw.empty()) {
      memcpy(cursor, expectedRaw.data(), expectedRaw.length() * sizeof(wchar_t));
    }
  } else {
    auto* header = reinterpret_cast<RevarImeReplaceBeforeCaretPayload*>(payload.data());
    header->version = 1;
    header->raw_length = static_cast<uint32_t>(rawLength < 0 ? 0 : rawLength);
    header->text_utf16_length = static_cast<uint32_t>(text.length());
    if (!text.empty()) {
      memcpy(payload.data() + sizeof(RevarImeReplaceBeforeCaretPayload),
             text.data(), text.length() * sizeof(wchar_t));
    }
  }

  COPYDATASTRUCT copy_data = {};
  copy_data.dwData = REVAR_IME_COPYDATA_REPLACE_BEFORE_CARET;
  copy_data.cbData = static_cast<DWORD>(payload.size());
  copy_data.lpData = payload.data();

  DWORD_PTR result = 0;
  LRESULT sent = SendMessageTimeoutW(hwnd, WM_COPYDATA, 0,
                                     reinterpret_cast<LPARAM>(&copy_data),
                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 50,
                                     &result);
  if (RevarTraceEnabled()) {
    std::wstringstream dbg;
    dbg << L"godot_direct_replace send sent=" << sent << L" result=" << result
        << L" hwnd=0x" << std::hex << reinterpret_cast<uintptr_t>(hwnd)
        << std::dec << L" raw_len=" << rawLength << L" text=" << text
        << L" expected=" << expectedRaw;
    WriteRevarDebugLog(dbg.str());
  }
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

  if (!inputs.empty()) {
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
  }
}

void SendRevarFallbackReplacement(LONG shadowLength, const std::wstring& text) {
  std::vector<INPUT> inputs;
  inputs.reserve(static_cast<size_t>(shadowLength) * 2 + text.length() * 2);

  for (LONG i = 0; i < shadowLength; ++i) {
    INPUT down = {};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = VK_BACK;
    inputs.push_back(down);

    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(up);
  }

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

  if (!inputs.empty()) {
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
  }
}

void SendRevarFallbackCommitOnly(const std::wstring& text) {
  // 弱 TSF 宿主里用 Backspace*N 回滚 raw text 可能误删用户已有内容。
  // 只在非专用宿主里使用非破坏性兜底。
  SendRevarUnicodeText(text);
}

std::wstring ToLowerWide(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return s;
}

std::string TrimString(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string StripQuotes(std::string s) {
  s = TrimString(s);
  if (s.length() >= 2 &&
      ((s.front() == '"' && s.back() == '"') ||
       (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.length() - 2);
  }
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

std::filesystem::path RevarInputConfigPath() {
  WCHAR appdata[MAX_PATH] = {0};
  DWORD len = GetEnvironmentVariableW(L"APPDATA", appdata, ARRAYSIZE(appdata));
  if (len == 0 || len >= ARRAYSIZE(appdata))
    return std::filesystem::path();
  return std::filesystem::path(appdata) / L"Rime" / L"revar_input.yaml";
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
    std::string trimmed = TrimString(line);
    if (trimmed.empty())
      continue;
    if (trimmed == list_name + ":") {
      in_list = true;
      continue;
    }
    if (in_list && trimmed[0] == '-') {
      std::string item = StripQuotes(TrimString(trimmed.substr(1)));
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

bool IsExactHostPathInConfigList(const std::wstring& path,
                                 const std::string& list_name) {
  if (list_name == "backspace_unicode_exact_paths" ||
      list_name == "direct_replace_exact_paths") {
    const auto paths = LoadExactHostPathList(list_name);
    for (const auto& exact_path : paths) {
      if (path == exact_path)
        return true;
    }
    return false;
  }
  return false;
}

std::wstring GetForegroundProcessPathLower() {
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

bool IsKnownBackspaceReplacementHost(const std::wstring& path) {
  // 只读配置文件里的完整路径 exact match；不做 path/name contains。
  return IsExactHostPathInConfigList(path, "backspace_unicode_exact_paths");
}

bool IsKnownDirectReplacementHost(const std::wstring& path) {
  // 补丁版 Godot/revarime 专用：只读配置里的完整路径 exact match。
  return IsExactHostPathInConfigList(path, "direct_replace_exact_paths");
}

bool ShouldUseBackspaceReplacementForForegroundHost(std::wstring* path_out) {
  std::wstring path = GetForegroundProcessPathLower();
  if (path_out)
    *path_out = path;
  if (path.empty())
    return false;
  if (IsKnownDirectReplacementHost(path))
    return false;
  if (IsKnownBackspaceReplacementHost(path))
    return true;
  return false;
}

}  // namespace

/* Start Composition */
class CStartCompositionEditSession : public CEditSession {
 public:
  CStartCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                               com_ptr<ITfContext> pContext,
                               BOOL fCUASWorkaroundEnabled,
                               BOOL inlinePreeditEnabled)
      : CEditSession(pTextService, pContext),
        _inlinePreeditEnabled(inlinePreeditEnabled) {
    _fCUASWorkaroundEnabled = fCUASWorkaroundEnabled;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  BOOL _fCUASWorkaroundEnabled;
  BOOL _inlinePreeditEnabled;
};

STDAPI CStartCompositionEditSession::DoEditSession(TfEditCookie ec) {
  HRESULT hr = E_FAIL;
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  if (_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                (LPVOID*)&pInsertAtSelection) != S_OK)
    return hr;
  if (pInsertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0,
                                                &pRangeComposition) != S_OK)
    return hr;

  com_ptr<ITfContextComposition> pContextComposition;
  com_ptr<ITfComposition> pComposition;
  if (_pContext->QueryInterface(IID_ITfContextComposition,
                                (LPVOID*)&pContextComposition) != S_OK)
    return hr;
  if ((pContextComposition->StartComposition(
           ec, pRangeComposition, _pTextService, &pComposition) == S_OK) &&
      (pComposition != NULL)) {
    _pTextService->_SetComposition(pComposition);

    /* WORKAROUND:
     *   CUAS does not provide a correct GetTextExt() position unless the
     * composition is filled with characters. So we insert a zero width space
     * here. The workaround is only needed when inline preedit is not enabled.
     *   See https://github.com/rime/weasel/pull/883#issuecomment-1567625762
     */
    if (!_inlinePreeditEnabled) {
      pRangeComposition->SetText(ec, TF_ST_CORRECTION, L" ", 1);
    }

    /* set selection */
    TF_SELECTION tfSelection;
    if (_inlinePreeditEnabled)
      pRangeComposition->Collapse(ec, TF_ANCHOR_END);
    else
      pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    tfSelection.range = pRangeComposition;
    tfSelection.style.ase = TF_AE_NONE;
    tfSelection.style.fInterimChar = FALSE;
    _pContext->SetSelection(ec, 1, &tfSelection);
  }

  return hr;
}

void WeaselTSF::_StartComposition(com_ptr<ITfContext> pContext,
                                  BOOL fCUASWorkaroundEnabled) {
  com_ptr<CStartCompositionEditSession> pStartCompositionEditSession;
  pStartCompositionEditSession.Attach(new CStartCompositionEditSession(
      this, pContext, fCUASWorkaroundEnabled, _cand->style().inline_preedit));
  _cand->StartUI();
  if (pStartCompositionEditSession != nullptr) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pStartCompositionEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
}

/* End Composition */
class CEndCompositionEditSession : public CEditSession {
 public:
  CEndCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                             com_ptr<ITfContext> pContext,
                             com_ptr<ITfComposition> pComposition,
                             BOOL clear = TRUE)
      : CEditSession(pTextService, pContext), _clear(clear) {
    _pComposition = pComposition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  BOOL _clear;
};

STDAPI CEndCompositionEditSession::DoEditSession(TfEditCookie ec) {
  /* Clear the dummy text we set before, if any. */
  if (_pComposition == nullptr)
    return S_OK;
  // Avoid null pointer dereference
  if (!_pTextService || !_pContext)
    return S_OK;

  _pTextService->_ClearCompositionDisplayAttributes(ec, _pContext);

  com_ptr<ITfRange> pCompositionRange;
  if (_clear && _pComposition->GetRange(&pCompositionRange) == S_OK)
    pCompositionRange->SetText(ec, 0, L"", 0);

  _pComposition->EndComposition(ec);
  if (_pTextService)  // if _pTextService released, skip _FinalizeComposition
    _pTextService->_FinalizeComposition();
  return S_OK;
}

void WeaselTSF::_EndComposition(com_ptr<ITfContext> pContext, BOOL clear) {
  CEndCompositionEditSession* pEditSession;
  HRESULT hr;

  _cand->EndUI();
  if ((pEditSession = new CEndCompositionEditSession(
           this, pContext, _pComposition, clear)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }
}

/* Get Text Extent */
class CGetTextExtentEditSession : public CEditSession {
 public:
  CGetTextExtentEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfContextView> pContextView,
                            com_ptr<ITfComposition> pComposition,
                            bool enhancedPosition,
                            bool captureAnchorOnly = false)
      : CEditSession(pTextService, pContext) {
    _pContextView = pContextView;
    _pComposition = pComposition;
    _enhancedPosition = enhancedPosition;
    _captureAnchorOnly = captureAnchorOnly;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfContextView> _pContextView;
  com_ptr<ITfComposition> _pComposition;
  bool _enhancedPosition;
  bool _captureAnchorOnly;
};

STDAPI CGetTextExtentEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  ITfRange* pRange;
  RECT rc;
  BOOL fClipped;
  TF_SELECTION selection;
  ULONG nSelection;

  if (FAILED(_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                       (LPVOID*)&pInsertAtSelection)))
    return E_FAIL;
  if (FAILED(_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                                     &nSelection)))
    return E_FAIL;

  if (_pComposition != nullptr && _pComposition->GetRange(&pRange) == S_OK) {
    pRange->Collapse(ec, TF_ANCHOR_START);
  } else {
    // composition end
    // note: selection.range is always an empty range
    pRange = selection.range;
  }

  HRESULT text_ext_hr = _pContextView->GetTextExt(ec, pRange, &rc, &fClipped);
  {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"comp GetTextExt hr=0x" << std::hex << text_ext_hr << std::dec
          << L" rc=(" << rc.left << L"," << rc.top << L"," << rc.right
          << L"," << rc.bottom << L") clipped=" << fClipped
          << L" has_composition=" << (_pComposition != nullptr)
          << L" enhanced=" << _enhancedPosition;
      WriteRevarDebugLog(dbg.str());
    }
  }
  if (text_ext_hr == S_OK && (rc.left != 0 || rc.top != 0)) {
    // get the foreground window pos and check if rc from GetTextExt is out of
    // window
    if (_enhancedPosition) {
      HWND hwnd;
      RECT rcForegroundWindow;
      hwnd = GetForegroundWindow();
      ::GetWindowRect(hwnd, &rcForegroundWindow);

      if (rc.left < rcForegroundWindow.left ||
          rc.left > rcForegroundWindow.right ||
          rc.top < rcForegroundWindow.top ||
          rc.top > rcForegroundWindow.bottom) {
        POINT pt;
        bool hasCaret = ::GetCaretPos(&pt);
        int offsetx = rcForegroundWindow.left - rc.left + (hasCaret ? pt.x : 0);
        int offsety = rcForegroundWindow.top - rc.top + (hasCaret ? pt.y : 0);
        rc.left += offsetx;
        rc.right += offsetx;
        rc.top += offsety;
        rc.bottom += offsety;
      }
    }
    if (_captureAnchorOnly) {
      _pTextService->_SetRevarTransparentAnchorPosition(rc);
    } else {
      _pTextService->_SetCompositionPosition(rc);
    }
  }
  return S_OK;
}

/* Composition Window Handling */
BOOL WeaselTSF::_UpdateCompositionWindow(com_ptr<ITfContext> pContext) {
  com_ptr<ITfContextView> pContextView;
  if (pContext->GetActiveView(&pContextView) != S_OK)
    return FALSE;
  com_ptr<CGetTextExtentEditSession> pEditSession;
  pEditSession.Attach(
      new CGetTextExtentEditSession(this, pContext, pContextView, _pComposition,
                                    _cand->style().enhanced_position));
  if (pEditSession == NULL) {
    return FALSE;
  }
  HRESULT hr;
  pContext->RequestEditSession(_tfClientId, pEditSession,
                               TF_ES_ASYNCDONTCARE | TF_ES_READ, &hr);
  return SUCCEEDED(hr);
}

BOOL WeaselTSF::_CaptureRevarTransparentAnchorPosition(
    com_ptr<ITfContext> pContext) {
  com_ptr<ITfContextView> pContextView;
  if (pContext->GetActiveView(&pContextView) != S_OK)
    return FALSE;
  com_ptr<CGetTextExtentEditSession> pEditSession;
  pEditSession.Attach(new CGetTextExtentEditSession(
      this, pContext, pContextView, _pComposition,
      _cand->style().enhanced_position, true));
  if (pEditSession == NULL)
    return FALSE;
  HRESULT hr = E_FAIL;
  pContext->RequestEditSession(_tfClientId, pEditSession,
                               TF_ES_SYNC | TF_ES_READ, &hr);
  if (hr != S_OK) {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"revar anchor capture sync failed hr=0x" << std::hex << hr;
      WriteRevarDebugLog(dbg.str());
    }
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READ, &hr);
  }
  return SUCCEEDED(hr);
}

void WeaselTSF::_SetRevarTransparentAnchorPosition(const RECT& rc) {
  _revarAnchorRect = rc;
  _fRevarHasAnchorRect = TRUE;
  if (RevarTraceEnabled()) {
    std::wstringstream dbg;
    dbg << L"revar anchor set rc=(" << rc.left << L"," << rc.top << L","
        << rc.right << L"," << rc.bottom << L")";
    WriteRevarDebugLog(dbg.str());
  }
}

void WeaselTSF::_ClearRevarTransparentAnchorPosition() {
  if (_fRevarHasAnchorRect) {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"revar anchor clear rc=(" << _revarAnchorRect.left << L","
          << _revarAnchorRect.top << L"," << _revarAnchorRect.right << L","
          << _revarAnchorRect.bottom << L")";
      WriteRevarDebugLog(dbg.str());
    }
  }
  _fRevarHasAnchorRect = FALSE;
  SetRectEmpty(&_revarAnchorRect);
}

void WeaselTSF::_SetCompositionPosition(const RECT& rc) {
  {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"comp _SetCompositionPosition rc=(" << rc.left << L"," << rc.top
          << L"," << rc.right << L"," << rc.bottom << L") cuas_tested="
          << _fCUASWorkaroundTested << L" cuas_enabled="
          << _fCUASWorkaroundEnabled;
      WriteRevarDebugLog(dbg.str());
    }
  }
  /* Test if rect is valid.
   * If it is invalid during CUAS test, we need to apply CUAS workaround */
  if (!_fCUASWorkaroundTested) {
    _fCUASWorkaroundTested = TRUE;
    if (rc.top == rc.bottom) {
      _fCUASWorkaroundEnabled = TRUE;
      return;
    }
  }
  if (!IsRectEmpty(&rc) && rc.bottom > rc.top) {
    _revarLastCompositionRect = rc;
    _fRevarHasLastCompositionRect = TRUE;
  }
  RECT anchored_rc = rc;
  if (_IsRevarTransparentModeEnabled() && !_revarShadowBuffer.empty() &&
      _fRevarHasAnchorRect) {
    const LONG current_height = rc.bottom - rc.top;
    const LONG anchor_height = _revarAnchorRect.bottom - _revarAnchorRect.top;
    LONG line_threshold =
        current_height > anchor_height ? current_height : anchor_height;
    line_threshold = line_threshold / 2;
    if (line_threshold < 8)
      line_threshold = 8;
    const LONG top_delta = rc.top > _revarAnchorRect.top
                               ? rc.top - _revarAnchorRect.top
                               : _revarAnchorRect.top - rc.top;
    const LONG bottom_delta = rc.bottom > _revarAnchorRect.bottom
                                  ? rc.bottom - _revarAnchorRect.bottom
                                  : _revarAnchorRect.bottom - rc.bottom;
    if (top_delta > line_threshold || bottom_delta > line_threshold) {
      if (RevarTraceEnabled()) {
        std::wstringstream dbg;
        dbg << L"revar anchor reline shadow_len=" << _revarShadowBuffer.length()
            << L" threshold=" << line_threshold << L" old_rc=("
            << _revarAnchorRect.left << L"," << _revarAnchorRect.top << L","
            << _revarAnchorRect.right << L"," << _revarAnchorRect.bottom
            << L") new_rc=(" << rc.left << L"," << rc.top << L"," << rc.right
            << L"," << rc.bottom << L")";
        WriteRevarDebugLog(dbg.str());
      }
      _revarAnchorRect = rc;
    }

    anchored_rc.left = _revarAnchorRect.left;
    anchored_rc.right =
        _revarAnchorRect.right > rc.right ? _revarAnchorRect.right : rc.right;
    anchored_rc.top = _revarAnchorRect.top;
    anchored_rc.bottom = _revarAnchorRect.bottom;
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"revar anchor apply shadow_len=" << _revarShadowBuffer.length()
          << L" src_rc=(" << rc.left << L"," << rc.top << L"," << rc.right
          << L"," << rc.bottom << L") anchored_rc=(" << anchored_rc.left
          << L"," << anchored_rc.top << L"," << anchored_rc.right << L","
          << anchored_rc.bottom << L")";
      WriteRevarDebugLog(dbg.str());
    }
  }

  RECT _rc;
  _rc.left = _rc.right = anchored_rc.left;
  _rc.top = _rc.bottom = anchored_rc.bottom;
  m_client.UpdateInputPosition(anchored_rc);
  _cand->UpdateInputPosition(anchored_rc);
}

/* Inline Preedit */
class CInlinePreeditEditSession : public CEditSession {
 public:
  CInlinePreeditEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfComposition> pComposition,
                            const std::shared_ptr<weasel::Context> context)
      : CEditSession(pTextService, pContext),
        _pComposition(pComposition),
        _context(context) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  const std::shared_ptr<weasel::Context> _context;
};

STDAPI CInlinePreeditEditSession::DoEditSession(TfEditCookie ec) {
  std::wstring preedit = _context->preedit.str;

  com_ptr<ITfRange> pRangeComposition;
  if (_pComposition == nullptr)
    return E_FAIL;
  if ((_pComposition->GetRange(&pRangeComposition)) != S_OK)
    return E_FAIL;

  if ((pRangeComposition->SetText(ec, 0, preedit.c_str(),
                                  static_cast<LONG>(preedit.length()))) != S_OK)
    return E_FAIL;

  /* TODO: Check the availability and correctness of these values */
  int sel_cursor = -1;
  for (size_t i = 0; i < _context->preedit.attributes.size(); i++) {
    if (_context->preedit.attributes.at(i).type == weasel::HIGHLIGHTED) {
      sel_cursor = _context->preedit.attributes.at(i).range.cursor;
      break;
    }
  }

  _pTextService->_SetCompositionDisplayAttributes(ec, _pContext,
                                                  pRangeComposition);

  /* Set caret */
  LONG cch;
  TF_SELECTION tfSelection;
  if (sel_cursor < 0) {
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
  } else {
    pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    pRangeComposition->ShiftStart(ec, sel_cursor, &cch, NULL);
  }
  tfSelection.range = pRangeComposition;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  _pContext->SetSelection(ec, 1, &tfSelection);

  return S_OK;
}

BOOL WeaselTSF::_ShowInlinePreedit(
    com_ptr<ITfContext> pContext,
    const std::shared_ptr<weasel::Context> context) {
  com_ptr<CInlinePreeditEditSession> pEditSession;
  pEditSession.Attach(
      new CInlinePreeditEditSession(this, pContext, _pComposition, context));
  if (pEditSession != NULL) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
  return TRUE;
}

void WeaselTSF::_SetRevarRawRange(com_ptr<ITfRange> pRange) {
  _pRevarRawRange = pRange;
  WriteRevarDebugLog(L"revar raw range set");
}

void WeaselTSF::_ClearRevarRawRange() {
  if (_pRevarRawRange != nullptr) {
    WriteRevarDebugLog(L"revar raw range clear");
  }
  _pRevarRawRange = nullptr;
}

class CReplaceRevarShadowEditSession : public CEditSession {
 public:
  CReplaceRevarShadowEditSession(com_ptr<WeaselTSF> pTextService,
                                 com_ptr<ITfContext> pContext,
                                 LONG shadowLength,
                                 const std::wstring& text)
      : CEditSession(pTextService, pContext),
        _shadowLength(shadowLength),
        _text(text) {}

  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  LONG _shadowLength;
  std::wstring _text;
};

STDMETHODIMP CReplaceRevarShadowEditSession::DoEditSession(TfEditCookie ec) {
  if (_shadowLength > 0 && _pTextService->_pRevarRawRange != nullptr) {
    com_ptr<ITfRange> pRange = _pTextService->_pRevarRawRange;
    WCHAR rangeText[256] = {0};
    ULONG copied = 0;
    HRESULT get_text_hr = pRange->GetText(ec, 0, rangeText,
                                          ARRAYSIZE(rangeText) - 1, &copied);
    if (SUCCEEDED(get_text_hr)) {
      ULONG end = copied < ARRAYSIZE(rangeText) - 1
                      ? copied
                      : static_cast<ULONG>(ARRAYSIZE(rangeText) - 1);
      rangeText[end] = L'\0';
    }
    {
      if (RevarTraceEnabled()) {
        std::wstringstream dbg;
        dbg << L"replace raw_range shadow_len=" << _shadowLength
            << L" get_text_hr=0x" << std::hex << get_text_hr << std::dec
            << L" copied=" << copied << L" expected="
            << _pTextService->_revarShadowBuffer << L" actual=" << rangeText
            << L" text=" << _text;
        WriteRevarDebugLog(dbg.str());
      }
    }
    HRESULT hr = pRange->SetText(ec, TF_ST_CORRECTION, _text.c_str(),
                                 static_cast<LONG>(_text.length()));
    if (SUCCEEDED(hr)) {
      pRange->Collapse(ec, TF_ANCHOR_END);
      TF_SELECTION tfSelection;
      tfSelection.range = pRange;
      tfSelection.style.ase = TF_AE_NONE;
      tfSelection.style.fInterimChar = FALSE;
      _pContext->SetSelection(ec, 1, &tfSelection);
      _pTextService->_ClearRevarRawRange();
      return S_OK;
    }
    {
      if (RevarTraceEnabled()) {
        std::wstringstream dbg;
        dbg << L"replace raw_range settext_failed hr=0x" << std::hex << hr;
        WriteRevarDebugLog(dbg.str());
      }
    }
    _pTextService->_ClearRevarRawRange();
  }

  TF_SELECTION tfSelection;
  ULONG fetched = 0;
  if (FAILED(_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection,
                                     &fetched)) ||
      fetched == 0 || tfSelection.range == nullptr) {
    return E_FAIL;
  }

  com_ptr<ITfRange> pRange = tfSelection.range;
  pRange->Collapse(ec, TF_ANCHOR_START);
  if (_shadowLength > 0) {
    LONG shifted = 0;
    pRange->ShiftStart(ec, -_shadowLength, &shifted, nullptr);
  }

  if (FAILED(pRange->SetText(ec, TF_ST_CORRECTION, _text.c_str(),
                             static_cast<LONG>(_text.length())))) {
    return E_FAIL;
  }

  pRange->Collapse(ec, TF_ANCHOR_END);
  tfSelection.range = pRange;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  _pContext->SetSelection(ec, 1, &tfSelection);
  return S_OK;
}

BOOL WeaselTSF::_ReplaceRevarShadowBufferWithTextInEditSession(
    com_ptr<ITfContext> pContext,
    TfEditCookie ec,
    const std::wstring& text) {
  LONG shadowLength = static_cast<LONG>(_revarShadowBuffer.length());

  std::wstring foregroundPath = GetForegroundProcessPathLower();
  if (shadowLength > 0 && IsKnownDirectReplacementHost(foregroundPath)) {
    std::wstring expectedRaw = _revarShadowBuffer;
    if (SendRevarGodotDirectText(shadowLength, text, expectedRaw)) {
      return TRUE;
    }
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"godot_direct_replace failed_no_destructive_fallback shadow_len="
          << shadowLength << L" text=" << text << L" expected=" << expectedRaw
          << L" path=" << foregroundPath;
      WriteRevarDebugLog(dbg.str());
    }
    return TRUE;
  }

  std::wstring hostPath;
  if (shadowLength > 0 &&
      ShouldUseBackspaceReplacementForForegroundHost(&hostPath)) {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"host_policy backspace_unicode_replace shadow_len="
          << shadowLength << L" text=" << text << L" path=" << hostPath;
      WriteRevarDebugLog(dbg.str());
    }
    SendRevarFallbackReplacement(shadowLength, text);
    return TRUE;
  }

  if (shadowLength > 0 && _pRevarRawRange != nullptr) {
    com_ptr<ITfRange> pRange = _pRevarRawRange;
    WCHAR rangeText[256] = {0};
    ULONG copied = 0;
    HRESULT get_text_hr = pRange->GetText(ec, 0, rangeText,
                                          ARRAYSIZE(rangeText) - 1, &copied);
    if (SUCCEEDED(get_text_hr)) {
      ULONG end = copied < ARRAYSIZE(rangeText) - 1
                      ? copied
                      : static_cast<ULONG>(ARRAYSIZE(rangeText) - 1);
      rangeText[end] = L'\0';
    }
    {
      if (RevarTraceEnabled()) {
        std::wstringstream dbg;
        dbg << L"replace direct raw_range shadow_len=" << shadowLength
            << L" get_text_hr=0x" << std::hex << get_text_hr << std::dec
            << L" copied=" << copied << L" expected=" << _revarShadowBuffer
            << L" actual=" << rangeText << L" text=" << text;
        WriteRevarDebugLog(dbg.str());
      }
    }
    if (SUCCEEDED(get_text_hr) && _revarShadowBuffer != rangeText) {
      std::wstring shadow_before = _revarShadowBuffer;
      _revarShadowBuffer.clear();
      _fRevarHasLastNonEmptyContext = FALSE;
      _LogRevarShadowBufferChange(L"replace_raw_range_mismatch_clear",
                                  shadow_before);
      WriteRevarDebugLog(L"replace raw_range_mismatch: no destructive "
                         L"backspace fallback by default");
      return FALSE;
    }
    HRESULT hr = pRange->SetText(ec, TF_ST_CORRECTION, text.c_str(),
                                 static_cast<LONG>(text.length()));
    if (SUCCEEDED(hr)) {
      pRange->Collapse(ec, TF_ANCHOR_END);
      TF_SELECTION tfSelection;
      tfSelection.range = pRange;
      tfSelection.style.ase = TF_AE_NONE;
      tfSelection.style.fInterimChar = FALSE;
      pContext->SetSelection(ec, 1, &tfSelection);
      _ClearRevarRawRange();
      return TRUE;
    }
    {
      if (RevarTraceEnabled()) {
        std::wstringstream dbg;
        dbg << L"replace direct raw_range settext_failed shadow_len="
            << shadowLength << L" hr=0x" << std::hex << hr << L" text="
            << text;
        WriteRevarDebugLog(dbg.str());
      }
    }
    std::wstring shadow_before = _revarShadowBuffer;
    _revarShadowBuffer.clear();
    _fRevarHasLastNonEmptyContext = FALSE;
    _LogRevarShadowBufferChange(L"replace_raw_range_settext_failed_clear",
                                shadow_before);
    WriteRevarDebugLog(
        L"replace raw_range_settext_failed: no destructive backspace fallback by default");
    return FALSE;
  }
  if (shadowLength > 0) {
    WriteRevarDebugLog(L"replace direct raw_range missing; fallback_shift_path");
  }

  TF_SELECTION tfSelection;
  ULONG fetched = 0;
  if (FAILED(pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection,
                                    &fetched)) ||
      fetched == 0 || tfSelection.range == nullptr) {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"replace direct no_selection fallback shadow_len=" << shadowLength
          << L" text=" << text;
      WriteRevarDebugLog(dbg.str());
    }
    std::wstring shadow_before = _revarShadowBuffer;
    _revarShadowBuffer.clear();
    _fRevarHasLastNonEmptyContext = FALSE;
    _LogRevarShadowBufferChange(L"replace_no_selection_clear", shadow_before);
    WriteRevarDebugLog(
        L"replace no_selection: no destructive backspace fallback by default");
    if (shadowLength == 0)
      SendRevarFallbackCommitOnly(text);
    return FALSE;
  }

  com_ptr<ITfRange> pRange = tfSelection.range;
  pRange->Collapse(ec, TF_ANCHOR_START);
  if (shadowLength > 0) {
    LONG shifted = 0;
    HRESULT hr = pRange->ShiftStart(ec, -shadowLength, &shifted, nullptr);
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"replace direct shift shadow_len=" << shadowLength
          << L" shifted=" << shifted << L" hr=0x" << std::hex << hr
          << L" text=" << text;
      WriteRevarDebugLog(dbg.str());
    }
    if (FAILED(hr) || std::labs(shifted) < shadowLength) {
      std::wstring shadow_before = _revarShadowBuffer;
      _revarShadowBuffer.clear();
      _fRevarHasLastNonEmptyContext = FALSE;
      _LogRevarShadowBufferChange(L"replace_shift_failed_clear", shadow_before);
      WriteRevarDebugLog(
          L"replace shift_failed: no destructive backspace fallback by default");
      return FALSE;
    }
  }

  if (shadowLength > 0) {
    WCHAR rangeText[256] = {0};
    ULONG copied = 0;
    HRESULT get_text_hr =
        pRange->GetText(ec, 0, rangeText, ARRAYSIZE(rangeText) - 1, &copied);
    if (SUCCEEDED(get_text_hr)) {
      ULONG end = copied < ARRAYSIZE(rangeText) - 1
                      ? copied
                      : static_cast<ULONG>(ARRAYSIZE(rangeText) - 1);
      rangeText[end] = L'\0';
    }
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"replace direct range_text shadow_len=" << shadowLength
          << L" get_text_hr=0x" << std::hex << get_text_hr << std::dec
          << L" copied=" << copied << L" expected=" << _revarShadowBuffer
          << L" actual=" << rangeText;
      WriteRevarDebugLog(dbg.str());
    }
    if (SUCCEEDED(get_text_hr) && _revarShadowBuffer != rangeText) {
      std::wstring shadow_before = _revarShadowBuffer;
      _revarShadowBuffer.clear();
      _fRevarHasLastNonEmptyContext = FALSE;
      _LogRevarShadowBufferChange(L"replace_range_text_mismatch_clear",
                                  shadow_before);
      WriteRevarDebugLog(L"replace range_text_mismatch: no destructive "
                         L"backspace fallback by default");
      return FALSE;
    }
  }

  HRESULT hr = pRange->SetText(ec, TF_ST_CORRECTION, text.c_str(),
                               static_cast<LONG>(text.length()));
  if (FAILED(hr)) {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"replace direct settext fallback shadow_len=" << shadowLength
          << L" hr=0x" << std::hex << hr << L" text=" << text;
      WriteRevarDebugLog(dbg.str());
    }
    std::wstring shadow_before = _revarShadowBuffer;
    _revarShadowBuffer.clear();
    _fRevarHasLastNonEmptyContext = FALSE;
    _LogRevarShadowBufferChange(L"replace_settext_failed_clear", shadow_before);
    WriteRevarDebugLog(
        L"replace settext_failed: no destructive backspace fallback by default");
    if (shadowLength == 0)
      SendRevarFallbackCommitOnly(text);
    return FALSE;
  }

  pRange->Collapse(ec, TF_ANCHOR_END);
  tfSelection.range = pRange;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  pContext->SetSelection(ec, 1, &tfSelection);
  return TRUE;
}

class CInsertRevarRawTextEditSession : public CEditSession {
 public:
  CInsertRevarRawTextEditSession(com_ptr<WeaselTSF> pTextService,
                                 com_ptr<ITfContext> pContext,
                                 const std::wstring& text)
      : CEditSession(pTextService, pContext), _text(text) {}

  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  std::wstring _text;
};

STDMETHODIMP CInsertRevarRawTextEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  if (FAILED(_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                       (LPVOID*)&pInsertAtSelection))) {
    return E_FAIL;
  }

  com_ptr<ITfRange> insertedRange;
  HRESULT hr = pInsertAtSelection->InsertTextAtSelection(
      ec, 0, _text.c_str(), static_cast<LONG>(_text.length()), &insertedRange);
  if (FAILED(hr) || insertedRange == nullptr) {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"insert revar raw InsertTextAtSelection failed hr=0x" << std::hex
          << hr << L" text=" << _text;
      WriteRevarDebugLog(dbg.str());
    }
    return FAILED(hr) ? hr : E_FAIL;
  }

  WCHAR insertedText[64] = {0};
  ULONG insertedCopied = 0;
  HRESULT inserted_get_text_hr = insertedRange->GetText(
      ec, 0, insertedText, ARRAYSIZE(insertedText) - 1, &insertedCopied);
  if (SUCCEEDED(inserted_get_text_hr)) {
    ULONG end = insertedCopied < ARRAYSIZE(insertedText) - 1
                    ? insertedCopied
                    : static_cast<ULONG>(ARRAYSIZE(insertedText) - 1);
    insertedText[end] = L'\0';
  }

  if (_pTextService->_pRevarRawRange != nullptr) {
    HRESULT extend_hr = _pTextService->_pRevarRawRange->ShiftEndToRange(
        ec, insertedRange, TF_ANCHOR_END);
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"insert revar raw extend_range hr=0x" << std::hex << extend_hr
          << L" get_text_hr=0x" << inserted_get_text_hr << std::dec
          << L" copied=" << insertedCopied << L" actual=" << insertedText
          << L" text=" << _text;
      WriteRevarDebugLog(dbg.str());
    }
  } else {
    _pTextService->_SetRevarRawRange(insertedRange);
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"insert revar raw set_range get_text_hr=0x" << std::hex
          << inserted_get_text_hr << std::dec << L" copied=" << insertedCopied
          << L" actual=" << insertedText << L" text=" << _text;
      WriteRevarDebugLog(dbg.str());
    }
  }

  com_ptr<ITfRange> selectionRange;
  if (SUCCEEDED(insertedRange->Clone(&selectionRange)) &&
      selectionRange != nullptr) {
    selectionRange->Collapse(ec, TF_ANCHOR_END);
    TF_SELECTION tfSelection;
    tfSelection.range = selectionRange;
    tfSelection.style.ase = TF_AE_NONE;
    tfSelection.style.fInterimChar = FALSE;
    _pContext->SetSelection(ec, 1, &tfSelection);
  }
  return S_OK;
}

BOOL WeaselTSF::_InsertRevarRawText(com_ptr<ITfContext> pContext,
                                    const std::wstring& text) {
  com_ptr<CInsertRevarRawTextEditSession> pEditSession;
  pEditSession.Attach(new CInsertRevarRawTextEditSession(this, pContext, text));
  if (pEditSession != NULL) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"insert revar raw via TSF request_hr=0x" << std::hex << hr
          << L" text=" << text;
      WriteRevarDebugLog(dbg.str());
    }
    return SUCCEEDED(hr);
  }
  return FALSE;
}

BOOL WeaselTSF::_ReplaceRevarShadowBufferWithText(com_ptr<ITfContext> pContext,
                                                  const std::wstring& text) {
  auto shadowLength = static_cast<LONG>(_revarShadowBuffer.length());
  com_ptr<CReplaceRevarShadowEditSession> pEditSession;
  pEditSession.Attach(
      new CReplaceRevarShadowEditSession(this, pContext, shadowLength, text));
  if (pEditSession != NULL) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    return SUCCEEDED(hr);
  }
  return FALSE;
}

/* Update Composition */
class CInsertTextEditSession : public CEditSession {
 public:
  CInsertTextEditSession(com_ptr<WeaselTSF> pTextService,
                         com_ptr<ITfContext> pContext,
                         com_ptr<ITfComposition> pComposition,
                         const std::wstring& text)
      : CEditSession(pTextService, pContext),
        _text(text),
        _pComposition(pComposition) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  std::wstring _text;
  com_ptr<ITfComposition> _pComposition;
};

STDMETHODIMP CInsertTextEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfRange> pRange;
  TF_SELECTION tfSelection;
  HRESULT hRet = S_OK;

  if (_pComposition == nullptr)
    return E_FAIL;
  if (FAILED(_pComposition->GetRange(&pRange)))
    return E_FAIL;

  if (FAILED(pRange->SetText(ec, 0, _text.c_str(),
                             static_cast<LONG>(_text.length()))))
    return E_FAIL;

  /* update the selection to an insertion point just past the inserted text. */
  pRange->Collapse(ec, TF_ANCHOR_END);

  tfSelection.range = pRange;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;

  _pContext->SetSelection(ec, 1, &tfSelection);

  return hRet;
}

BOOL WeaselTSF::_InsertText(com_ptr<ITfContext> pContext,
                            const std::wstring& text) {
  CInsertTextEditSession* pEditSession;
  HRESULT hr;

  if ((pEditSession = new CInsertTextEditSession(this, pContext, _pComposition,
                                                 text)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }

  return TRUE;
}

void WeaselTSF::_UpdateComposition(com_ptr<ITfContext> pContext) {
  HRESULT hr;
  const long long t0 = RevarTraceNowUs();

  _pEditSessionContext = pContext;

  _pEditSessionContext->RequestEditSession(
      _tfClientId, this, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  const long long t_req = RevarTraceNowUs();
  _async_edit = !!(hr == TF_S_ASYNC);
  {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"comp _UpdateComposition request_hr=0x" << std::hex << hr
          << std::dec << L" async=" << _async_edit
          << L" shadow_len=" << _revarShadowBuffer.length()
          << L" shadow=" << _revarShadowBuffer
          << L" us_request=" << (t_req - t0);
      WriteRevarDebugLog(dbg.str());
    }
  }
  BOOL window_ok = _UpdateCompositionWindow(pContext);
  const long long t_win = RevarTraceNowUs();
  {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"comp _UpdateCompositionWindow ok=" << window_ok
          << L" us_window=" << (t_win - t_req)
          << L" us_total=" << (t_win - t0);
      WriteRevarDebugLog(dbg.str());
    }
  }
}

void WeaselTSF::_UpdateCompositionAsyncOnly(com_ptr<ITfContext> pContext) {
  HRESULT hr = E_FAIL;
  const long long t0 = RevarTraceNowUs();
  _pEditSessionContext = pContext;
  _pEditSessionContext->RequestEditSession(
      _tfClientId, this, TF_ES_ASYNC | TF_ES_READWRITE, &hr);
  const long long t1 = RevarTraceNowUs();
  _async_edit = !!(hr == TF_S_ASYNC);
  if (RevarTraceEnabled()) {
    std::wstringstream dbg;
    dbg << L"comp _UpdateCompositionAsyncOnly request_hr=0x" << std::hex << hr
        << std::dec << L" async=" << _async_edit
        << L" shadow_len=" << _revarShadowBuffer.length()
        << L" shadow=" << _revarShadowBuffer
        << L" us_request=" << (t1 - t0);
    WriteRevarDebugLog(dbg.str());
  }
}

/* Composition State */
STDAPI WeaselTSF::OnCompositionTerminated(TfEditCookie ecWrite,
                                          ITfComposition* pComposition) {
  {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"comp OnCompositionTerminated transparent="
          << _IsRevarTransparentModeEnabled() << L" shadow_len="
          << _revarShadowBuffer.length() << L" shadow=" << _revarShadowBuffer
          << L" ui_active=" << _fRevarTransparentUIActive;
      WriteRevarDebugLog(dbg.str());
    }
  }
  // NOTE:
  // This will be called when an edit session ended up with an empty composition
  // string, Even if it is closed normally. Silly M$.

  if (_IsRevarTransparentModeEnabled() && !_revarShadowBuffer.empty()) {
    // revar transparent mode 的真实输入状态由 shadow_buffer 驱动，不由宿主
    // TSF composition 生命周期驱动。Godot 这类宿主会在 raw 字符进正文后主动
    // 结束 TSF composition；如果这里跟着 Abort/Destroy UI，就会出现“打一键候选框
    // 闪一下消失，Backspace 后又恢复”的状态错位。
    _FinalizeComposition();
    if (!_fRevarTransparentUIActive) {
      _StartUI();
      _fRevarTransparentUIActive = TRUE;
    }
    return S_OK;
  }

  _AbortComposition();
  return S_OK;
}

void WeaselTSF::_AbortComposition(bool clear) {
  {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"comp _AbortComposition clear=" << clear
          << L" transparent=" << _IsRevarTransparentModeEnabled()
          << L" shadow_len=" << _revarShadowBuffer.length()
          << L" shadow=" << _revarShadowBuffer
          << L" is_composing=" << _IsComposing();
      WriteRevarDebugLog(dbg.str());
    }
  }
  m_client.ClearComposition();
  if (_IsComposing()) {
    _EndComposition(_pEditSessionContext, clear);
  }
  _committed = TRUE;
  _cand->Destroy();
}

void WeaselTSF::_FinalizeComposition() {
  _pComposition = nullptr;
}

void WeaselTSF::_SetComposition(com_ptr<ITfComposition> pComposition) {
  _pComposition = pComposition;
}

BOOL WeaselTSF::_IsComposing() {
  return _pComposition != NULL;
}
