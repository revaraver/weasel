#include "stdafx.h"
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "ResponseParser.h"
#include "LatencyLog.h"

STDAPI WeaselTSF::DoEditSession(TfEditCookie ec) {
  unsigned long long total_start = weasel_timing::QpcUs();
  // get commit string from server
  std::wstring commit;
  weasel::Config config;
  auto context = std::make_shared<weasel::Context>();
  weasel::ResponseParser parser(&commit, context.get(), &_status, &config,
                                &_cand->style());

  unsigned long long t0 = weasel_timing::QpcUs();
  bool ok = m_client.GetResponseData(std::ref(parser));
  unsigned long long get_response_us = weasel_timing::QpcUs() - t0;

  t0 = weasel_timing::QpcUs();
  _UpdateLanguageBar(_status);
  unsigned long long update_language_bar_us = weasel_timing::QpcUs() - t0;

  unsigned long long composition_ops_us = 0;
  if (ok) {
    t0 = weasel_timing::QpcUs();
    if (!commit.empty()) {
      // For auto-selecting, commit and preedit can both exist.
      // Commit and close the original composition first.
      if (!_IsComposing()) {
        _StartComposition(_pEditSessionContext,
                          _fCUASWorkaroundEnabled && !config.inline_preedit);
      }
      _InsertText(_pEditSessionContext, commit);
      _EndComposition(_pEditSessionContext, false);
      _committed = TRUE;
    } else {
      _committed = FALSE;
    }
    if (_status.composing && !_IsComposing()) {
      _StartComposition(_pEditSessionContext,
                        _fCUASWorkaroundEnabled && !config.inline_preedit);
    } else if (!_status.composing && _IsComposing()) {
      _EndComposition(_pEditSessionContext, true);
    }
    if (_IsComposing() && config.inline_preedit) {
      _ShowInlinePreedit(_pEditSessionContext, context);
    }
    _UpdateCompositionWindow(_pEditSessionContext);
    composition_ops_us = weasel_timing::QpcUs() - t0;
  }

  t0 = weasel_timing::QpcUs();
  _UpdateUI(*context, _status);
  unsigned long long update_ui_us = weasel_timing::QpcUs() - t0;

  unsigned long long total_us = weasel_timing::QpcUs() - total_start;
  if (total_us >= 5000 || get_response_us >= 5000 || composition_ops_us >= 5000 || update_ui_us >= 5000) {
    weasel_timing::Logf("DoEditSession", total_us,
                        "ok=%d composing=%d commit_len=%zu get_response=%.3fms langbar=%.3fms composition_ops=%.3fms update_ui=%.3fms",
                        (int)ok, (int)_status.composing, commit.length(),
                        get_response_us / 1000.0, update_language_bar_us / 1000.0,
                        composition_ops_us / 1000.0, update_ui_us / 1000.0);
  }

  return TRUE;
}
