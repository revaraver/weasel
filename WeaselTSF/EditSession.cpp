#include "stdafx.h"
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "ResponseParser.h"
#include <RevarDevTrace.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace {
void WriteRevarDebugLog(const std::wstring& line) {
  RevarTraceLog(L"edit", line);
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
      if (!commit.empty()) {
        if (RevarTraceEnabled()) {
          std::wstringstream dbg;
          dbg << L"response transparent commit schema=" << _status.schema_id
              << L" shadow=" << _revarShadowBuffer << L" commit=" << commit;
          WriteRevarDebugLog(dbg.str());
        }

        _ReplaceRevarShadowBufferWithTextInEditSession(_pEditSessionContext,
                                                       ec, commit);
        std::wstring shadow_before = _revarShadowBuffer;
        _revarShadowBuffer.clear();
        _fRevarHasLastNonEmptyContext = FALSE;
        _LogRevarShadowBufferChange(L"commit_response_clear", shadow_before);
        _committed = TRUE;
      } else {
        _committed = FALSE;
      }

      const bool transparent_active =
          _status.composing || !_revarShadowBuffer.empty();
      if (transparent_active) {
        if (!_fRevarTransparentUIActive) {
          _StartUI();
          _fRevarTransparentUIActive = TRUE;
        }
      } else {
        std::wstring shadow_before = _revarShadowBuffer;
        _revarShadowBuffer.clear();
        _fRevarHasLastNonEmptyContext = FALSE;
        _LogRevarShadowBufferChange(L"transparent_inactive_clear", shadow_before);
        if (_fRevarTransparentUIActive) {
          _EndUI();
          _fRevarTransparentUIActive = FALSE;
        }
        if (_IsComposing()) {
          _EndComposition(_pEditSessionContext, true);
        }
      }
      // _UpdateComposition() updates the composition/candidate window after
      // RequestEditSession returns. Avoid doing the same GetTextExt/window
      // update twice inside every key event; Godot makes that duplicated TSF
      // path very visible as code-mode input damping.
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
      // See transparent branch above: _UpdateComposition() performs one
      // composition-window update after this edit session returns.
    }
  }

  _UpdateUI(*context, _status);

  return TRUE;
}
