// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <array>

namespace regkit::ui
{

// themed painting, hover and close buttons for a tab control, the owner keeps the tabs themselves
class TabStrip
{
  public:
    using CloseCallback = void (*)(void* context, int index);

    void Attach(HWND tab, CloseCallback on_close, void* context);
    // pads the tabs for their close buttons and returns the height one tab row needs
    int Refit(int min_width) const;
    // rows while they fit in max_height, else one row scrolled by the control's arrows; returns the strip height
    int Fit(int width, int row_height, int max_height);

  private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR ref_data);
    int RowsHeight(int width, int row_height) const;
    void SetMultiline(bool multiline) const;
    void UpdateHot(POINT point);
    bool CloseRect(int index, RECT* rect) const;
    void Paint(HDC hdc) const;
    void DrawItem(HDC hdc, int index, const RECT& item_rect, bool selected) const;

    HWND tab_ = nullptr;
    CloseCallback on_close_ = nullptr;
    void* context_ = nullptr;
    int hot_ = -1;
    int close_hot_ = -1;
    int close_down_ = -1;
    bool tracking_ = false;
    // the last Fit's inputs & height, a layout pass with the same inputs keeps the mode without toggling it
    std::array<int, 5> fit_inputs_ = {};
    int fit_height_ = 0;
};

// an x centered in rect, as used by every close button
void DrawCloseGlyph(HDC hdc, const RECT& rect, COLORREF color, UINT dpi);

} // namespace regkit::ui
