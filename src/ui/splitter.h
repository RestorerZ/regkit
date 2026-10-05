// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <algorithm>

namespace regkit::ui
{

// a draggable bar between two panes, the owner's layout sets its rect and applies the size
class Splitter
{
  public:
    Splitter(bool vertical, bool grows_backward) noexcept : vertical_(vertical), backward_(grows_backward)
    {
    }

    RECT rect = {};

    bool dragging() const noexcept
    {
        return dragging_;
    }

    bool Hit(POINT point) const noexcept
    {
        return PtInRect(&rect, point) != FALSE;
    }

    HCURSOR Cursor() const
    {
        return LoadCursorW(nullptr, vertical_ ? IDC_SIZEWE : IDC_SIZENS);
    }

    void Begin(HWND owner, POINT point, int size, int minimum, int maximum)
    {
        start_ = Coordinate(point);
        size_ = size;
        minimum_ = minimum;
        maximum_ = std::max(minimum, maximum);
        dragging_ = true;
        SetCapture(owner);
    }

    int Track(POINT point) const noexcept
    {
        const int delta = Coordinate(point) - start_;
        return std::clamp(size_ + (backward_ ? -delta : delta), minimum_, maximum_);
    }

    bool End(HWND owner)
    {
        if (!dragging_)
        {
            return false;
        }
        dragging_ = false;
        if (GetCapture() == owner)
        {
            ReleaseCapture();
        }
        return true;
    }

    // the current pen draws the grip line
    void Paint(HDC dc, HBRUSH fill) const
    {
        if (IsRectEmpty(&rect))
        {
            return;
        }
        FillRect(dc, &rect, fill);
        const int middle = vertical_ ? (rect.left + rect.right) / 2 : (rect.top + rect.bottom) / 2;
        MoveToEx(dc, vertical_ ? middle : rect.left + 4, vertical_ ? rect.top + 4 : middle, nullptr);
        LineTo(dc, vertical_ ? middle : rect.right - 4, vertical_ ? rect.bottom - 4 : middle);
    }

  private:
    int Coordinate(POINT point) const noexcept
    {
        return vertical_ ? point.x : point.y;
    }

    bool vertical_;
    bool backward_;
    bool dragging_ = false;
    int start_ = 0;
    int size_ = 0;
    int minimum_ = 0;
    int maximum_ = 0;
};

} // namespace regkit::ui
