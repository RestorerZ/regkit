// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>
#include <commctrl.h>

namespace regkit::appearance {

void PaintListHeader(HWND header, HFONT font);
void ReleaseListHeaderTheme(HWND header);

void PaintListGrid(HWND list, HDC hdc, const RECT& area, int first_line_y, int row_height, COLORREF color);
void PaintListGridTail(HWND list, HDC hdc, COLORREF color);
HBRUSH ListSurfaceBrush(HWND list);
LRESULT PaintGridToolbar(HWND toolbar, NMTBCUSTOMDRAW* draw, HBRUSH surface);

} // namespace regkit::appearance
