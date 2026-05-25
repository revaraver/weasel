#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "ResponseParser.h"
#include "CandidateList.h"
#include "LatencyLog.h"

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
  unsigned long long total_start = weasel_timing::QpcUs();
  HRESULT hr = E_FAIL;
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  unsigned long long t0 = weasel_timing::QpcUs();
  HRESULT hr_qi = _pContext->QueryInterface(IID_ITfInsertAtSelection,
                                            (LPVOID*)&pInsertAtSelection);
  unsigned long long qi_insert_us = weasel_timing::QpcUs() - t0;
  if (hr_qi != S_OK) {
    weasel_timing::Logf("StartComposition.DoEditSession", weasel_timing::QpcUs() - total_start,
                        "failed=QueryInsertAtSelection hr=0x%08X qi_insert=%.3fms",
                        (unsigned)hr_qi, qi_insert_us / 1000.0);
    return hr;
  }
  t0 = weasel_timing::QpcUs();
  HRESULT hr_insert_query = pInsertAtSelection->InsertTextAtSelection(
      ec, TF_IAS_QUERYONLY, NULL, 0, &pRangeComposition);
  unsigned long long insert_query_us = weasel_timing::QpcUs() - t0;
  if (hr_insert_query != S_OK) {
    weasel_timing::Logf("StartComposition.DoEditSession", weasel_timing::QpcUs() - total_start,
                        "failed=InsertTextAtSelection hr=0x%08X qi_insert=%.3fms insert_query=%.3fms",
                        (unsigned)hr_insert_query, qi_insert_us / 1000.0,
                        insert_query_us / 1000.0);
    return hr;
  }

  com_ptr<ITfContextComposition> pContextComposition;
  com_ptr<ITfComposition> pComposition;
  t0 = weasel_timing::QpcUs();
  HRESULT hr_qi_context = _pContext->QueryInterface(IID_ITfContextComposition,
                                                    (LPVOID*)&pContextComposition);
  unsigned long long qi_context_us = weasel_timing::QpcUs() - t0;
  if (hr_qi_context != S_OK) {
    weasel_timing::Logf("StartComposition.DoEditSession", weasel_timing::QpcUs() - total_start,
                        "failed=QueryContextComposition hr=0x%08X qi_insert=%.3fms insert_query=%.3fms qi_context=%.3fms",
                        (unsigned)hr_qi_context, qi_insert_us / 1000.0,
                        insert_query_us / 1000.0, qi_context_us / 1000.0);
    return hr;
  }
  t0 = weasel_timing::QpcUs();
  HRESULT hr_start = pContextComposition->StartComposition(
      ec, pRangeComposition, _pTextService, &pComposition);
  unsigned long long start_composition_us = weasel_timing::QpcUs() - t0;
  unsigned long long set_text_us = 0;
  unsigned long long collapse_us = 0;
  unsigned long long set_selection_us = 0;
  if ((hr_start == S_OK) && (pComposition != NULL)) {
    _pTextService->_SetComposition(pComposition);

    /* WORKAROUND:
     *   CUAS does not provide a correct GetTextExt() position unless the
     * composition is filled with characters. So we insert a zero width space
     * here. The workaround is only needed when inline preedit is not enabled.
     *   See https://github.com/rime/weasel/pull/883#issuecomment-1567625762
     */
    if (!_inlinePreeditEnabled) {
      t0 = weasel_timing::QpcUs();
      pRangeComposition->SetText(ec, TF_ST_CORRECTION, L" ", 1);
      set_text_us = weasel_timing::QpcUs() - t0;
    }

    /* set selection */
    TF_SELECTION tfSelection;
    t0 = weasel_timing::QpcUs();
    if (_inlinePreeditEnabled)
      pRangeComposition->Collapse(ec, TF_ANCHOR_END);
    else
      pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    collapse_us = weasel_timing::QpcUs() - t0;
    tfSelection.range = pRangeComposition;
    tfSelection.style.ase = TF_AE_NONE;
    tfSelection.style.fInterimChar = FALSE;
    t0 = weasel_timing::QpcUs();
    _pContext->SetSelection(ec, 1, &tfSelection);
    set_selection_us = weasel_timing::QpcUs() - t0;
  }

  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || insert_query_us >= 5000 || start_composition_us >= 5000 ||
      set_text_us >= 5000 || set_selection_us >= 5000) {
    weasel_timing::Logf("StartComposition.DoEditSession", total_us,
                        "hr_start=0x%08X has_comp=%d inline=%d qi_insert=%.3fms insert_query=%.3fms qi_context=%.3fms start=%.3fms set_text=%.3fms collapse=%.3fms set_selection=%.3fms",
                        (unsigned)hr_start, (int)(pComposition != NULL),
                        (int)_inlinePreeditEnabled, qi_insert_us / 1000.0,
                        insert_query_us / 1000.0, qi_context_us / 1000.0,
                        start_composition_us / 1000.0, set_text_us / 1000.0,
                        collapse_us / 1000.0, set_selection_us / 1000.0);
  }
  return hr;
}

void WeaselTSF::_StartComposition(com_ptr<ITfContext> pContext,
                                  BOOL fCUASWorkaroundEnabled) {
  unsigned long long total_start = weasel_timing::QpcUs();
  com_ptr<CStartCompositionEditSession> pStartCompositionEditSession;
  pStartCompositionEditSession.Attach(new CStartCompositionEditSession(
      this, pContext, fCUASWorkaroundEnabled, _cand->style().inline_preedit));
  unsigned long long t0 = weasel_timing::QpcUs();
  _cand->StartUI();
  unsigned long long start_ui_us = weasel_timing::QpcUs() - t0;
  HRESULT hr = E_FAIL;
  unsigned long long request_us = 0;
  if (pStartCompositionEditSession != nullptr) {
    t0 = weasel_timing::QpcUs();
    pContext->RequestEditSession(_tfClientId, pStartCompositionEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    request_us = weasel_timing::QpcUs() - t0;
  }
  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || start_ui_us >= 5000 || request_us >= 5000 || hr == TF_S_ASYNC) {
    weasel_timing::Logf("StartComposition.Request", total_us,
                        "hr=0x%08X async=%d inline=%d cuas=%d start_ui=%.3fms request_rw=%.3fms",
                        (unsigned)hr, (int)(hr == TF_S_ASYNC),
                        (int)_cand->style().inline_preedit,
                        (int)fCUASWorkaroundEnabled, start_ui_us / 1000.0,
                        request_us / 1000.0);
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
  unsigned long long total_start = weasel_timing::QpcUs();
  /* Clear the dummy text we set before, if any. */
  if (_pComposition == nullptr)
    return S_OK;
  // Avoid null pointer dereference
  if (!_pTextService || !_pContext)
    return S_OK;

  unsigned long long t0 = weasel_timing::QpcUs();
  _pTextService->_ClearCompositionDisplayAttributes(ec, _pContext);
  unsigned long long clear_attr_us = weasel_timing::QpcUs() - t0;

  com_ptr<ITfRange> pCompositionRange;
  unsigned long long get_range_us = 0;
  unsigned long long set_text_us = 0;
  HRESULT hr_range = S_OK;
  if (_clear) {
    t0 = weasel_timing::QpcUs();
    hr_range = _pComposition->GetRange(&pCompositionRange);
    get_range_us = weasel_timing::QpcUs() - t0;
    if (hr_range == S_OK) {
      t0 = weasel_timing::QpcUs();
      pCompositionRange->SetText(ec, 0, L"", 0);
      set_text_us = weasel_timing::QpcUs() - t0;
    }
  }

  t0 = weasel_timing::QpcUs();
  _pComposition->EndComposition(ec);
  unsigned long long end_composition_us = weasel_timing::QpcUs() - t0;
  unsigned long long finalize_us = 0;
  if (_pTextService) {  // if _pTextService released, skip _FinalizeComposition
    t0 = weasel_timing::QpcUs();
    _pTextService->_FinalizeComposition();
    finalize_us = weasel_timing::QpcUs() - t0;
  }
  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || clear_attr_us >= 5000 || get_range_us >= 5000 ||
      set_text_us >= 5000 || end_composition_us >= 5000) {
    weasel_timing::Logf("EndComposition.DoEditSession", total_us,
                        "clear=%d hr_range=0x%08X clear_attr=%.3fms get_range=%.3fms set_text=%.3fms end=%.3fms finalize=%.3fms",
                        (int)_clear, (unsigned)hr_range,
                        clear_attr_us / 1000.0, get_range_us / 1000.0,
                        set_text_us / 1000.0, end_composition_us / 1000.0,
                        finalize_us / 1000.0);
  }
  return S_OK;
}

void WeaselTSF::_EndComposition(com_ptr<ITfContext> pContext, BOOL clear) {
  unsigned long long total_start = weasel_timing::QpcUs();
  CEndCompositionEditSession* pEditSession;
  HRESULT hr = E_FAIL;

  unsigned long long t0 = weasel_timing::QpcUs();
  _cand->EndUI();
  unsigned long long end_ui_us = weasel_timing::QpcUs() - t0;
  unsigned long long request_us = 0;
  if ((pEditSession = new CEndCompositionEditSession(
           this, pContext, _pComposition, clear)) != NULL) {
    t0 = weasel_timing::QpcUs();
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    request_us = weasel_timing::QpcUs() - t0;
    pEditSession->Release();
  }
  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || end_ui_us >= 5000 || request_us >= 5000 || hr == TF_S_ASYNC) {
    weasel_timing::Logf("EndComposition.Request", total_us,
                        "hr=0x%08X async=%d clear=%d end_ui=%.3fms request_rw=%.3fms",
                        (unsigned)hr, (int)(hr == TF_S_ASYNC), (int)clear,
                        end_ui_us / 1000.0, request_us / 1000.0);
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
  unsigned long long total_start = weasel_timing::QpcUs();
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  ITfRange* pRange;
  RECT rc = {0};
  BOOL fClipped = FALSE;
  TF_SELECTION selection;
  ULONG nSelection;

  unsigned long long t0 = weasel_timing::QpcUs();
  HRESULT hr_qi = _pContext->QueryInterface(IID_ITfInsertAtSelection,
                                            (LPVOID*)&pInsertAtSelection);
  unsigned long long qi_us = weasel_timing::QpcUs() - t0;
  if (FAILED(hr_qi)) {
    weasel_timing::Logf("GetTextExtent.DoEditSession", weasel_timing::QpcUs() - total_start,
                        "failed=QueryInterface hr=0x%08X qi=%.3fms", (unsigned)hr_qi,
                        qi_us / 1000.0);
    return E_FAIL;
  }

  t0 = weasel_timing::QpcUs();
  HRESULT hr_sel = _pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1,
                                           &selection, &nSelection);
  unsigned long long get_selection_us = weasel_timing::QpcUs() - t0;
  if (FAILED(hr_sel)) {
    weasel_timing::Logf("GetTextExtent.DoEditSession", weasel_timing::QpcUs() - total_start,
                        "failed=GetSelection hr=0x%08X qi=%.3fms get_selection=%.3fms",
                        (unsigned)hr_sel, qi_us / 1000.0, get_selection_us / 1000.0);
    return E_FAIL;
  }

  if (_pComposition != nullptr && _pComposition->GetRange(&pRange) == S_OK) {
    pRange->Collapse(ec, TF_ANCHOR_START);
  } else {
    // composition end
    // note: selection.range is always an empty range
    pRange = selection.range;
  }

  t0 = weasel_timing::QpcUs();
  HRESULT hr_ext = _pContextView->GetTextExt(ec, pRange, &rc, &fClipped);
  unsigned long long get_text_ext_us = weasel_timing::QpcUs() - t0;
  bool valid_rc = SUCCEEDED(hr_ext) && (rc.left != 0 || rc.top != 0);
  if (valid_rc) {
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

  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || get_selection_us >= 5000 || get_text_ext_us >= 5000) {
    weasel_timing::Logf("GetTextExtent.DoEditSession", total_us,
                        "hr_ext=0x%08X valid_rc=%d clipped=%d qi=%.3fms get_selection=%.3fms get_text_ext=%.3fms rc=%ld,%ld,%ld,%ld enhanced=%d",
                        (unsigned)hr_ext, (int)valid_rc, (int)fClipped,
                        qi_us / 1000.0, get_selection_us / 1000.0,
                        get_text_ext_us / 1000.0, rc.left, rc.top, rc.right,
                        rc.bottom, (int)_enhancedPosition);
  }
  return S_OK;
}

/* Composition Window Handling */
BOOL WeaselTSF::_UpdateCompositionWindow(com_ptr<ITfContext> pContext) {
  unsigned long long total_start = weasel_timing::QpcUs();
  com_ptr<ITfContextView> pContextView;
  unsigned long long t0 = weasel_timing::QpcUs();
  HRESULT hr_view = pContext->GetActiveView(&pContextView);
  unsigned long long get_active_view_us = weasel_timing::QpcUs() - t0;
  if (hr_view != S_OK) {
    weasel_timing::Logf("UpdateCompositionWindow", weasel_timing::QpcUs() - total_start,
                        "failed=GetActiveView hr=0x%08X get_active_view=%.3fms",
                        (unsigned)hr_view, get_active_view_us / 1000.0);
    return FALSE;
  }
  com_ptr<CGetTextExtentEditSession> pEditSession;
  pEditSession.Attach(
      new CGetTextExtentEditSession(this, pContext, pContextView, _pComposition,
                                    _cand->style().enhanced_position));
  if (pEditSession == NULL) {
    return FALSE;
  }
  HRESULT hr;
  t0 = weasel_timing::QpcUs();
  pContext->RequestEditSession(_tfClientId, pEditSession,
                               TF_ES_ASYNCDONTCARE | TF_ES_READ, &hr);
  unsigned long long request_edit_us = weasel_timing::QpcUs() - t0;
  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || get_active_view_us >= 5000 || request_edit_us >= 5000 || hr == TF_S_ASYNC) {
    weasel_timing::Logf("UpdateCompositionWindow", total_us,
                        "hr=0x%08X async=%d get_active_view=%.3fms request_edit_read=%.3fms",
                        (unsigned)hr, (int)(hr == TF_S_ASYNC),
                        get_active_view_us / 1000.0, request_edit_us / 1000.0);
  }
  return SUCCEEDED(hr);
}

void WeaselTSF::_SetCompositionPosition(const RECT& rc) {
  unsigned long long total_start = weasel_timing::QpcUs();
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
  unsigned long long t0 = weasel_timing::QpcUs();
  m_client.UpdateInputPosition(rc);
  unsigned long long client_update_us = weasel_timing::QpcUs() - t0;
  t0 = weasel_timing::QpcUs();
  _cand->UpdateInputPosition(rc);
  unsigned long long cand_update_us = weasel_timing::QpcUs() - t0;
  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || client_update_us >= 5000 || cand_update_us >= 5000) {
    weasel_timing::Logf("SetCompositionPosition", total_us,
                        "client_update=%.3fms cand_update=%.3fms rc=%ld,%ld,%ld,%ld",
                        client_update_us / 1000.0, cand_update_us / 1000.0,
                        rc.left, rc.top, rc.right, rc.bottom);
  }
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
  unsigned long long total_start = weasel_timing::QpcUs();
  std::wstring preedit = _context->preedit.str;

  com_ptr<ITfRange> pRangeComposition;
  if (_pComposition == nullptr)
    return E_FAIL;
  unsigned long long t0 = weasel_timing::QpcUs();
  HRESULT hr_range = _pComposition->GetRange(&pRangeComposition);
  unsigned long long get_range_us = weasel_timing::QpcUs() - t0;
  if (hr_range != S_OK) {
    weasel_timing::Logf("InlinePreedit.DoEditSession", weasel_timing::QpcUs() - total_start,
                        "failed=GetRange hr=0x%08X get_range=%.3fms preedit_len=%zu",
                        (unsigned)hr_range, get_range_us / 1000.0,
                        preedit.length());
    return E_FAIL;
  }

  t0 = weasel_timing::QpcUs();
  HRESULT hr_set_text = pRangeComposition->SetText(ec, 0, preedit.c_str(),
                                                   static_cast<LONG>(preedit.length()));
  unsigned long long set_text_us = weasel_timing::QpcUs() - t0;
  if (hr_set_text != S_OK) {
    weasel_timing::Logf("InlinePreedit.DoEditSession", weasel_timing::QpcUs() - total_start,
                        "failed=SetText hr=0x%08X get_range=%.3fms set_text=%.3fms preedit_len=%zu",
                        (unsigned)hr_set_text, get_range_us / 1000.0,
                        set_text_us / 1000.0, preedit.length());
    return E_FAIL;
  }

  /* TODO: Check the availability and correctness of these values */
  int sel_cursor = -1;
  for (size_t i = 0; i < _context->preedit.attributes.size(); i++) {
    if (_context->preedit.attributes.at(i).type == weasel::HIGHLIGHTED) {
      sel_cursor = _context->preedit.attributes.at(i).range.cursor;
      break;
    }
  }

  t0 = weasel_timing::QpcUs();
  _pTextService->_SetCompositionDisplayAttributes(ec, _pContext,
                                                  pRangeComposition);
  unsigned long long set_attr_us = weasel_timing::QpcUs() - t0;

  /* Set caret */
  LONG cch;
  TF_SELECTION tfSelection;
  t0 = weasel_timing::QpcUs();
  if (sel_cursor < 0) {
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
  } else {
    pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    pRangeComposition->ShiftStart(ec, sel_cursor, &cch, NULL);
  }
  unsigned long long caret_range_us = weasel_timing::QpcUs() - t0;
  tfSelection.range = pRangeComposition;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  t0 = weasel_timing::QpcUs();
  _pContext->SetSelection(ec, 1, &tfSelection);
  unsigned long long set_selection_us = weasel_timing::QpcUs() - t0;

  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || get_range_us >= 5000 || set_text_us >= 5000 ||
      set_attr_us >= 5000 || caret_range_us >= 5000 || set_selection_us >= 5000) {
    weasel_timing::Logf("InlinePreedit.DoEditSession", total_us,
                        "preedit_len=%zu attr_count=%zu sel_cursor=%d get_range=%.3fms set_text=%.3fms set_attr=%.3fms caret_range=%.3fms set_selection=%.3fms",
                        preedit.length(), _context->preedit.attributes.size(),
                        sel_cursor, get_range_us / 1000.0, set_text_us / 1000.0,
                        set_attr_us / 1000.0, caret_range_us / 1000.0,
                        set_selection_us / 1000.0);
  }

  return S_OK;
}

BOOL WeaselTSF::_ShowInlinePreedit(
    com_ptr<ITfContext> pContext,
    const std::shared_ptr<weasel::Context> context) {
  unsigned long long total_start = weasel_timing::QpcUs();
  com_ptr<CInlinePreeditEditSession> pEditSession;
  pEditSession.Attach(
      new CInlinePreeditEditSession(this, pContext, _pComposition, context));
  HRESULT hr = E_FAIL;
  unsigned long long request_us = 0;
  if (pEditSession != NULL) {
    unsigned long long t0 = weasel_timing::QpcUs();
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    request_us = weasel_timing::QpcUs() - t0;
  }
  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || request_us >= 5000 || hr == TF_S_ASYNC) {
    weasel_timing::Logf("InlinePreedit.Request", total_us,
                        "hr=0x%08X async=%d composing=%d preedit_len=%zu request_rw=%.3fms",
                        (unsigned)hr, (int)(hr == TF_S_ASYNC),
                        (int)_status.composing, context->preedit.str.length(),
                        request_us / 1000.0);
  }
  return TRUE;
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
  unsigned long long total_start = weasel_timing::QpcUs();
  com_ptr<ITfRange> pRange;
  TF_SELECTION tfSelection;
  HRESULT hRet = S_OK;

  if (_pComposition == nullptr)
    return E_FAIL;
  unsigned long long t0 = weasel_timing::QpcUs();
  HRESULT hr_range = _pComposition->GetRange(&pRange);
  unsigned long long get_range_us = weasel_timing::QpcUs() - t0;
  if (FAILED(hr_range)) {
    weasel_timing::Logf("InsertText.DoEditSession", weasel_timing::QpcUs() - total_start,
                        "failed=GetRange hr=0x%08X get_range=%.3fms text_len=%zu",
                        (unsigned)hr_range, get_range_us / 1000.0,
                        _text.length());
    return E_FAIL;
  }

  t0 = weasel_timing::QpcUs();
  HRESULT hr_set_text = pRange->SetText(ec, 0, _text.c_str(),
                                        static_cast<LONG>(_text.length()));
  unsigned long long set_text_us = weasel_timing::QpcUs() - t0;
  if (FAILED(hr_set_text)) {
    weasel_timing::Logf("InsertText.DoEditSession", weasel_timing::QpcUs() - total_start,
                        "failed=SetText hr=0x%08X get_range=%.3fms set_text=%.3fms text_len=%zu",
                        (unsigned)hr_set_text, get_range_us / 1000.0,
                        set_text_us / 1000.0, _text.length());
    return E_FAIL;
  }

  /* update the selection to an insertion point just past the inserted text. */
  t0 = weasel_timing::QpcUs();
  pRange->Collapse(ec, TF_ANCHOR_END);
  unsigned long long collapse_us = weasel_timing::QpcUs() - t0;

  tfSelection.range = pRange;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;

  t0 = weasel_timing::QpcUs();
  _pContext->SetSelection(ec, 1, &tfSelection);
  unsigned long long set_selection_us = weasel_timing::QpcUs() - t0;

  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || get_range_us >= 5000 || set_text_us >= 5000 ||
      collapse_us >= 5000 || set_selection_us >= 5000) {
    weasel_timing::Logf("InsertText.DoEditSession", total_us,
                        "text_len=%zu get_range=%.3fms set_text=%.3fms collapse=%.3fms set_selection=%.3fms",
                        _text.length(), get_range_us / 1000.0,
                        set_text_us / 1000.0, collapse_us / 1000.0,
                        set_selection_us / 1000.0);
  }

  return hRet;
}

BOOL WeaselTSF::_InsertText(com_ptr<ITfContext> pContext,
                            const std::wstring& text) {
  unsigned long long total_start = weasel_timing::QpcUs();
  CInsertTextEditSession* pEditSession;
  HRESULT hr = E_FAIL;
  unsigned long long request_us = 0;

  if ((pEditSession = new CInsertTextEditSession(this, pContext, _pComposition,
                                                 text)) != NULL) {
    unsigned long long t0 = weasel_timing::QpcUs();
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    request_us = weasel_timing::QpcUs() - t0;
    pEditSession->Release();
  }

  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || request_us >= 5000 || hr == TF_S_ASYNC) {
    weasel_timing::Logf("InsertText.Request", total_us,
                        "hr=0x%08X async=%d text_len=%zu request_rw=%.3fms",
                        (unsigned)hr, (int)(hr == TF_S_ASYNC), text.length(),
                        request_us / 1000.0);
  }

  return TRUE;
}

void WeaselTSF::_UpdateComposition(com_ptr<ITfContext> pContext) {
  unsigned long long total_start = weasel_timing::QpcUs();
  HRESULT hr;

  _pEditSessionContext = pContext;

  unsigned long long t0 = weasel_timing::QpcUs();
  _pEditSessionContext->RequestEditSession(
      _tfClientId, this, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  unsigned long long request_rw_us = weasel_timing::QpcUs() - t0;
  _async_edit = !!(hr == TF_S_ASYNC);

  t0 = weasel_timing::QpcUs();
  BOOL updated_window = _UpdateCompositionWindow(pContext);
  unsigned long long update_window_us = weasel_timing::QpcUs() - t0;
  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || request_rw_us >= 5000 || update_window_us >= 5000 || hr == TF_S_ASYNC) {
    weasel_timing::Logf("UpdateComposition", total_us,
                        "hr=0x%08X async=%d request_rw=%.3fms update_window=%.3fms updated_window=%d composing=%d committed=%d",
                        (unsigned)hr, (int)_async_edit, request_rw_us / 1000.0,
                        update_window_us / 1000.0, (int)updated_window,
                        (int)_status.composing, (int)_committed);
  }
}

/* Composition State */
STDAPI WeaselTSF::OnCompositionTerminated(TfEditCookie ecWrite,
                                          ITfComposition* pComposition) {
  // NOTE:
  // This will be called when an edit session ended up with an empty composition
  // string, Even if it is closed normally. Silly M$.

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
