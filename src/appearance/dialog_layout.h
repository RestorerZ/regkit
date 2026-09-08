// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <initializer_list>
#include <vector>

namespace regkit::appearance {

void SetControlFont(HWND control, HFONT font);
void Place(HWND control, int x, int y, int width, int height);
void RestoreDialogOwner(HWND owner, bool* restored);
void PositionDialog(HWND dialog, HWND owner, int width, int height);
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

} // namespace regkit::appearance
