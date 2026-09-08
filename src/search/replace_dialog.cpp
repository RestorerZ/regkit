// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search/replace_dialog.h"

#include <algorithm>

#include <commctrl.h>
#include <uxtheme.h>

#include "appearance/dialog_layout.h"
#include "appearance/dialog_metrics.h"
#include "search/query_dialog.h"
#include "appearance/theme.h"
#include "appearance/default_font.h"
#include "appearance/feedback.h"

namespace regkit {

namespace {

constexpr wchar_t kDialogClass[] = L"RegKitReplaceDialog";

enum ControlId {
  kFindLabel = 100,
  kFindEdit = 101,
  kReplaceLabel = 102,
  kReplaceEdit = 103,
  kWhereGroup = 110,
  kKeyLabel = 111,
  kKeyEdit = 112,
  kKeyBrowse = 113,
  kOptionsGroup = 120,
  kRecursive = 121,
  kMatchCase = 122,
  kMatchWhole = 123,
  kUseRegex = 124,
  kSearchKeys = 125,
  kSearchValues = 126,
  kSearchData = 127,
  kReplaceButton = IDOK,
  kCancelButton = IDCANCEL,
};

struct ReplaceDialogState {
  HWND hwnd = nullptr;
  HWND find_edit = nullptr;
  HWND replace_edit = nullptr;
  HWND key_edit = nullptr;
  HWND key_browse = nullptr;
  HWND recursive = nullptr;
  HWND match_case = nullptr;
  HWND match_whole = nullptr;
  HWND use_regex = nullptr;
  HWND search_keys = nullptr;
  HWND search_values = nullptr;
  HWND search_data = nullptr;
  HWND replace_button = nullptr;
  HWND cancel_button = nullptr;
  HWND owner = nullptr;
  HFONT font = nullptr;
  ReplaceDialogResult* out = nullptr;
  bool accepted = false;
  bool owner_restored = false;
};

HFONT CreateDialogFont() {
  return ui::DefaultUIFont();
}


void CenterWindowToOwner(HWND hwnd, HWND owner) {
  if (!hwnd) {
    return;
  }
  RECT rect = {};
  if (!GetWindowRect(hwnd, &rect)) {
    return;
  }
  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;
  RECT owner_rect = {};
  if (owner && GetWindowRect(owner, &owner_rect)) {
    int owner_w = owner_rect.right - owner_rect.left;
    int owner_h = owner_rect.bottom - owner_rect.top;
    int x = owner_rect.left + std::max(0, (owner_w - width) / 2);
    int y = owner_rect.top + std::max(0, (owner_h - height) / 2);
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
  }
}

void LayoutDialog(HWND hwnd, ReplaceDialogState* state, HFONT font) {
  if (!hwnd || !state) {
    return;
  }
  using namespace appearance::metrics;
  RECT client = {};
  GetClientRect(hwnd, &client);
  int width = client.right - client.left;
  int x = kMargin;
  int y = kMargin;
  int label_w = 90;
  int key_label_w = 32;
  int line_h = kControlHeight;
  auto place_check = [&](HWND check, int x_pos, int y_pos, int width) {
    if (check) {
      SetWindowPos(check, nullptr, x_pos, y_pos, width, kCheckHeight, SWP_NOZORDER);
    }
  };

  HWND find_label = GetDlgItem(hwnd, kFindLabel);
  SetWindowPos(find_label, nullptr, x, y + kLabelInset, label_w, kLabelHeight, SWP_NOZORDER);
  int edit_w = width - x * 2 - label_w - kLabelGap;
  SetWindowPos(state->find_edit, nullptr, x + label_w + kLabelGap, y, edit_w, line_h, SWP_NOZORDER);
  y += kControlPitch;

  HWND replace_label = GetDlgItem(hwnd, kReplaceLabel);
  SetWindowPos(replace_label, nullptr, x, y + kLabelInset, label_w, kLabelHeight, SWP_NOZORDER);
  SetWindowPos(state->replace_edit, nullptr, x + label_w + kLabelGap, y, edit_w, line_h, SWP_NOZORDER);
  y += line_h + kBlockGap;

  int group_w = width - x * 2;
  int where_h = kGroupTop + line_h + kGroupBottom;
  SetWindowPos(GetDlgItem(hwnd, kWhereGroup), nullptr, x, y, group_w, where_h, SWP_NOZORDER);
  int gx = x + kGroupInset;
  int gy = y + kGroupTop;
  int browse_w = 80;
  int key_w = group_w - kGroupInset * 2 - key_label_w - kLabelGap * 2 - browse_w;
  SetWindowPos(GetDlgItem(hwnd, kKeyLabel), nullptr, gx, gy + kLabelInset, key_label_w, kLabelHeight, SWP_NOZORDER);
  SetWindowPos(state->key_edit, nullptr, gx + key_label_w + kLabelGap, gy, key_w, line_h, SWP_NOZORDER);
  SetWindowPos(state->key_browse, nullptr, gx + key_label_w + kLabelGap * 2 + key_w, gy, browse_w, line_h, SWP_NOZORDER);
  y += where_h + kBlockGap;

  int options_h = kGroupTop + kRowPitch * 3 + kCheckHeight + kGroupBottom;
  SetWindowPos(GetDlgItem(hwnd, kOptionsGroup), nullptr, x, y, group_w, options_h, SWP_NOZORDER);
  int ox = x + kGroupInset;
  int oy = y + kGroupTop;
  int col_w = (group_w - kGroupInset * 2 - kLabelGap) / 2;
  int option_col2_x = ox + col_w + kLabelGap;
  place_check(state->recursive, ox, oy, col_w);
  place_check(state->match_case, option_col2_x, oy, col_w);
  place_check(state->match_whole, ox, oy + kRowPitch, col_w);
  place_check(state->use_regex, option_col2_x, oy + kRowPitch, col_w);
  place_check(state->search_keys, ox, oy + kRowPitch * 2, col_w);
  place_check(state->search_values, option_col2_x, oy + kRowPitch * 2, col_w);
  place_check(state->search_data, ox, oy + kRowPitch * 3, col_w);
  y += options_h + kBlockGap;

  int btn_y = y;
  int cancel_x = width - kMargin - kButtonWidth;
  int replace_x = cancel_x - kButtonGap - kButtonWidth;
  SetWindowPos(state->replace_button, nullptr, replace_x, btn_y, kButtonWidth, kButtonHeight, SWP_NOZORDER);
  SetWindowPos(state->cancel_button, nullptr, cancel_x, btn_y, kButtonWidth, kButtonHeight, SWP_NOZORDER);
  appearance::FitDialogHeight(hwnd, btn_y + kButtonHeight + kMargin);

  appearance::SetControlFont(hwnd, font);
  appearance::SetControlFont(find_label, font);
  appearance::SetControlFont(replace_label, font);
  for (HWND edit : {state->find_edit, state->replace_edit, state->key_edit}) {
    appearance::CenterEditText(edit, font, 2, 2);
  }
}

LRESULT CALLBACK ReplaceDialogProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<ReplaceDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
  case WM_NCCREATE: {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  case WM_CREATE: {
    state = reinterpret_cast<ReplaceDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!state) {
      return -1;
    }
    state->hwnd = hwnd;
    SetWindowTextW(hwnd, L"Replace");
    state->font = CreateDialogFont();
    HFONT font = state->font;

    CreateWindowExW(0, L"STATIC", L"Find what:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kFindLabel), nullptr, nullptr);
    state->find_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_MULTILINE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kFindEdit), nullptr, nullptr);

    CreateWindowExW(0, L"STATIC", L"Replace with:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kReplaceLabel), nullptr, nullptr);
    state->replace_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_MULTILINE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kReplaceEdit), nullptr, nullptr);

    CreateWindowExW(0, L"BUTTON", L"Where to search", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kWhereGroup), nullptr, nullptr);
    CreateWindowExW(0, L"STATIC", L"Key:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kKeyLabel), nullptr, nullptr);
    state->key_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_MULTILINE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kKeyEdit), nullptr, nullptr);
    state->key_browse = CreateWindowExW(0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kKeyBrowse), nullptr, nullptr);

    CreateWindowExW(0, L"BUTTON", L"Options", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptionsGroup), nullptr, nullptr);
    state->recursive = CreateWindowExW(0, L"BUTTON", L"Recursive", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kRecursive), nullptr, nullptr);
    state->match_case = CreateWindowExW(0, L"BUTTON", L"Match case", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kMatchCase), nullptr, nullptr);
    state->match_whole = CreateWindowExW(0, L"BUTTON", L"Match whole string", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kMatchWhole), nullptr, nullptr);
    state->use_regex = CreateWindowExW(0, L"BUTTON", L"Use regular expressions", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kUseRegex), nullptr, nullptr);
    state->search_keys = CreateWindowExW(0, L"BUTTON", L"Replace in key names", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kSearchKeys), nullptr, nullptr);
    state->search_values = CreateWindowExW(0, L"BUTTON", L"Replace in value names", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kSearchValues), nullptr, nullptr);
    state->search_data = CreateWindowExW(0, L"BUTTON", L"Replace in value data", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kSearchData), nullptr, nullptr);

    state->replace_button = CreateWindowExW(0, L"BUTTON", L"Replace", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kReplaceButton), nullptr, nullptr);
    state->cancel_button = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kCancelButton), nullptr, nullptr);

    if (state->out) {
      SetWindowTextW(state->find_edit, state->out->find_text.c_str());
      SetWindowTextW(state->replace_edit, state->out->replace_text.c_str());
      SetWindowTextW(state->key_edit, state->out->start_key.c_str());
      SendMessageW(state->recursive, BM_SETCHECK, state->out->recursive ? BST_CHECKED : BST_UNCHECKED, 0);
      SendMessageW(state->match_case, BM_SETCHECK, state->out->match_case ? BST_CHECKED : BST_UNCHECKED, 0);
      SendMessageW(state->match_whole, BM_SETCHECK, state->out->match_whole ? BST_CHECKED : BST_UNCHECKED, 0);
      SendMessageW(state->use_regex, BM_SETCHECK, state->out->use_regex ? BST_CHECKED : BST_UNCHECKED, 0);
      SendMessageW(state->search_keys, BM_SETCHECK, state->out->replace_keys ? BST_CHECKED : BST_UNCHECKED, 0);
      SendMessageW(state->search_values, BM_SETCHECK, state->out->replace_values ? BST_CHECKED : BST_UNCHECKED, 0);
      SendMessageW(state->search_data, BM_SETCHECK, state->out->replace_data ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
      SendMessageW(state->recursive, BM_SETCHECK, BST_CHECKED, 0);
      SendMessageW(state->search_values, BM_SETCHECK, BST_CHECKED, 0);
      SendMessageW(state->search_data, BM_SETCHECK, BST_CHECKED, 0);
    }

    EnumChildWindows(
        hwnd,
        [](HWND child, LPARAM param) -> BOOL {
          HFONT font_handle = reinterpret_cast<HFONT>(param);
          appearance::SetControlFont(child, font_handle);
          return TRUE;
        },
        reinterpret_cast<LPARAM>(font));

    Theme::Current().ApplyToWindow(hwnd);
    Theme::Current().ApplyToChildren(hwnd);
    LayoutDialog(hwnd, state, font);
    return 0;
  }
  case WM_DESTROY:
    if (state && state->font) {
      DeleteObject(state->font);
      state->font = nullptr;
    }
    return 0;
  case WM_SIZE: {
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    LayoutDialog(hwnd, state, font);
    return 0;
  }
  case WM_ERASEBKGND: {
    HDC hdc = reinterpret_cast<HDC>(wparam);
    RECT rect = {};
    GetClientRect(hwnd, &rect);
    FillRect(hdc, &rect, Theme::Current().BackgroundBrush());
    return 1;
  }
  case WM_SETTINGCHANGE: {
    if (Theme::UpdateFromSystem()) {
      Theme::Current().ApplyToWindow(hwnd);
      Theme::Current().ApplyToChildren(hwnd);
      InvalidateRect(hwnd, nullptr, TRUE);
    }
    return 0;
  }
  case WM_CTLCOLORSTATIC: {
    HDC hdc = reinterpret_cast<HDC>(wparam);
    HWND target = reinterpret_cast<HWND>(lparam);
    return reinterpret_cast<LRESULT>(Theme::Current().ControlColor(hdc, target, CTLCOLOR_STATIC));
  }
  case WM_CTLCOLOREDIT: {
    HDC hdc = reinterpret_cast<HDC>(wparam);
    HWND target = reinterpret_cast<HWND>(lparam);
    return reinterpret_cast<LRESULT>(Theme::Current().ControlColor(hdc, target, CTLCOLOR_EDIT));
  }
  case WM_CTLCOLORBTN: {
    HDC hdc = reinterpret_cast<HDC>(wparam);
    HWND target = reinterpret_cast<HWND>(lparam);
    return reinterpret_cast<LRESULT>(Theme::Current().ControlColor(hdc, target, CTLCOLOR_BTN));
  }
  case DM_GETDEFID:
    return MAKELRESULT(IDOK, DC_HASDEFID);
  case WM_COMMAND: {
    if (!state) {
      return 0;
    }
    switch (LOWORD(wparam)) {
    case kKeyBrowse: {
      std::wstring selected;
      if (ShowBrowseKeyDialog(hwnd, &selected)) {
        if (!selected.empty()) {
          SetWindowTextW(state->key_edit, selected.c_str());
        }
      }
      return 0;
    }
    case kReplaceButton: {
      wchar_t find_text[512] = {};
      GetWindowTextW(state->find_edit, find_text, static_cast<int>(_countof(find_text)));
      std::wstring find_value = find_text;
      if (find_value.empty()) {
        ui::ShowError(hwnd, L"Enter text to find.");
        return 0;
      }
      wchar_t replace_text[512] = {};
      GetWindowTextW(state->replace_edit, replace_text, static_cast<int>(_countof(replace_text)));
      wchar_t key_text[512] = {};
      GetWindowTextW(state->key_edit, key_text, static_cast<int>(_countof(key_text)));

      if (state->out) {
        state->out->find_text = find_value;
        state->out->replace_text = replace_text;
        state->out->start_key = key_text;
        state->out->recursive = SendMessageW(state->recursive, BM_GETCHECK, 0, 0) == BST_CHECKED;
        state->out->match_case = SendMessageW(state->match_case, BM_GETCHECK, 0, 0) == BST_CHECKED;
        state->out->match_whole = SendMessageW(state->match_whole, BM_GETCHECK, 0, 0) == BST_CHECKED;
        state->out->use_regex = SendMessageW(state->use_regex, BM_GETCHECK, 0, 0) == BST_CHECKED;
        state->out->replace_keys = SendMessageW(state->search_keys, BM_GETCHECK, 0, 0) == BST_CHECKED;
        state->out->replace_values = SendMessageW(state->search_values, BM_GETCHECK, 0, 0) == BST_CHECKED;
        state->out->replace_data = SendMessageW(state->search_data, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!state->out->replace_keys && !state->out->replace_values &&
            !state->out->replace_data) {
          ui::ShowError(hwnd, L"Select what should be replaced.");
          return 0;
        }
      }
      state->accepted = true;
      appearance::RestoreDialogOwner(state->owner, &state->owner_restored);
      DestroyWindow(hwnd);
      return 0;
    }
    case kCancelButton:
      appearance::RestoreDialogOwner(state->owner, &state->owner_restored);
      DestroyWindow(hwnd);
      return 0;
    default:
      break;
    }
    break;
  }
  case WM_CLOSE:
    if (state) {
      appearance::RestoreDialogOwner(state->owner, &state->owner_restored);
    }
    DestroyWindow(hwnd);
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND CreateReplaceDialogWindow(HINSTANCE instance, HWND owner, ReplaceDialogState* state) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = ReplaceDialogProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kDialogClass;
  RegisterClassW(&wc);

  return CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kDialogClass, L"Replace", WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 520, 360, owner, nullptr, instance, state);
}

} // namespace

bool ShowReplaceDialog(HWND owner, ReplaceDialogResult* result) {
  if (!result) {
    return false;
  }
  HINSTANCE instance = GetModuleHandleW(nullptr);
  ReplaceDialogState state;
  state.out = result;
  state.owner = owner;
  HWND hwnd = CreateReplaceDialogWindow(instance, owner, &state);
  if (!hwnd) {
    return false;
  }
  SetWindowTextW(hwnd, L"Replace");

  Theme::Current().ApplyToWindow(hwnd);
  CenterWindowToOwner(hwnd, owner);

  EnableWindow(owner, FALSE);
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);

  appearance::RunModalLoop(hwnd);

  appearance::RestoreDialogOwner(owner, &state.owner_restored);
  return state.accepted;
}

} // namespace regkit
