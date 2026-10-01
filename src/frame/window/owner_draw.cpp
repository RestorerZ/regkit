// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"

namespace regkit
{

using namespace window_detail;

void MainWindow::Impl::DrawAddressButton(const DRAWITEMSTRUCT* info)
{
    if (!info)
    {
        return;
    }
    const Theme& theme = Theme::Current();
    HDC hdc = info->hDC;
    RECT rect = info->rcItem;
    bool pressed = (info->itemState & ODS_SELECTED) != 0;

    COLORREF bg_color = pressed ? theme.HoverColor() : theme.SurfaceColor();
    FillRect(hdc, &rect, appearance::CachedBrush(bg_color));

    HPEN pen = appearance::CachedPen(theme.BorderColor());
    HPEN old_pen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, rect.left, rect.top + 3, nullptr);
    LineTo(hdc, rect.left, rect.bottom - 3);
    SelectObject(hdc, old_pen);

    if (info->CtlID == kAddressGoId)
    {
        if (address_go_icon_)
        {
            UINT dpi = win32::DpiForWindow(hwnd_);
            int icon_size = appearance::ScaleForDpi(kToolbarGlyphSize, dpi);
            int icon_x = rect.left + (rect.right - rect.left - icon_size) / 2;
            int icon_y = rect.top + (rect.bottom - rect.top - icon_size) / 2;
            if (info->itemState & ODS_DISABLED)
            {
                DrawState(hdc, nullptr, nullptr, reinterpret_cast<LPARAM>(address_go_icon_), 0, icon_x, icon_y, icon_size, icon_size, DST_ICON | DSS_DISABLED);
            }
            else
            {
                DrawIconEx(hdc, icon_x, icon_y, address_go_icon_, icon_size, icon_size, 0, nullptr, DI_NORMAL);
            }
        }
        else
        {
            POINT pts[3] = {
                {rect.left + 8, rect.top + 6},
                {rect.left + 8, rect.bottom - 6},
                {rect.right - 6, (rect.top + rect.bottom) / 2},
            };
            COLORREF arrow_color = theme.MutedTextColor();
            HBRUSH arrow_brush = appearance::CachedBrush(arrow_color);
            HBRUSH old_brush = reinterpret_cast<HBRUSH>(SelectObject(hdc, arrow_brush));
            HPEN arrow_pen = appearance::CachedPen(arrow_color);
            HPEN old_arrow = reinterpret_cast<HPEN>(SelectObject(hdc, arrow_pen));
            Polygon(hdc, pts, 3);
            SelectObject(hdc, old_arrow);
            SelectObject(hdc, old_brush);
        }
    }
}

void MainWindow::Impl::DrawHeaderCloseButton(const DRAWITEMSTRUCT* info)
{
    if (!info)
    {
        return;
    }
    const Theme& theme = Theme::Current();
    HDC hdc = info->hDC;
    RECT rect = info->rcItem;
    bool pressed = (info->itemState & ODS_SELECTED) != 0;

    COLORREF bg_color = pressed ? theme.HoverColor() : theme.HeaderColor();
    FillRect(hdc, &rect, appearance::CachedBrush(bg_color));

    DrawCloseGlyph(hdc, rect, theme.MutedTextColor(), win32::DpiForWindow(info->hwndItem));
}

void MainWindow::Impl::DrawFilterClearButton(const DRAWITEMSTRUCT* info)
{
    if (!info)
    {
        return;
    }
    const Theme& theme = Theme::Current();
    HDC hdc = info->hDC;
    RECT rect = info->rcItem;
    const bool pressed = (info->itemState & ODS_SELECTED) != 0;
    FillRect(hdc, &rect, appearance::CachedBrush(pressed ? theme.HoverColor() : theme.SurfaceColor()));
    HPEN pen = appearance::CachedPen(theme.BorderColor());
    HPEN old_pen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, rect.left, rect.top + 3, nullptr);
    LineTo(hdc, rect.left, rect.bottom - 3);
    SelectObject(hdc, old_pen);
    DrawCloseGlyph(hdc, rect, theme.MutedTextColor(), win32::DpiForWindow(info->hwndItem));
}

} // namespace regkit
