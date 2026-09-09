// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "appearance/feedback.h"

#include "appearance/default_font.h"
#include "appearance/dialog_layout.h"
#include "appearance/dialog_metrics.h"
#include "win32/window_metrics.h"

#include <algorithm>
#include <cwchar>
#include <vector>

#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>

#include "appearance/gdi_cache.h"
#include "appearance/theme.h"
#include "win32/file_dialog.h"
#include "win32/shell_paths.h"

namespace regkit::ui {

namespace {
#ifndef WC_LINK
#define WC_LINK L"SysLink"
#endif

constexpr wchar_t kErrorClass[] = L"RegKitErrorDialog";
constexpr wchar_t kChoiceClass[] = L"RegKitChoiceDialog";
constexpr wchar_t kAboutClass[] = L"RegKitAboutDialog";
constexpr wchar_t kAppTitle[] = L"RegKit";

struct ErrorDialogState {
  HWND hwnd = nullptr;
  HWND text = nullptr;
  HWND detail_box = nullptr;
  HWND ok_btn = nullptr;
  HWND owner = nullptr;
  HFONT font = nullptr;
  std::wstring title;
  std::wstring message;
  std::wstring detail;
  bool accepted = false;
  bool owner_restored = false;
};

struct ChoiceDialogState {
  HWND hwnd = nullptr;
  HWND icon = nullptr;
  HWND text = nullptr;
  HWND detail_edit = nullptr;
  HWND detail_tip = nullptr;
  HWND yes_btn = nullptr;
  HWND no_btn = nullptr;
  HWND cancel_btn = nullptr;
  HWND default_btn = nullptr;
  HWND owner = nullptr;
  HFONT font = nullptr;
  std::wstring title;
  std::wstring message;
  std::wstring detail;
  std::wstring yes_label;
  std::wstring no_label;
  std::wstring cancel_label;
  ChoiceButtonWidths button_widths;
  int default_id = 0;
  PCWSTR icon_id = nullptr;
  int result = IDCANCEL;
  bool accepted = false;
  bool owner_restored = false;
};

struct AboutDialogState {
  HWND hwnd = nullptr;
  HWND credits = nullptr;
  HWND repo_link = nullptr;
  HWND discord_link = nullptr;
  HWND website_link = nullptr;
  HWND email_link = nullptr;
  HWND ok_btn = nullptr;
  HWND owner = nullptr;
  HFONT font = nullptr;
  bool accepted = false;
  bool owner_restored = false;
};

void ApplyConfirmFonts(HWND hwnd, HFONT font) {
  if (!font) {
    return;
  }
  SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  EnumChildWindows(
      hwnd,
      [](HWND child, LPARAM param) -> BOOL {
        HFONT font_handle = reinterpret_cast<HFONT>(param);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_handle), TRUE);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(font));
}

int TextWidth(HWND window, HFONT font, const std::wstring& text) {
  if (text.empty()) {
    return 0;
  }
  HDC dc = GetDC(window);
  if (!dc) {
    return 0;
  }
  HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
  SIZE size = {};
  GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
  if (old_font) {
    SelectObject(dc, old_font);
  }
  ReleaseDC(window, dc);
  return static_cast<int>(size.cx);
}

int TextBlockHeight(HWND window, HFONT font, const std::wstring& text, int width) {
  HDC dc = GetDC(window);
  if (!dc) {
    return 0;
  }
  HFONT old_font = font ? reinterpret_cast<HFONT>(SelectObject(dc, font)) : nullptr;
  RECT rect = {0, 0, width, 0};
  DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  if (old_font) {
    SelectObject(dc, old_font);
  }
  ReleaseDC(window, dc);
  return rect.bottom;
}

constexpr int kMaxDetailLines = 10;

int DetailLineCount(const std::wstring& detail) {
  if (detail.empty()) {
    return 1;
  }
  return 1 + static_cast<int>(std::count(detail.begin(), detail.end(), L'\n'));
}

void FitChoiceDialogToContent(HWND hwnd, ChoiceDialogState* state) {
  if (!hwnd || !state || !state->detail_edit) {
    return;
  }
  RECT client = {};
  GetClientRect(hwnd, &client);
  using namespace appearance::metrics;
  const UINT dpi = win32::DpiForWindow(hwnd);
  const int margin = Scaled(kDialogContentMargin, dpi);
  const int block_gap = Scaled(kBlockGap, dpi);
  const int icon_w = state->icon_id ? Scaled(32, dpi) : 0;
  const int content_w =
      (client.right - client.left) - margin * 2 - icon_w - (icon_w ? block_gap : 0);
  const int text_h =
      std::max(icon_w, TextBlockHeight(hwnd, state->font, state->message, content_w));
  const int lines = std::min(DetailLineCount(state->detail), kMaxDetailLines);
  const int detail_h =
      Scaled(kControlHeight, dpi) + (lines - 1) * Scaled(kDetailLineHeight, dpi);
  const int needed = margin + text_h + block_gap + detail_h + block_gap +
                     Scaled(kButtonHeight, dpi) + Scaled(kDialogButtonBottomMargin, dpi);
  if (needed <= client.bottom - client.top) {
    return;
  }
  RECT frame = {0, 0, client.right - client.left, needed};
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
  const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  win32::AdjustWindowRectForDpi(&frame, style, ex_style, dpi);
  SetWindowPos(hwnd, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void LayoutChoiceDialog(HWND hwnd, ChoiceDialogState* state) {
  if (!hwnd || !state) {
    return;
  }
  using namespace appearance::metrics;
  RECT client = {};
  GetClientRect(hwnd, &client);
  const UINT dpi = win32::DpiForWindow(hwnd);
  const int margin = Scaled(kDialogContentMargin, dpi);
  const int block_gap = Scaled(kBlockGap, dpi);
  const int button_h = Scaled(kButtonHeight, dpi);
  const int button_gap = Scaled(kButtonGap, dpi);
  const int width = client.right - client.left;
  const int icon_w = state->icon_id ? Scaled(32, dpi) : 0;
  appearance::Place(state->icon, margin, margin, icon_w, icon_w);

  const int text_x = margin + icon_w + (icon_w ? block_gap : 0);
  const int content_w = width - text_x - margin;
  int y = margin;
  const int text_h =
      std::max(icon_w, TextBlockHeight(hwnd, state->font, state->message, content_w));
  appearance::Place(state->text, text_x, y, content_w, text_h);
  y += text_h + block_gap;
  if (state->detail_edit) {
    const int lines = std::min(DetailLineCount(state->detail), kMaxDetailLines);
    const int detail_h =
        Scaled(kControlHeight, dpi) + (lines - 1) * Scaled(kDetailLineHeight, dpi);
    appearance::Place(state->detail_edit, text_x, y, content_w, detail_h);
    if (state->detail_tip) {
      SendMessageW(state->detail_tip, TTM_ACTIVATE,
                   lines == 1 && TextWidth(hwnd, state->font, state->detail) >
                                     content_w - Scaled(8, dpi),
                   0);
    }
    y += detail_h + block_gap;
  }

  HWND buttons[] = {state->yes_btn, state->no_btn, state->cancel_btn};
  const int widths[] = {Scaled(state->button_widths.yes, dpi),
                        Scaled(state->button_widths.no, dpi),
                        Scaled(state->button_widths.cancel, dpi)};
  int total_w = 0;
  int button_count = 0;
  for (int i = 0; i < 3; ++i) {
    if (buttons[i]) {
      total_w += widths[i];
      ++button_count;
    }
  }
  if (button_count == 0) {
    return;
  }
  total_w += button_gap * (button_count - 1);
  int x = std::max(margin, width - Scaled(kDialogButtonRightMargin, dpi) - total_w);
  for (int i = 0; i < 3; ++i) {
    if (!buttons[i]) {
      continue;
    }
    appearance::Place(buttons[i], x, y, widths[i], button_h);
    x += widths[i] + button_gap;
  }
  appearance::FitDialogHeight(hwnd, y + button_h + Scaled(kDialogButtonBottomMargin, dpi));
}

void LayoutErrorDialog(HWND hwnd, ErrorDialogState* state) {
  if (!hwnd || !state) {
    return;
  }
  using namespace appearance::metrics;
  RECT client = {};
  GetClientRect(hwnd, &client);
  const UINT dpi = win32::DpiForWindow(hwnd);
  const int margin = Scaled(kDialogContentMargin, dpi);
  const int block_gap = Scaled(kBlockGap, dpi);
  const int button_h = Scaled(kButtonHeight, dpi);
  const int button_w = Scaled(kButtonMinWidth, dpi);
  const int bottom_margin = Scaled(kDialogButtonBottomMargin, dpi);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  const int text_w = width - margin * 2;
  int btn_y = height - bottom_margin - button_h;
  const int message_h = TextBlockHeight(hwnd, state->font, state->message, text_w);
  if (state->detail_box) {
    appearance::Place(state->text, margin, margin, text_w,
                      std::min(message_h, std::max(0, btn_y - margin)));
    const int detail_y = margin + message_h + block_gap;
    appearance::Place(state->detail_box, margin, detail_y, text_w,
                      std::max(0, btn_y - detail_y - block_gap));
  } else if (state->text) {
    appearance::Place(state->text, margin, margin, text_w, message_h);
    btn_y = margin + message_h + block_gap;
    appearance::FitDialogHeight(hwnd, btn_y + button_h + bottom_margin);
  }
  appearance::Place(state->ok_btn, width - Scaled(kDialogButtonRightMargin, dpi) - button_w,
                    btn_y, button_w, button_h);
}

void LayoutAboutDialog(HWND hwnd, AboutDialogState* state) {
  if (!hwnd || !state) {
    return;
  }
  using namespace appearance::metrics;
  RECT client = {};
  GetClientRect(hwnd, &client);
  const UINT dpi = win32::DpiForWindow(hwnd);
  const int padding = Scaled(kDialogContentMargin, dpi);
  const int line_h = Scaled(kCheckHeight, dpi);
  const int gap = Scaled(kRowGap, dpi);
  const int button_h = Scaled(kButtonHeight, dpi);
  const int button_w = Scaled(kButtonMinWidth, dpi);
  const int bottom_margin = Scaled(kDialogButtonBottomMargin, dpi);
  const int width = client.right - client.left;
  const int text_w = width - padding * 2;
  int y = padding;

  for (HWND line : {state->credits, state->repo_link, state->discord_link, state->website_link,
                    state->email_link}) {
    appearance::Place(line, padding, y, text_w, line_h);
    y += line_h + gap;
  }

  const int btn_y = y - gap + Scaled(kBlockGap, dpi);
  appearance::Place(state->ok_btn, width - Scaled(kDialogButtonRightMargin, dpi) - button_w,
                    btn_y, button_w, button_h);
  appearance::FitDialogHeight(hwnd, btn_y + button_h + bottom_margin);
}

LRESULT CALLBACK ChoiceDialogProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<ChoiceDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
  case WM_NCCREATE: {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  case WM_CREATE: {
    state = reinterpret_cast<ChoiceDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!state) {
      return -1;
    }
    state->hwnd = hwnd;
    SetWindowTextW(hwnd, state->title.empty() ? kAppTitle : state->title.c_str());
    state->font = DefaultUIFont(win32::DpiForWindow(hwnd));
    if (state->icon_id) {
      state->icon = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ICON, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
      HICON icon = LoadIconW(nullptr, state->icon_id);
      if (state->icon && icon) {
        SendMessageW(state->icon, STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);
      }
    }
    state->text = CreateWindowExW(0, L"STATIC", state->message.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    if (!state->detail.empty()) {
      const int detail_lines = DetailLineCount(state->detail);
      DWORD detail_style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY;
      if (detail_lines > 1) {
        detail_style |= ES_MULTILINE;
      }
      if (detail_lines > kMaxDetailLines) {
        detail_style |= WS_VSCROLL;
      }
      state->detail_edit = CreateWindowExW(0, L"EDIT", state->detail.c_str(),
                                           detail_style,
                                           0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
      state->detail_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                          WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                          CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                          CW_USEDEFAULT, hwnd, nullptr, nullptr, nullptr);
      if (state->detail_tip && state->detail_edit) {
        TOOLINFOW info = {};
        info.cbSize = sizeof(info);
        info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        info.hwnd = hwnd;
        info.uId = reinterpret_cast<UINT_PTR>(state->detail_edit);
        info.lpszText = const_cast<wchar_t*>(state->detail.c_str());
        SendMessageW(state->detail_tip, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&info));
        SendMessageW(state->detail_tip, TTM_SETMAXTIPWIDTH, 0, 900);
        AllowDarkModeForWindow(state->detail_tip, Theme::UseDarkMode());
        SetWindowTheme(state->detail_tip, Theme::UseDarkMode() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
      }
    }
    if (!state->yes_label.empty()) {
      state->yes_btn = CreateWindowExW(0, L"BUTTON", state->yes_label.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDYES), nullptr, nullptr);
    }
    if (!state->no_label.empty()) {
      state->no_btn = CreateWindowExW(0, L"BUTTON", state->no_label.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDNO), nullptr, nullptr);
    }
    if (!state->cancel_label.empty()) {
      state->cancel_btn = CreateWindowExW(0, L"BUTTON", state->cancel_label.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
    }
    state->default_btn = nullptr;
    if (state->default_id == IDYES) {
      state->default_btn = state->yes_btn;
    } else if (state->default_id == IDNO) {
      state->default_btn = state->no_btn;
    } else if (state->default_id == IDCANCEL) {
      state->default_btn = state->cancel_btn;
    }
    if (!state->default_btn) {
      state->default_btn = state->yes_btn ? state->yes_btn
                          : (state->no_btn ? state->no_btn : state->cancel_btn);
    }
    if (state->default_btn) {
      SetWindowLongPtrW(state->default_btn, GWL_STYLE,
                        GetWindowLongPtrW(state->default_btn, GWL_STYLE) | BS_DEFPUSHBUTTON);
    }

    ApplyConfirmFonts(hwnd, state->font);
    Theme::Current().ApplyToWindow(hwnd);
    Theme::Current().ApplyToChildren(hwnd);
    FitChoiceDialogToContent(hwnd, state);
    LayoutChoiceDialog(hwnd, state);
    if (state->default_btn) {
      SetFocus(state->default_btn);
    }
    return 0;
  }
  case WM_DPICHANGED:
    if (state) {
      appearance::RefreshDialogFont(hwnd, &state->font, LOWORD(wparam));
    }
    appearance::ApplyDpiChange(hwnd, lparam);
    return 0;
  case WM_SIZE:
    LayoutChoiceDialog(hwnd, state);
    return 0;
  case WM_SETTINGCHANGE:
    if (Theme::UpdateFromSystem()) {
      Theme::Current().ApplyToWindow(hwnd);
      Theme::Current().ApplyToChildren(hwnd);
      InvalidateRect(hwnd, nullptr, TRUE);
    }
    return 0;
  case WM_ERASEBKGND: {
    HDC hdc = reinterpret_cast<HDC>(wparam);
    RECT rect = {};
    GetClientRect(hwnd, &rect);
    FillRect(hdc, &rect, Theme::Current().BackgroundBrush());
    return TRUE;
  }
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLORDLG:
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
  case WM_CTLCOLORBTN: {
    HDC hdc = reinterpret_cast<HDC>(wparam);
    HWND target = reinterpret_cast<HWND>(lparam);
    int type = CTLCOLOR_STATIC;
    if (msg == WM_CTLCOLOREDIT) {
      type = CTLCOLOR_EDIT;
    } else if (msg == WM_CTLCOLORLISTBOX) {
      type = CTLCOLOR_LISTBOX;
    } else if (msg == WM_CTLCOLORBTN) {
      type = CTLCOLOR_BTN;
    }
    return reinterpret_cast<INT_PTR>(Theme::Current().ControlColor(hdc, target, type));
  }
  case DM_GETDEFID:
    if (state && state->default_btn) {
      return MAKELRESULT(GetDlgCtrlID(state->default_btn), DC_HASDEFID);
    }
    break;
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case IDYES:
      state->result = IDYES;
      state->accepted = true;
      appearance::RestoreDialogOwner(state->owner, &state->owner_restored);
      DestroyWindow(hwnd);
      return 0;
    case IDNO:
      state->result = IDNO;
      state->accepted = true;
      appearance::RestoreDialogOwner(state->owner, &state->owner_restored);
      DestroyWindow(hwnd);
      return 0;
    case IDCANCEL:
      state->result = IDCANCEL;
      state->accepted = true;
      appearance::RestoreDialogOwner(state->owner, &state->owner_restored);
      DestroyWindow(hwnd);
      return 0;
    default:
      break;
    }
    break;
  case WM_DESTROY:
    if (state && state->font) {
      DeleteObject(state->font);
      state->font = nullptr;
    }
    return 0;
  case WM_CLOSE:
    if (state) {
      state->result = IDCANCEL;
      state->accepted = true;
      appearance::RestoreDialogOwner(state->owner, &state->owner_restored);
    }
    DestroyWindow(hwnd);
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK ErrorDialogProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<ErrorDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
  case WM_NCCREATE: {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  case WM_CREATE: {
    state = reinterpret_cast<ErrorDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!state) {
      return -1;
    }
    state->hwnd = hwnd;
    SetWindowTextW(hwnd, state->title.empty() ? kAppTitle : state->title.c_str());
    state->font = DefaultUIFont(win32::DpiForWindow(hwnd));
    state->text = CreateWindowExW(0, L"STATIC", state->message.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    if (!state->detail.empty()) {
      state->detail_box = CreateWindowExW(
          0, L"EDIT", state->detail.c_str(),
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
              ES_MULTILINE | ES_READONLY | ES_AUTOHSCROLL,
          0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    }
    state->ok_btn = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);

    ApplyConfirmFonts(hwnd, state->font);
    Theme::Current().ApplyToWindow(hwnd);
    Theme::Current().ApplyToChildren(hwnd);
    LayoutErrorDialog(hwnd, state);
    return 0;
  }
  case WM_DPICHANGED:
    if (state) {
      appearance::RefreshDialogFont(hwnd, &state->font, LOWORD(wparam));
    }
    appearance::ApplyDpiChange(hwnd, lparam);
    return 0;
  case WM_SIZE:
    LayoutErrorDialog(hwnd, state);
    return 0;
  case WM_SETTINGCHANGE:
    if (Theme::UpdateFromSystem()) {
      Theme::Current().ApplyToWindow(hwnd);
      Theme::Current().ApplyToChildren(hwnd);
      InvalidateRect(hwnd, nullptr, TRUE);
    }
    return 0;
  case WM_ERASEBKGND: {
    HDC hdc = reinterpret_cast<HDC>(wparam);
    RECT rect = {};
    GetClientRect(hwnd, &rect);
    FillRect(hdc, &rect, Theme::Current().BackgroundBrush());
    return TRUE;
  }
  case WM_CTLCOLORDLG: {
    HDC hdc = reinterpret_cast<HDC>(wparam);
    return reinterpret_cast<LRESULT>(Theme::Current().ControlColor(hdc, hwnd, CTLCOLOR_DLG));
  }
  case WM_CTLCOLORSTATIC: {
    HDC hdc = reinterpret_cast<HDC>(wparam);
    HWND target = reinterpret_cast<HWND>(lparam);
    return reinterpret_cast<LRESULT>(Theme::Current().ControlColor(hdc, target, CTLCOLOR_STATIC));
  }
  case WM_CTLCOLORBTN: {
    HDC hdc = reinterpret_cast<HDC>(wparam);
    HWND target = reinterpret_cast<HWND>(lparam);
    return reinterpret_cast<LRESULT>(Theme::Current().ControlColor(hdc, target, CTLCOLOR_BTN));
  }
  case DM_GETDEFID:
    return MAKELRESULT(IDOK, DC_HASDEFID);
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case IDOK:
    case IDCANCEL:
      if (state) {
        state->accepted = true;
        appearance::RestoreDialogOwner(state->owner, &state->owner_restored);
      }
      DestroyWindow(hwnd);
      return 0;
    default:
      break;
    }
    break;
  case WM_DESTROY:
    if (state && state->font) {
      DeleteObject(state->font);
      state->font = nullptr;
    }
    return 0;
  case WM_CLOSE:
    if (state) {
      state->accepted = true;
      appearance::RestoreDialogOwner(state->owner, &state->owner_restored);
    }
    DestroyWindow(hwnd);
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK AboutDialogProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<AboutDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
  case WM_NCCREATE: {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  case WM_CREATE: {
    state = reinterpret_cast<AboutDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!state) {
      return -1;
    }
    state->hwnd = hwnd;
    SetWindowTextW(hwnd, L"About RegKit");
    state->font = DefaultUIFont(win32::DpiForWindow(hwnd));
    state->credits = CreateWindowExW(0, L"STATIC", L"\x00A9 nohuto 2026", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    state->repo_link = CreateWindowExW(0, WC_LINK,
                                       L"Repository: <a href=\"https://github.com/nohuto/regkit\">"
                                       L"https://github.com/nohuto/regkit</a>",
                                       WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    state->discord_link = CreateWindowExW(0, WC_LINK,
                                          L"Discord: <a href=\"https://discord.noverse.dev\">"
                                          L"https://discord.noverse.dev</a>",
                                          WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    state->website_link = CreateWindowExW(0, WC_LINK,
                                          L"Website: <a href=\"https://www.noverse.dev/\">"
                                          L"https://www.noverse.dev/</a>",
                                          WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    state->email_link = CreateWindowExW(0, WC_LINK,
                                        L"Email: <a href=\"mailto:contact@noverse.dev\">"
                                        L"contact@noverse.dev</a>",
                                        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    state->ok_btn = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);

    ApplyConfirmFonts(hwnd, state->font);
    Theme::Current().ApplyToWindow(hwnd);
    Theme::Current().ApplyToChildren(hwnd);
    LayoutAboutDialog(hwnd, state);
    return 0;
  }
  case WM_SHOWWINDOW:
    if (wparam && state && state->ok_btn) {
      SetFocus(state->ok_btn);
      return 0;
    }
    break;
  case WM_SETFOCUS:
    if (state && state->ok_btn) {
      SetFocus(state->ok_btn);
      return 0;
    }
    break;
  case WM_DPICHANGED:
    if (state) {
      appearance::RefreshDialogFont(hwnd, &state->font, LOWORD(wparam));
    }
    appearance::ApplyDpiChange(hwnd, lparam);
    return 0;
  case WM_SIZE:
    LayoutAboutDialog(hwnd, state);
    return 0;
  case WM_SETTINGCHANGE:
    if (Theme::UpdateFromSystem()) {
      Theme::Current().ApplyToWindow(hwnd);
      Theme::Current().ApplyToChildren(hwnd);
      InvalidateRect(hwnd, nullptr, TRUE);
    }
    return 0;
  case WM_ERASEBKGND: {
    HDC hdc = reinterpret_cast<HDC>(wparam);
    RECT rect = {};
    GetClientRect(hwnd, &rect);
    FillRect(hdc, &rect, Theme::Current().BackgroundBrush());
    return TRUE;
  }
  case WM_CTLCOLORDLG:
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLORBTN: {
    HDC hdc = reinterpret_cast<HDC>(wparam);
    HWND target = reinterpret_cast<HWND>(lparam);
    int type = CTLCOLOR_STATIC;
    if (msg == WM_CTLCOLORDLG) {
      type = CTLCOLOR_DLG;
    } else if (msg == WM_CTLCOLORBTN) {
      type = CTLCOLOR_BTN;
    }
    return reinterpret_cast<LRESULT>(Theme::Current().ControlColor(hdc, target, type));
  }
  case WM_NOTIFY: {
    auto* hdr = reinterpret_cast<NMHDR*>(lparam);
    if (hdr && (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
      auto* link = reinterpret_cast<NMLINK*>(lparam);
      if (link && link->item.szUrl[0] != L'\0') {
        ShellExecuteW(hwnd, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
      }
    }
    break;
  }
  case DM_GETDEFID:
    return MAKELRESULT(IDOK, DC_HASDEFID);
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case IDOK:
    case IDCANCEL:
      if (state) {
        state->accepted = true;
        appearance::RestoreDialogOwner(state->owner, &state->owner_restored);
      }
      DestroyWindow(hwnd);
      return 0;
    default:
      break;
    }
    break;
  case WM_DESTROY:
    if (state && state->font) {
      DeleteObject(state->font);
      state->font = nullptr;
    }
    return 0;
  case WM_CLOSE:
    if (state) {
      state->accepted = true;
      appearance::RestoreDialogOwner(state->owner, &state->owner_restored);
    }
    DestroyWindow(hwnd);
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

std::wstring NormalizeLineBreaks(const std::wstring& text) {
  std::wstring output;
  output.reserve(text.size());
  for (size_t index = 0; index < text.size(); ++index) {
    const wchar_t c = text[index];
    if (c == L'\r') {
      continue;
    }
    if (c == L'\n') {
      output.append(L"\r\n");
      continue;
    }
    output.push_back(c);
  }
  return output;
}

bool ShowErrorDialog(HWND owner, const std::wstring& title,
                     const std::wstring& message) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = ErrorDialogProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kErrorClass;
  RegisterClassW(&wc);

  ErrorDialogState state;
  state.owner = owner;
  state.title = title;
  const size_t split = message.find(L'\n');
  if (split == std::wstring::npos) {
    state.message = message;
  } else {
    state.message = message.substr(0, split);
    state.detail = message.substr(split + 1);
    while (!state.detail.empty() &&
           (state.detail.front() == L'\n' || state.detail.front() == L'\r')) {
      state.detail.erase(state.detail.begin());
    }
    state.detail = NormalizeLineBreaks(state.detail);
  }
  if (!state.message.empty() && state.message.back() == L'\r') {
    state.message.pop_back();
  }
  const UINT dpi = win32::DpiForWindow(owner);
  const int width = appearance::metrics::Scaled(state.detail.empty() ? 320 : 520, dpi);
  const int height = appearance::metrics::Scaled(state.detail.empty() ? 120 : 300, dpi);
  HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kErrorClass, title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU | (state.detail.empty() ? 0 : WS_THICKFRAME), CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr, wc.hInstance, &state);
  if (!hwnd) {
    return false;
  }
  appearance::CenterWindow(hwnd, owner);

  EnableWindow(owner, FALSE);
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);

  appearance::RunModalLoop(hwnd);

  appearance::RestoreDialogOwner(owner, &state.owner_restored);
  return state.accepted;
}

bool ShowAboutDialog(HWND owner) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = AboutDialogProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kAboutClass;
  RegisterClassW(&wc);

  AboutDialogState state;
  state.owner = owner;
  HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kAboutClass, L"About RegKit", WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, appearance::metrics::Scaled(460, win32::DpiForWindow(owner)), appearance::metrics::Scaled(240, win32::DpiForWindow(owner)), owner, nullptr, wc.hInstance, &state);
  if (!hwnd) {
    return false;
  }
  appearance::CenterWindow(hwnd, owner);

  EnableWindow(owner, FALSE);
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);

  appearance::RunModalLoop(hwnd);

  appearance::RestoreDialogOwner(owner, &state.owner_restored);
  return state.accepted;
}

bool ShowChoiceDialog(HWND owner, const std::wstring& title, const std::wstring& message, const std::wstring& yes_label, const std::wstring& no_label, const std::wstring& cancel_label, int* result, PCWSTR icon_id, int width, int height, ChoiceButtonWidths button_widths = {}, const std::wstring& detail = std::wstring(), int default_id = 0) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = ChoiceDialogProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kChoiceClass;
  RegisterClassW(&wc);

  ChoiceDialogState state;
  state.owner = owner;
  state.title = title;
  state.message = message;
  state.detail = detail;
  state.yes_label = yes_label;
  state.no_label = no_label;
  state.cancel_label = cancel_label;
  state.icon_id = icon_id;
  state.button_widths = button_widths;
  state.default_id = default_id;
  const UINT dpi = win32::DpiForWindow(owner);
  width = appearance::metrics::Scaled(width, dpi);
  height = appearance::metrics::Scaled(height, dpi);
  HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kChoiceClass, kAppTitle, WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr, wc.hInstance, &state);
  if (!hwnd) {
    return false;
  }
  appearance::CenterWindow(hwnd, owner);

  EnableWindow(owner, FALSE);
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);

  appearance::RunModalLoop(hwnd);

  appearance::RestoreDialogOwner(owner, &state.owner_restored);
  if (!state.accepted) {
    return false;
  }
  if (result) {
    *result = state.result;
  }
  return true;
}

HRESULT CALLBACK TaskDialogCenterCallback(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR ref_data) {
  if (msg == TDN_CREATED) {
    appearance::CenterWindow(hwnd, reinterpret_cast<HWND>(ref_data));
  }
  return S_OK;
}

bool ShowTaskDialog(HWND owner, const std::wstring& title, const std::wstring& message, TASKDIALOG_COMMON_BUTTON_FLAGS buttons, int* button, PCWSTR icon) {
  if (Theme::UseDarkMode()) {
    return false;
  }
  TASKDIALOGCONFIG config = {};
  config.cbSize = sizeof(config);
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
  config.dwCommonButtons = buttons;
  config.pszWindowTitle = title.empty() ? kAppTitle : title.c_str();
  config.pszContent = message.c_str();
  config.pszMainIcon = icon;
  config.pfCallback = TaskDialogCenterCallback;
  config.lpCallbackData = reinterpret_cast<LONG_PTR>(owner);
  int clicked = 0;
  HRESULT hr = TaskDialogIndirect(&config, &clicked, nullptr, nullptr);
  if (FAILED(hr)) {
    return false;
  }
  if (button) {
    *button = clicked;
  }
  return true;
}

} // namespace

bool ListViewItemSelected(HWND list, int item_index) {
  return item_index >= 0 && (ListView_GetItemState(list, item_index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
}

namespace {
void ApplyListViewThemeColors(NMLVCUSTOMDRAW* draw, const Theme& theme) {
  draw->clrText = theme.TextColor();
  draw->clrTextBk = theme.PanelColor();
}
} // namespace

LRESULT HandleThemedListViewCustomDraw(HWND list, NMLVCUSTOMDRAW* draw) {
  if (!list || !draw) {
    return CDRF_DODEFAULT;
  }
  switch (draw->nmcd.dwDrawStage) {
  case CDDS_PREPAINT:
    return CDRF_NOTIFYITEMDRAW;
  case CDDS_ITEMPREPAINT:
    draw->nmcd.uItemState &= ~(CDIS_FOCUS | CDIS_HOT);
    if (draw->nmcd.uItemState & CDIS_SELECTED) {
      return CDRF_DODEFAULT;
    }
    ApplyListViewThemeColors(draw, Theme::Current());
    return CDRF_DODEFAULT;
  default:
    break;
  }
  return CDRF_DODEFAULT;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
  if (!OpenClipboard(owner)) {
    return false;
  }
  EmptyClipboard();
  size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!memory) {
    CloseClipboard();
    return false;
  }
  void* data = GlobalLock(memory);
  if (!data) {
    GlobalFree(memory);
    CloseClipboard();
    return false;
  }
  memcpy(data, text.c_str(), bytes);
  GlobalUnlock(memory);
  const bool copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
  if (!copied) {
    GlobalFree(memory);
  }
  CloseClipboard();
  return copied;
}

void ShowError(HWND owner, const std::wstring& message) {
  if (!ShowErrorDialog(owner, L"Error", message)) {
    ShowTaskDialog(owner, L"Error", message, TDCBF_OK_BUTTON, nullptr, TD_ERROR_ICON);
  }
}

void ShowWarning(HWND owner, const std::wstring& message) {
  if (!ShowErrorDialog(owner, L"Warning", message)) {
    ShowTaskDialog(owner, L"Warning", message, TDCBF_OK_BUTTON, nullptr, TD_WARNING_ICON);
  }
}

void ShowInfo(HWND owner, const std::wstring& message) {
  if (!ShowErrorDialog(owner, L"Information", message)) {
    ShowTaskDialog(owner, L"Information", message, TDCBF_OK_BUTTON, nullptr, TD_INFORMATION_ICON);
  }
}

void ShowAbout(HWND owner) {
  if (ShowAboutDialog(owner)) {
    return;
  }
  ShowInfo(owner, L"\x00A9 nohuto 2026\n"
                  L"Repository: https://github.com/nohuto/regkit\n"
                  L"Discord: https://discord.noverse.dev\n"
                  L"Website: https://www.noverse.dev/\n"
                  L"Email: contact@noverse.dev");
}

bool ConfirmRegFileMerge(HWND owner, const std::wstring& path) {
  const std::wstring message =
      L"Adding information can unintentionally change or delete values and\n"
      L"cause components to stop working correctly. If you don't trust the\n"
      L"source of this information, don't add it to the registry.\n\n"
      L"Are you sure you want to continue?";
  int result = IDCANCEL;
  if (ShowChoiceDialog(owner, kAppTitle, message, L"Yes", L"No", L"", &result, IDI_WARNING, 560, 232, {}, path)) {
    return result == IDYES;
  }
  int clicked = 0;
  const std::wstring plain = message + L"\n\n" + path;
  if (ShowTaskDialog(owner, kAppTitle, plain, TDCBF_YES_BUTTON | TDCBF_NO_BUTTON, &clicked, TD_WARNING_ICON)) {
    return clicked == IDYES;
  }
  return false;
}

void ShowRegFileMergeSucceeded(HWND owner, const std::wstring& path) {
  const std::wstring message =
      L"The keys and values it contains have been added to the registry.";
  int result = IDCANCEL;
  if (ShowChoiceDialog(owner, kAppTitle, message, L"OK", L"", L"", &result, IDI_INFORMATION, 520, 182, {}, path)) {
    return;
  }
  const std::wstring plain = message + L"\n\n" + path;
  if (!ShowTaskDialog(owner, kAppTitle, plain, TDCBF_OK_BUTTON, nullptr, TD_INFORMATION_ICON)) {
    ShowInfo(owner, plain);
  }
}

void ShowRegFileMergeFailed(HWND owner, const std::wstring& path, const std::wstring& detail) {
  std::wstring message = L"The registry file couldn't be imported.";
  if (!detail.empty()) {
    message += L"\n\n";
    message += detail;
  }
  int result = IDCANCEL;
  if (ShowChoiceDialog(owner, kAppTitle, message, L"OK", L"", L"", &result, IDI_ERROR, 520, 212, {}, path)) {
    return;
  }
  const std::wstring plain = message + L"\n\n" + path;
  if (!ShowTaskDialog(owner, kAppTitle, plain, TDCBF_OK_BUTTON, nullptr, TD_ERROR_ICON)) {
    ShowError(owner, plain);
  }
}


bool ConfirmDelete(HWND owner, const std::wstring& title,
                   const std::vector<std::wstring>& names,
                   const std::wstring& override_message) {
  if (names.empty()) {
    return false;
  }
  const bool many = names.size() > 1;
  std::wstring message = override_message;
  if (!message.empty()) {
  } else if (_wcsicmp(title.c_str(), L"Delete Key") == 0) {
    message = many ? L"Delete these keys and all of their subkeys?"
                   : L"Delete this key and all of its subkeys?";
  } else if (_wcsnicmp(title.c_str(), L"Delete Value", 12) == 0) {
    message = many ? L"Delete these values?" : L"Delete this value?";
  } else {
    message = many ? L"Delete these items?" : L"Delete this item?";
  }

  std::wstring detail;
  for (const std::wstring& name : names) {
    if (!detail.empty()) {
      detail.append(L"\r\n");
    }
    detail.append(name.empty() ? L"(Default)" : name);
  }

  const int lines = std::min(static_cast<int>(names.size()), kMaxDetailLines);
  int result = IDCANCEL;
  if (ShowChoiceDialog(owner, title, message, L"Delete", L"", L"Cancel", &result,
                       nullptr, 460, 128 + (lines - 1) * 16, {}, detail)) {
    return result == IDYES;
  }
  return false;
}

bool ConfirmDelete(HWND owner, const std::wstring& title, const std::wstring& name,
                   const std::wstring& override_message) {
  return ConfirmDelete(owner, title, std::vector<std::wstring>{name}, override_message);
}


int PromptKeyChoice(HWND owner, const std::wstring& message, const std::wstring& key_path, const std::wstring& title, const std::wstring& yes_label, const std::wstring& no_label, const std::wstring& cancel_label, ChoiceButtonWidths widths) {
  int result = IDCANCEL;
  if (ShowChoiceDialog(owner, title, message, yes_label, no_label, cancel_label,
                       &result, nullptr, 560, 140, widths, key_path)) {
    return result;
  }
  return IDCANCEL;
}

int PromptChoice(HWND owner, const std::wstring& message, const std::wstring& title, const std::wstring& yes_label, const std::wstring& no_label, const std::wstring& cancel_label, ChoiceButtonWidths widths, int width) {
  int result = IDCANCEL;
  if (ShowChoiceDialog(owner, title, message, yes_label, no_label, cancel_label, &result, nullptr, width, 120, widths)) {
    return result;
  }
  return IDCANCEL;
}

bool ReportFileDialogResult(HWND owner, HRESULT hr) {
  if (FAILED(hr) && !win32::DialogCancelled(hr)) {
    ShowError(owner, win32::FormatDialogError(hr));
  }
  return SUCCEEDED(hr);
}

bool LaunchNewInstance() {
  std::wstring exe = util::JoinPath(util::GetModuleDirectory(), L"RegKit.exe");
  if (exe.empty()) {
    return false;
  }
  HINSTANCE result = ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<intptr_t>(result) > 32;
}

} // namespace regkit::ui
