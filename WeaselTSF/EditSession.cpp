#include "stdafx.h"
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "ResponseParser.h"

#include <algorithm>
#include <fstream>
#include <sstream>

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
}  // namespace

STDAPI WeaselTSF::DoEditSession(TfEditCookie ec) {
  // get commit string from server
  std::wstring commit;
  weasel::Config config;
  auto context = std::make_shared<weasel::Context>();
  weasel::ResponseParser parser(&commit, context.get(), &_status, &config,
                                &_cand->style());

  bool ok = m_client.GetResponseData(std::ref(parser));

  _UpdateLanguageBar(_status);

  if (ok) {
    if (_IsRevarTransparentModeEnabled()) {
      std::wstringstream dbg;
      dbg << L"response transparent schema=" << _status.schema_id
          << L" composing=" << _status.composing << L" shadow="
          << _revarShadowBuffer << L" preedit=" << context->preedit.str
          << L" commit=" << commit << L" cand_count="
          << context->cinfo.candies.size();
      size_t n = std::min<size_t>(context->cinfo.candies.size(), 5);
      for (size_t i = 0; i < n; ++i) {
        dbg << L" cand" << i << L"=" << context->cinfo.candies[i].str;
      }
      WriteRevarDebugLog(dbg.str());

      if (!commit.empty()) {
        _ReplaceRevarShadowBufferWithTextInEditSession(_pEditSessionContext,
                                                       ec, commit);
        _revarShadowBuffer.clear();
        _committed = TRUE;
      } else {
        _committed = FALSE;
      }

      if (_status.composing) {
        if (!_fRevarTransparentUIActive) {
          _StartUI();
          _fRevarTransparentUIActive = TRUE;
        }
      } else {
        _revarShadowBuffer.clear();
        if (_fRevarTransparentUIActive) {
          _EndUI();
          _fRevarTransparentUIActive = FALSE;
        }
        if (_IsComposing()) {
          _EndComposition(_pEditSessionContext, true);
        }
      }
      _UpdateCompositionWindow(_pEditSessionContext);
    } else {
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
    }
  }

  _UpdateUI(*context, _status);

  return TRUE;
}
