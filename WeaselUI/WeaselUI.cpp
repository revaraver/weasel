#include "stdafx.h"
#include <WeaselUI.h>
#include <RevarDevTrace.h>
#include "WeaselPanel.h"

using namespace weasel;

class weasel::UIImpl {
 public:
  WeaselPanel panel;

  UIImpl(weasel::UI& ui) : panel(ui), shown(false) {}
  ~UIImpl() {}
  void Refresh() {
    if (!panel.IsWindow())
      return;
    if (timer) {
      Hide();
      KillTimer(panel.m_hWnd, AUTOHIDE_TIMER);
      timer = 0;
    }
    panel.Refresh();
  }
  void Show();
  void Hide();
  void ShowWithTimeout(size_t millisec);
  bool IsShown() const { return shown; }

  static VOID CALLBACK OnTimer(_In_ HWND hwnd,
                               _In_ UINT uMsg,
                               _In_ UINT_PTR idEvent,
                               _In_ DWORD dwTime);
  static const int AUTOHIDE_TIMER = 20121220;
  static UINT_PTR timer;
  bool shown;
};

UINT_PTR UIImpl::timer = 0;

void UIImpl::Show() {
  if (!panel.IsWindow()) {
    RevarTraceLog(L"ui", L"UIImpl::Show skipped no_hwnd");
    return;
  }
  CRect wr;
  panel.GetWindowRect(&wr);
  bool already_shown = shown && panel.IsWindowVisible();
  if (RevarTraceEnabled()) {
    std::wstringstream dbg;
    dbg << L"UIImpl::Show before shown=" << shown
        << L" already_shown=" << already_shown << L" hwnd=0x" << std::hex
        << reinterpret_cast<uintptr_t>(panel.m_hWnd) << std::dec
        << L" wr=" << RevarTraceRect(wr);
    RevarTraceLog(L"ui", dbg.str());
  }
  shown = true;
  if (timer) {
    KillTimer(panel.m_hWnd, AUTOHIDE_TIMER);
    timer = 0;
  }
  if (already_shown)
    return;
  panel.ShowWindow(SW_SHOWNA);
}

void UIImpl::Hide() {
  if (!panel.IsWindow())
    return;
  bool already_hidden = !shown && !panel.IsWindowVisible();
  shown = false;
  if (timer) {
    KillTimer(panel.m_hWnd, AUTOHIDE_TIMER);
    timer = 0;
  }
  if (already_hidden)
    return;
  panel.ShowWindow(SW_HIDE);
}

void UIImpl::ShowWithTimeout(size_t millisec) {
  if (!panel.IsWindow())
    return;
  DLOG(INFO) << "ShowWithTimeout: " << millisec;
  panel.ShowWindow(SW_SHOWNA);
  shown = true;
  SetTimer(panel.m_hWnd, AUTOHIDE_TIMER, static_cast<UINT>(millisec),
           &UIImpl::OnTimer);
  timer = UINT_PTR(this);
}
VOID CALLBACK UIImpl::OnTimer(_In_ HWND hwnd,
                              _In_ UINT uMsg,
                              _In_ UINT_PTR idEvent,
                              _In_ DWORD dwTime) {
  DLOG(INFO) << "OnTimer:";
  KillTimer(hwnd, idEvent);
  UIImpl* self = (UIImpl*)timer;
  timer = 0;
  if (self) {
    self->Hide();
    self->shown = false;
  }
}

bool UI::Create(HWND parent) {
  if (RevarTraceEnabled()) {
    std::wstringstream dbg;
    dbg << L"UI::Create parent=0x" << std::hex << reinterpret_cast<uintptr_t>(parent)
        << std::dec << L" has_pimpl=" << (pimpl_ != nullptr)
        << L" has_pending=" << has_pending_input_pos_
        << L" pending=" << RevarTraceRect(pending_input_pos_);
    RevarTraceLog(L"ui", dbg.str());
  }
  if (pimpl_) {
    if (pimpl_->panel.IsWindow()) {
      return true;
    }
    pimpl_->panel.Create(
        parent, 0, 0, WS_POPUP,
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        0U, 0);
    if (has_pending_input_pos_) {
      pimpl_->panel.MoveTo(pending_input_pos_);
    }
    return true;
  }

  pimpl_ = new UIImpl(*this);
  if (!pimpl_)
    return false;

  pimpl_->panel.Create(
      parent, 0, 0, WS_POPUP,
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
      0U, 0);
  if (has_pending_input_pos_) {
    pimpl_->panel.MoveTo(pending_input_pos_);
  }
  return true;
}

void UI::Destroy(bool full) {
  if (pimpl_) {
    // destroy panel
    if (pimpl_->panel.IsWindow()) {
      pimpl_->panel.DestroyWindow();
    }
    if (full) {
      delete pimpl_;
      pimpl_ = 0;
      pDWR.reset();
    }
  }
  has_pending_input_pos_ = false;
  SetRectEmpty(&pending_input_pos_);
}

bool UI::GetIsReposition() {
  if (pimpl_)
    return pimpl_->panel.GetIsReposition();
  else
    return false;
}

void UI::Show() {
  if (pimpl_) {
    pimpl_->Show();
  }
}

void UI::Hide() {
  if (pimpl_) {
    pimpl_->Hide();
  }
}

void UI::ShowWithTimeout(size_t millisec) {
  if (pimpl_) {
    pimpl_->ShowWithTimeout(millisec);
  }
}

bool UI::IsCountingDown() const {
  return pimpl_ && pimpl_->timer != 0;
}

bool UI::IsShown() const {
  return pimpl_ && pimpl_->IsShown();
}

void UI::Refresh() {
  if (pimpl_) {
    pimpl_->Refresh();
  }
}

void UI::UpdateInputPosition(RECT const& rc) {
  {
    if (RevarTraceEnabled()) {
      std::wstringstream dbg;
      dbg << L"UI::UpdateInputPosition rc=" << RevarTraceRect(rc)
          << L" has_pimpl=" << (pimpl_ != nullptr)
          << L" panel_hwnd="
          << (pimpl_ ? reinterpret_cast<uintptr_t>(pimpl_->panel.m_hWnd) : 0)
          << L" is_window=" << (pimpl_ && pimpl_->panel.IsWindow());
      RevarTraceLog(L"ui", dbg.str());
    }
  }
  // ReVar transparent/RVIR can compute the text-ext rect before the candidate

  // stale/default position and then moves once the later rect arrives.
  pending_input_pos_ = rc;
  has_pending_input_pos_ = true;
  if (pimpl_ && pimpl_->panel.IsWindow()) {
    pimpl_->panel.MoveTo(rc);
  }
}

void UI::Update(const Context& ctx, const Status& status) {
  if (ctx_ == ctx && status_ == status)
    return;
  ctx_ = ctx;
  status_ = status;
  if (style_.candidate_abbreviate_length > 0) {
    for (auto& c : ctx_.cinfo.candies) {
      if (c.str.length() > (size_t)style_.candidate_abbreviate_length) {
        c.str =
            c.str.substr(0, (size_t)style_.candidate_abbreviate_length - 1) +
            L"..." + c.str.substr(c.str.length() - 1);
      }
    }
  }
  Refresh();
}
