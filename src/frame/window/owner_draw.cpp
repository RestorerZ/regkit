// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"

namespace regkit
{

using namespace window_detail;

void MainWindow::Impl::DrawPanelButton(const DRAWITEMSTRUCT* info)
{
    const Theme& theme = Theme::Current();
    HDC hdc = info->hDC;
    const RECT& rect = info->rcItem;
    const bool header = info->CtlID == kTreeHeaderCloseId || info->CtlID == kHistoryHeaderCloseId;
    const bool pressed = (info->itemState & ODS_SELECTED) != 0;
    FillRect(hdc, &rect, appearance::CachedBrush(pressed ? theme.HoverColor() : header ? theme.HeaderColor() : theme.SurfaceColor()));
    const UINT dpi = win32::DpiForWindow(info->hwndItem);
    if (!header)
    {
        HPEN old_pen = reinterpret_cast<HPEN>(SelectObject(hdc, appearance::CachedPen(theme.BorderColor())));
        MoveToEx(hdc, rect.left, rect.top + 3, nullptr);
        LineTo(hdc, rect.left, rect.bottom - 3);
        SelectObject(hdc, old_pen);
    }
    if (info->CtlID != kAddressGoId)
    {
        ui::DrawCloseGlyph(hdc, rect, theme.MutedTextColor(), dpi);
        return;
    }
    const int icon_size = appearance::ScaleForDpi(kToolbarGlyphSize, dpi);
    const int icon_x = rect.left + (rect.right - rect.left - icon_size) / 2;
    const int icon_y = rect.top + (rect.bottom - rect.top - icon_size) / 2;
    if (info->itemState & ODS_DISABLED)
    {
        DrawState(hdc, nullptr, nullptr, reinterpret_cast<LPARAM>(address_go_icon_), 0, icon_x, icon_y, icon_size, icon_size, DST_ICON | DSS_DISABLED);
    }
    else
    {
        DrawIconEx(hdc, icon_x, icon_y, address_go_icon_, icon_size, icon_size, 0, nullptr, DI_NORMAL);
    }
}

} // namespace regkit
