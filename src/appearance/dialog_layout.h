// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <initializer_list>
#include <vector>

namespace regkit::appearance {

void SetControlFont(HWND control, HFONT font);
void SetDialogFont(HWND dialog, HFONT font);
void Place(HWND control, int x, int y, int width, int height);
void RestoreDialogOwner(HWND owner, bool* restored);
void CenterWindow(HWND window, HWND owner);
void ApplyDpiChange(HWND window, LPARAM suggested_rect);
void RefreshDialogFont(HWND window, HFONT* owned_font, UINT dpi);
void RunModalLoop(HWND dialog);
void CenterEditText(HWND edit, HFONT font, int left_pad, int right_pad);
void FitDialogHeight(HWND dialog, int client_height);

enum AnchorFlags : unsigned {
  kAnchorLeft = 1u,
  kAnchorTop = 2u,
  kAnchorRight = 4u,
  kAnchorBottom = 8u,
};

struct AnchorRule {
  int id = 0;
  unsigned anchors = kAnchorLeft | kAnchorTop;
};

class DialogResizer {
public:
  void Attach(HWND dialog, std::initializer_list<AnchorRule> rules);
  void Apply(HWND dialog) const;
  void ClampMinSize(MINMAXINFO* info) const;

private:
  struct Item {
    int id = 0;
    unsigned anchors = 0;
    RECT rect = {};
  };

  std::vector<Item> items_;
  SIZE client_ = {};
  SIZE min_window_ = {};
};

void AttachThemedBorder(HWND control);
void DetachThemedBorder(HWND control);

struct DialogWindow {
  HWND hwnd = nullptr;
  HWND owner = nullptr;
  HFONT font = nullptr;
  HWND focus = nullptr;
  int default_id = IDOK;
  bool accepted = false;
  bool owner_restored = false;
};

template <typename State>
State* DialogWindowState(
    HWND hwnd
) {
  return static_cast<State*>(reinterpret_cast<DialogWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
}

SIZE DialogWindowSize(HWND owner, int client_width, int client_height, DWORD extra_style = 0);
void ApplyDialogTheme(HWND dialog);
void CloseDialogWindow(DialogWindow* dialog, bool accepted);
LRESULT DefDialogWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
bool RunDialogWindow(DialogWindow* dialog, const wchar_t* class_name, WNDPROC proc, const wchar_t* title, SIZE size, DWORD extra_style = 0);

} // namespace regkit::appearance
