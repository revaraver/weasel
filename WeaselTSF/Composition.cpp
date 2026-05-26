#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "ResponseParser.h"
#include "CandidateList.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
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
                            bool enhancedPosition)
      : CEditSession(pTextService, pContext) {
    _pContextView = pContextView;
    _pComposition = pComposition;
    _enhancedPosition = enhancedPosition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfContextView> _pContextView;
  com_ptr<ITfComposition> _pComposition;
  bool _enhancedPosition;
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

  if ((_pContextView->GetTextExt(ec, pRange, &rc, &fClipped)) == S_OK &&
      (rc.left != 0 || rc.top != 0)) {
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
    _pTextService->_SetCompositionPosition(rc);
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

void WeaselTSF::_SetCompositionPosition(const RECT& rc) {
  /* Test if rect is valid.
   * If it is invalid during CUAS test, we need to apply CUAS workaround */
  if (!_fCUASWorkaroundTested) {
    _fCUASWorkaroundTested = TRUE;
    if (rc.top == rc.bottom) {
      _fCUASWorkaroundEnabled = TRUE;
      return;
    }
  }
  RECT _rc;
  _rc.left = _rc.right = rc.left;
  _rc.top = _rc.bottom = rc.bottom;
  m_client.UpdateInputPosition(rc);
  _cand->UpdateInputPosition(rc);
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
  if (IsForegroundGodotHost()) {
    std::wstringstream dbg;
    dbg << L"godot sendinput replace shadow_len=" << shadowLength
        << L" text=" << text;
    WriteRevarDebugLog(dbg.str());
    _revarShadowBuffer.clear();
    if (SendRevarGodotDirectText(shadowLength, text)) {
      return TRUE;
    }
    SendRevarFallbackReplacement(shadowLength, text);
    return TRUE;
  }

  TF_SELECTION tfSelection;
  ULONG fetched = 0;
  if (FAILED(pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection,
                                    &fetched)) ||
      fetched == 0 || tfSelection.range == nullptr) {
    std::wstringstream dbg;
    dbg << L"replace direct no_selection fallback shadow_len=" << shadowLength
        << L" text=" << text;
    WriteRevarDebugLog(dbg.str());
    _revarShadowBuffer.clear();
    SendRevarFallbackCommitOnly(text);
    return FALSE;
  }

  com_ptr<ITfRange> pRange = tfSelection.range;
  pRange->Collapse(ec, TF_ANCHOR_START);
  if (shadowLength > 0) {
    LONG shifted = 0;
    HRESULT hr = pRange->ShiftStart(ec, -shadowLength, &shifted, nullptr);
    std::wstringstream dbg;
    dbg << L"replace direct shift shadow_len=" << shadowLength
        << L" shifted=" << shifted << L" hr=0x" << std::hex << hr
        << L" text=" << text;
    WriteRevarDebugLog(dbg.str());
    if (FAILED(hr) || std::labs(shifted) < shadowLength) {
      _revarShadowBuffer.clear();
      SendRevarFallbackCommitOnly(text);
      return FALSE;
    }
  }

  HRESULT hr = pRange->SetText(ec, TF_ST_CORRECTION, text.c_str(),
                               static_cast<LONG>(text.length()));
  if (FAILED(hr)) {
    std::wstringstream dbg;
    dbg << L"replace direct settext fallback shadow_len=" << shadowLength
        << L" hr=0x" << std::hex << hr << L" text=" << text;
    WriteRevarDebugLog(dbg.str());
    _revarShadowBuffer.clear();
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

BOOL WeaselTSF::_InsertRevarRawText(com_ptr<ITfContext> pContext,
                                    const std::wstring& text) {
  com_ptr<CReplaceRevarShadowEditSession> pEditSession;
  pEditSession.Attach(new CReplaceRevarShadowEditSession(this, pContext, 0,
                                                         text));
  if (pEditSession != NULL) {
    HRESULT hr = E_FAIL;
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_SYNC | TF_ES_READWRITE, &hr);
    if (hr != S_OK) {
      std::wstringstream dbg;
      dbg << L"raw insert sync failed hr=0x" << std::hex << hr
          << L" text=" << text;
      WriteRevarDebugLog(dbg.str());
      pContext->RequestEditSession(_tfClientId, pEditSession,
                                   TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    }
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

  _pEditSessionContext = pContext;

  _pEditSessionContext->RequestEditSession(
      _tfClientId, this, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  _async_edit = !!(hr == TF_S_ASYNC);
  _UpdateCompositionWindow(pContext);
}

/* Composition State */
STDAPI WeaselTSF::OnCompositionTerminated(TfEditCookie ecWrite,
                                          ITfComposition* pComposition) {
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
