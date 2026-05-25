#include "stdafx.h"

#include "WeaselTSF.h"
#include "LatencyLog.h"

void WeaselTSF::_ClearCompositionDisplayAttributes(TfEditCookie ec,
                                                   _In_ ITfContext* pContext) {
  unsigned long long total_start = weasel_timing::QpcUs();
  ITfRange* pRangeComposition = nullptr;
  ITfProperty* pDisplayAttributeProperty = nullptr;

  unsigned long long t0 = weasel_timing::QpcUs();
  HRESULT hr_range = _pComposition->GetRange(&pRangeComposition);
  unsigned long long get_range_us = weasel_timing::QpcUs() - t0;
  if (FAILED(hr_range)) {
    weasel_timing::Logf("ClearCompositionDisplayAttributes", weasel_timing::QpcUs() - total_start,
                        "failed=GetRange hr=0x%08X get_range=%.3fms",
                        (unsigned)hr_range, get_range_us / 1000.0);
    return;
  }

  unsigned long long get_property_us = 0;
  unsigned long long clear_us = 0;
  t0 = weasel_timing::QpcUs();
  HRESULT hr_property = pContext->GetProperty(GUID_PROP_ATTRIBUTE,
                                              &pDisplayAttributeProperty);
  get_property_us = weasel_timing::QpcUs() - t0;
  if (SUCCEEDED(hr_property)) {
    t0 = weasel_timing::QpcUs();
    pDisplayAttributeProperty->Clear(ec, pRangeComposition);
    clear_us = weasel_timing::QpcUs() - t0;

    pDisplayAttributeProperty->Release();
  }

  pRangeComposition->Release();
  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || get_range_us >= 5000 || get_property_us >= 5000 || clear_us >= 5000) {
    weasel_timing::Logf("ClearCompositionDisplayAttributes", total_us,
                        "hr_property=0x%08X get_range=%.3fms get_property=%.3fms clear=%.3fms",
                        (unsigned)hr_property, get_range_us / 1000.0,
                        get_property_us / 1000.0, clear_us / 1000.0);
  }
}

BOOL WeaselTSF::_SetCompositionDisplayAttributes(TfEditCookie ec,
                                                 _In_ ITfContext* pContext,
                                                 ITfRange* pRangeComposition) {
  unsigned long long total_start = weasel_timing::QpcUs();
  ITfProperty* pDisplayAttributeProperty = nullptr;
  HRESULT hr = S_OK;
  unsigned long long get_range_us = 0;

  if (pRangeComposition == nullptr) {
    unsigned long long t0 = weasel_timing::QpcUs();
    hr = _pComposition->GetRange(&pRangeComposition);
    get_range_us = weasel_timing::QpcUs() - t0;
  }
  if (FAILED(hr)) {
    weasel_timing::Logf("SetCompositionDisplayAttributes", weasel_timing::QpcUs() - total_start,
                        "failed=GetRange hr=0x%08X get_range=%.3fms",
                        (unsigned)hr, get_range_us / 1000.0);
    return FALSE;
  }

  hr = E_FAIL;

  unsigned long long get_property_us = 0;
  unsigned long long set_value_us = 0;
  unsigned long long t0 = weasel_timing::QpcUs();
  HRESULT hr_property = pContext->GetProperty(GUID_PROP_ATTRIBUTE,
                                              &pDisplayAttributeProperty);
  get_property_us = weasel_timing::QpcUs() - t0;
  if (SUCCEEDED(hr_property)) {
    VARIANT var;
    var.vt = VT_I4;  // we're going to set a TfGuidAtom
    var.lVal = _gaDisplayAttributeInput;

    t0 = weasel_timing::QpcUs();
    hr = pDisplayAttributeProperty->SetValue(ec, pRangeComposition, &var);
    set_value_us = weasel_timing::QpcUs() - t0;

    pDisplayAttributeProperty->Release();
  }

  // DO NOT release range composition here
  // it will be released in another function
  // pRangeComposition->Release();
  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || get_range_us >= 5000 || get_property_us >= 5000 || set_value_us >= 5000) {
    weasel_timing::Logf("SetCompositionDisplayAttributes", total_us,
                        "hr_property=0x%08X hr_set=0x%08X get_range=%.3fms get_property=%.3fms set_value=%.3fms",
                        (unsigned)hr_property, (unsigned)hr,
                        get_range_us / 1000.0, get_property_us / 1000.0,
                        set_value_us / 1000.0);
  }
  return (hr == S_OK);
}

BOOL WeaselTSF::_InitDisplayAttributeGuidAtom() {
  ITfCategoryMgr* pCategoryMgr = nullptr;
  HRESULT hr =
      CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                       IID_ITfCategoryMgr, (void**)&pCategoryMgr);

  if (FAILED(hr)) {
    return FALSE;
  }

  hr = pCategoryMgr->RegisterGUID(c_guidDisplayAttributeInput,
                                  &_gaDisplayAttributeInput);
  if (FAILED(hr)) {
    goto Exit;
  }

Exit:
  pCategoryMgr->Release();

  return (hr == S_OK);
}
