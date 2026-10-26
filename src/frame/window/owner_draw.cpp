// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"

#include <win32ui/gdi/gdi_cache.h>
#include <win32ui/gdi/icon_loader.h>
#include <win32ui/window_metrics.h>

namespace regkit
{

using namespace window_detail;

void MainWindow::Impl::DrawPanelButton(const DRAWITEMSTRUCT* info)
{
    const win32ui::ThemeColors& colors = win32ui::CurrentThemeColors();
    HDC hdc = info->hDC;
    const RECT& rect = info->rcItem;
    const bool header = info->CtlID == kTreeHeaderCloseId || info->CtlID == kHistoryHeaderCloseId;
    const bool pressed = (info->itemState & ODS_SELECTED) != 0;
    FillRect(hdc, &rect, win32ui::CachedBrush(pressed ? colors.hover : header ? colors.header
                                                                              : colors.surface));
    const UINT dpi = win32ui::DpiForWindow(info->hwndItem);
    if (!header)
    {
        HPEN old_pen = reinterpret_cast<HPEN>(SelectObject(hdc, win32ui::CachedPen(colors.border)));
        MoveToEx(hdc, rect.left, rect.top + 3, nullptr);
        LineTo(hdc, rect.left, rect.bottom - 3);
        SelectObject(hdc, old_pen);
    }
    if (info->CtlID != kAddressGoId)
    {
        win32ui::DrawCloseGlyph(hdc, rect, colors.muted_text, dpi);
        return;
    }
    const int icon_size = win32ui::ScaleForDpi(win32ui::kIconDip, dpi);
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
