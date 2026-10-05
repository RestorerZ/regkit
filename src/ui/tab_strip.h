// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

namespace regkit::ui
{

// themed painting, hover and close buttons for a tab control, the owner keeps the tabs themselves
class TabStrip
{
  public:
    using CloseCallback = void (*)(void* context, int index);

    void Attach(HWND tab, CloseCallback on_close, void* context);
    // pads the tabs for their close buttons and returns the height the tab row needs
    int Refit(int min_width) const;

  private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR ref_data);
    void UpdateHot(POINT point);
    bool CloseRect(int index, RECT* rect) const;
    void Paint(HDC hdc) const;
    void DrawItem(HDC hdc, int index, const RECT& item_rect, int header_bottom, bool selected) const;

    HWND tab_ = nullptr;
    CloseCallback on_close_ = nullptr;
    void* context_ = nullptr;
    int hot_ = -1;
    int close_hot_ = -1;
    int close_down_ = -1;
    bool tracking_ = false;
};

// an x centered in rect, as used by every close button
void DrawCloseGlyph(HDC hdc, const RECT& rect, COLORREF color, UINT dpi);

} // namespace regkit::ui
