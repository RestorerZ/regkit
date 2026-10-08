// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/tab_strip.h"

#include "ui/gdi_cache.h"
#include "ui/icon_loader.h"
#include "ui/theme.h"
#include "win32/window_metrics.h"

#include <algorithm>

#include <commctrl.h>
#include <windowsx.h>

namespace regkit::ui
{

namespace
{

// button tabs keep their own gaps between tabs & rows and a taller frame, so no insets and less padding
constexpr int kPadY = 2;
constexpr int kTextPaddingX = 10;
constexpr int kCloseSize = 14;
constexpr int kCloseGap = 6;

bool CloseButtonRect(const RECT& item_rect, UINT dpi, RECT* close_rect)
{
    const int height = item_rect.bottom - item_rect.top;
    const int size = std::min(appearance::ScaleForDpi(kCloseSize, dpi), std::max(8, height - 6));
    close_rect->right = item_rect.right - appearance::ScaleForDpi(kCloseGap, dpi);
    close_rect->left = close_rect->right - size;
    close_rect->top = item_rect.top + (height - size) / 2;
    close_rect->bottom = close_rect->top + size;
    return close_rect->left < close_rect->right;
}

} // namespace

void DrawCloseGlyph(HDC hdc, const RECT& rect, COLORREF color, UINT dpi)
{
    const int radius = appearance::ScaleForDpi(3, dpi);
    const int pen_width = std::max(1, appearance::ScaleForDpi(1, dpi));
    const int center_x = (rect.left + rect.right) / 2;
    const int center_y = (rect.top + rect.bottom) / 2;
    HGDIOBJ old_pen = SelectObject(hdc, appearance::CachedPen(color, pen_width));
    MoveToEx(hdc, center_x - radius, center_y - radius, nullptr);
    LineTo(hdc, center_x + radius + 1, center_y + radius + 1);
    MoveToEx(hdc, center_x + radius, center_y - radius, nullptr);
    LineTo(hdc, center_x - radius - 1, center_y + radius + 1);
    SelectObject(hdc, old_pen);
}

void TabStrip::Attach(HWND tab, CloseCallback on_close, void* context)
{
    tab_ = tab;
    on_close_ = on_close;
    context_ = context;
    TabCtrl_SetPadding(tab_, kTextPaddingX, kPadY);
    SetWindowSubclass(tab_, Proc, 0, reinterpret_cast<DWORD_PTR>(this));
}

int TabStrip::Refit(int min_width) const
{
    const UINT dpi = win32::DpiForWindow(tab_);
    TabCtrl_SetPadding(tab_, appearance::ScaleForDpi(kTextPaddingX + (TabCtrl_GetItemCount(tab_) > 1 ? kCloseSize + kCloseGap : 0), dpi), kPadY);
    SendMessageW(tab_, TCM_SETMINTABWIDTH, 0, min_width);
    TEXTMETRICW metrics = {};
    if (HDC hdc = GetDC(tab_))
    {
        HGDIOBJ old_font = SelectObject(hdc, reinterpret_cast<HFONT>(SendMessageW(tab_, WM_GETFONT, 0, 0)));
        GetTextMetricsW(hdc, &metrics);
        SelectObject(hdc, old_font);
        ReleaseDC(tab_, hdc);
    }
    InvalidateRect(tab_, nullptr, FALSE);
    const int min_height = std::max<int>(24, metrics.tmHeight + kPadY * 2 + 2);
    RECT item_rect = {};
    return TabCtrl_GetItemRect(tab_, 0, &item_rect) ? std::max<int>(min_height, item_rect.bottom - item_rect.top) : min_height;
}

int TabStrip::Fit(int width, int row_height, int max_height)
{
    const int count = TabCtrl_GetItemCount(tab_);
    int widths = 0;
    for (int i = 0; i < count; ++i)
    {
        RECT item_rect = {};
        if (TabCtrl_GetItemRect(tab_, i, &item_rect))
        {
            widths += item_rect.right - item_rect.left;
        }
    }
    const std::array<int, 5> inputs = {width, row_height, max_height, count, widths};
    if (inputs == fit_inputs_)
    {
        return fit_height_;
    }
    fit_inputs_ = inputs;
    SetMultiline(true);
    fit_height_ = RowsHeight(width, row_height);
    if (fit_height_ > max_height)
    {
        // more rows than the window can spare, the rest of the window stays for the panes
        SetMultiline(false);
        fit_height_ = row_height;
        // reselect scrolls the one row to the selected tab, TCM_SETCURSEL sends no change notifications
        if (const int current = TabCtrl_GetCurSel(tab_); current > 0)
        {
            TabCtrl_SetCurSel(tab_, 0);
            TabCtrl_SetCurSel(tab_, current);
        }
    }
    return fit_height_;
}

void TabStrip::SetMultiline(bool multiline) const
{
    const LONG_PTR style = GetWindowLongPtrW(tab_, GWL_STYLE);
    const LONG_PTR wanted = multiline ? style | TCS_MULTILINE : style & ~static_cast<LONG_PTR>(TCS_MULTILINE);
    if (wanted != style)
    {
        SetWindowLongPtrW(tab_, GWL_STYLE, wanted);
        SetWindowPos(tab_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

int TabStrip::RowsHeight(int width, int row_height) const
{
    RECT window = {};
    GetWindowRect(tab_, &window);
    if (window.right - window.left != width)
    {
        SetWindowPos(tab_, nullptr, 0, 0, width, window.bottom - window.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    }
    // button rows keep a gap between them, the last row's bottom border sits on its rects' bottom edge
    int last_bottom = -1;
    const int count = TabCtrl_GetItemCount(tab_);
    for (int i = 0; i < count; ++i)
    {
        RECT item_rect = {};
        if (TabCtrl_GetItemRect(tab_, i, &item_rect))
        {
            last_bottom = std::max<int>(last_bottom, item_rect.bottom);
        }
    }
    return std::max(row_height, last_bottom + 1);
}

void TabStrip::UpdateHot(POINT point)
{
    TCHITTESTINFO hit = {};
    hit.pt = point;
    const int index = TabCtrl_HitTest(tab_, &hit);
    RECT close_rect = {};
    const int close_hot = CloseRect(index, &close_rect) && PtInRect(&close_rect, point) ? index : -1;
    if (index != hot_ || close_hot != close_hot_)
    {
        hot_ = index;
        close_hot_ = close_hot;
        InvalidateRect(tab_, nullptr, FALSE);
    }
}

bool TabStrip::CloseRect(int index, RECT* rect) const
{
    RECT item_rect = {};
    if (index < 0 || TabCtrl_GetItemCount(tab_) <= 1 || !TabCtrl_GetItemRect(tab_, index, &item_rect))
    {
        return false;
    }
    return CloseButtonRect(item_rect, win32::DpiForWindow(tab_), rect);
}

// a closed box per tab, button tabs stand apart and the selected one differs only by its colors
void TabStrip::DrawItem(HDC hdc, int index, const RECT& item_rect, bool selected) const
{
    const Theme& theme = Theme::Current();
    const bool hot = index == hot_;
    FillRect(hdc, &item_rect, appearance::CachedBrush(hot ? theme.HoverColor() : selected ? theme.SurfaceColor()
                                                                                          : theme.PanelColor()));

    HGDIOBJ old_pen = SelectObject(hdc, appearance::CachedPen(theme.BorderColor(), 1));
    MoveToEx(hdc, item_rect.left, item_rect.bottom, nullptr);
    LineTo(hdc, item_rect.left, item_rect.top);
    LineTo(hdc, item_rect.right, item_rect.top);
    LineTo(hdc, item_rect.right, item_rect.bottom);
    LineTo(hdc, item_rect.left, item_rect.bottom);
    SelectObject(hdc, old_pen);

    const UINT dpi = win32::DpiForWindow(tab_);
    const int text_padding = appearance::ScaleForDpi(kTextPaddingX, dpi);
    RECT close_rect = {};
    const bool has_close = TabCtrl_GetItemCount(tab_) > 1 && CloseButtonRect(item_rect, dpi, &close_rect);
    RECT text_rect = item_rect;
    text_rect.left = item_rect.left + text_padding;
    text_rect.right = has_close ? std::max(text_rect.left, close_rect.left - appearance::ScaleForDpi(kCloseGap, dpi)) : item_rect.right - text_padding;
    SetTextColor(hdc, selected || hot ? theme.TextColor() : theme.MutedTextColor());
    SetBkMode(hdc, TRANSPARENT);
    wchar_t text[256] = {};
    TCITEMW item = {};
    item.mask = TCIF_TEXT;
    item.pszText = text;
    item.cchTextMax = static_cast<int>(_countof(text));
    if (TabCtrl_GetItem(tab_, index, &item))
    {
        DrawTextW(hdc, text, -1, &text_rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    if (!has_close)
    {
        return;
    }
    const bool down = index == close_down_;
    if (down || index == close_hot_)
    {
        FillRect(hdc, &close_rect, appearance::CachedBrush(down ? theme.SelectionColor() : theme.HoverColor()));
    }
    DrawCloseGlyph(hdc, close_rect, down ? theme.SelectionTextColor() : theme.TextColor(), dpi);
}

void TabStrip::Paint(HDC hdc) const
{
    RECT client = {};
    GetClientRect(tab_, &client);
    const Theme& theme = Theme::Current();
    FillRect(hdc, &client, theme.BackgroundBrush());
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(tab_, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = font ? SelectObject(hdc, font) : nullptr;

    const int count = TabCtrl_GetItemCount(tab_);
    const int current = TabCtrl_GetCurSel(tab_);
    for (int i = 0; i < count; ++i)
    {
        RECT item_rect = {};
        if (TabCtrl_GetItemRect(tab_, i, &item_rect))
        {
            DrawItem(hdc, i, item_rect, i == current);
        }
    }
    if (old_font)
    {
        SelectObject(hdc, old_font);
    }
}

LRESULT CALLBACK TabStrip::Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref_data)
{
    auto* self = reinterpret_cast<TabStrip*>(ref_data);
    const POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        self->UpdateHot(point);
        if (!self->tracking_)
        {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            self->tracking_ = TrackMouseEvent(&track) != FALSE;
        }
        return 0;
    case WM_MOUSELEAVE:
        self->tracking_ = false;
        if (self->hot_ != -1 || self->close_hot_ != -1)
        {
            self->hot_ = -1;
            self->close_hot_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        {
            TCHITTESTINFO hit = {};
            hit.pt = point;
            const int index = TabCtrl_HitTest(hwnd, &hit);
            RECT close_rect = {};
            if (self->CloseRect(index, &close_rect) && PtInRect(&close_rect, point))
            {
                self->close_down_ = index;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        }
    case WM_LBUTTONUP:
        if (self->close_down_ >= 0)
        {
            const int index = self->close_down_;
            self->close_down_ = -1;
            ReleaseCapture();
            RECT close_rect = {};
            if (self->CloseRect(index, &close_rect) && PtInRect(&close_rect, point))
            {
                self->hot_ = -1;
                self->close_hot_ = -1;
                self->on_close_(self->context_, index);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (self->close_down_ >= 0)
        {
            self->close_down_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_PARENTNOTIFY:
        // the one row fallback creates its scroll arrows, they take the current theme
        if (LOWORD(wparam) == WM_CREATE)
        {
            Theme::Current().ApplyToTabControl(hwnd);
        }
        break;
    case WM_PAINT:
        PaintBuffered(hwnd);
        return 0;
    case WM_PRINTCLIENT:
        self->Paint(reinterpret_cast<HDC>(wparam));
        return 0;
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

} // namespace regkit::ui
