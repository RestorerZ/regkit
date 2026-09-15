// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "appearance/list_header.h"

#include "appearance/gdi_cache.h"
#include "appearance/theme.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <vsstyle.h>

namespace regkit::appearance {

HBRUSH ListSurfaceBrush(HWND list);

namespace {

constexpr wchar_t kHeaderThemeProp[] = L"RegKitHeaderTheme";
constexpr int kHeaderTextPadding = 8;

HTHEME HeaderTheme(
    HWND header
) {
  HTHEME cached = reinterpret_cast<HTHEME>(GetPropW(header, kHeaderThemeProp));
  if (!cached) {
    cached = OpenThemeData(header, VSCLASS_HEADER);
    SetPropW(header, kHeaderThemeProp, cached);
  }
  return cached;
}

HBRUSH HeaderSurfaceBrush(
    HWND header
) {
  return ListSurfaceBrush(GetParent(header));
}

} // namespace

void PaintListHeader(
    HWND header,
    HFONT font
) {
  if (!header) {
    return;
  }
  PAINTSTRUCT ps = {};
  HDC target = BeginPaint(header, &ps);
  if (!target) {
    return;
  }
  HDC hdc = nullptr;
  HPAINTBUFFER buffer = BeginBufferedPaint(target, &ps.rcPaint, BPBF_COMPATIBLEBITMAP, nullptr, &hdc);
  if (!hdc) {
    hdc = target;
  }

  const Theme& theme = Theme::Current();
  HBRUSH surface = HeaderSurfaceBrush(header);
  RECT client = {};
  GetClientRect(header, &client);
  FillRect(hdc, &client, surface);

  if (!font) {
    font = reinterpret_cast<HFONT>(SendMessageW(header, WM_GETFONT, 0, 0));
  }
  HFONT old_font = font ? reinterpret_cast<HFONT>(SelectObject(hdc, font)) : nullptr;

  HTHEME header_theme = HeaderTheme(header);
  SIZE arrow_size = {0, 0};
  if (header_theme) {
    GetThemePartSize(header_theme, hdc, HP_HEADERSORTARROW, HSAS_SORTEDUP, nullptr, TS_TRUE, &arrow_size);
  }
  if (arrow_size.cx <= 0 || arrow_size.cy <= 0) {
    arrow_size.cx = 8;
    arrow_size.cy = 8;
  }

  POINT cursor = {};
  int hot_item = -1;
  if (GetCursorPos(&cursor)) {
    ScreenToClient(header, &cursor);
    HDHITTESTINFO hit = {};
    hit.pt = cursor;
    hot_item = static_cast<int>(
        SendMessageW(header, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit))
    );
  }
  const bool pressed = GetCapture() == header && GetKeyState(VK_LBUTTON) < 0;

  const int count = Header_GetItemCount(header);
  for (int i = 0; i < count; ++i) {
    RECT rect = {};
    if (!Header_GetItemRect(header, i, &rect)) {
      continue;
    }
    RECT visible = {};
    if (!IntersectRect(&visible, &rect, &ps.rcPaint)) {
      continue;
    }

    wchar_t text[128] = {};
    HDITEMW item = {};
    item.mask = HDI_TEXT | HDI_FORMAT;
    item.pszText = text;
    item.cchTextMax = static_cast<int>(_countof(text));
    Header_GetItem(header, i, &item);

    const bool sorted_up = (item.fmt & HDF_SORTUP) != 0;
    const bool sorted_down = (item.fmt & HDF_SORTDOWN) != 0;

    HBRUSH item_surface = surface;
    if (i == hot_item) {
      item_surface = CachedBrush(pressed ? theme.SelectionColor() : theme.HoverColor());
    }
    FillRect(hdc, &rect, item_surface);
    RECT divider = {rect.right - 1, rect.top, rect.right, rect.bottom};
    FillRect(hdc, &divider, CachedBrush(theme.BorderColor()));

    RECT text_rect = rect;
    text_rect.left += kHeaderTextPadding;
    text_rect.right -= kHeaderTextPadding;

    UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
    if (item.fmt & HDF_RIGHT) {
      format |= DT_RIGHT;
    } else if (item.fmt & HDF_CENTER) {
      format |= DT_CENTER;
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, theme.TextColor());
    DrawTextW(hdc, text, -1, &text_rect, format);

    if ((sorted_up || sorted_down) && header_theme) {
      RECT arrow_rect = rect;
      arrow_rect.right -= 1;
      arrow_rect.bottom = rect.top + arrow_size.cy;
      const int arrow_state = sorted_up ? HSAS_SORTEDUP : HSAS_SORTEDDOWN;
      DrawThemeBackground(header_theme, hdc, HP_HEADERSORTARROW, arrow_state, &arrow_rect, nullptr);
    }
  }

  if (old_font) {
    SelectObject(hdc, old_font);
  }
  if (buffer) {
    EndBufferedPaint(buffer, TRUE);
  }
  EndPaint(header, &ps);
}

void ReleaseListHeaderTheme(
    HWND header
) {
  if (!header) {
    return;
  }
  if (HTHEME cached = reinterpret_cast<HTHEME>(GetPropW(header, kHeaderThemeProp))) {
    CloseThemeData(cached);
  }
  RemovePropW(header, kHeaderThemeProp);
}

void PaintListGrid(
    HWND list,
    HDC hdc,
    const RECT& area,
    int first_line_y,
    int row_height,
    COLORREF color
) {
  HWND header = list ? ListView_GetHeader(list) : nullptr;
  if (!hdc || !header || area.top >= area.bottom) {
    return;
  }
  RECT header_rect = {};
  if (!GetWindowRect(header, &header_rect)) {
    return;
  }
  MapWindowPoints(nullptr, list, reinterpret_cast<POINT*>(&header_rect), 2);
  RECT client = {};
  GetClientRect(list, &client);
  HBRUSH brush = CachedBrush(color);

  const int count = Header_GetItemCount(header);
  for (int i = 0; i < count; ++i) {
    RECT item = {};
    if (!Header_GetItemRect(header, i, &item)) {
      continue;
    }
    const int x = header_rect.left + item.right - 1;
    if (x < client.left || x >= client.right) {
      continue;
    }
    RECT line = {x, area.top, x + 1, area.bottom};
    FillRect(hdc, &line, brush);
  }

  if (row_height <= 0) {
    return;
  }
  for (int y = first_line_y; y < area.bottom; y += row_height) {
    if (y < area.top) {
      continue;
    }
    RECT line = {client.left, y, client.right, y + 1};
    FillRect(hdc, &line, brush);
  }
}

void PaintListGridTail(
    HWND list,
    HDC hdc,
    COLORREF color
) {
  HWND header = list ? ListView_GetHeader(list) : nullptr;
  if (!hdc || !header) {
    return;
  }
  RECT client = {};
  GetClientRect(list, &client);
  RECT header_rect = {};
  if (!GetWindowRect(header, &header_rect)) {
    return;
  }
  MapWindowPoints(nullptr, list, reinterpret_cast<POINT*>(&header_rect), 2);

  RECT area = client;
  area.top = header_rect.bottom;
  int row_height = 0;
  const int count = ListView_GetItemCount(list);
  if (count > 0) {
    RECT last = {};
    if (ListView_GetItemRect(list, count - 1, &last, LVIR_BOUNDS)) {
      row_height = last.bottom - last.top;
      if (last.bottom > area.top) {
        area.top = last.bottom;
      }
    }
  }
  PaintListGrid(list, hdc, area, area.top + row_height - 1, row_height, color);
}

HBRUSH ListSurfaceBrush(
    HWND list
) {
  const COLORREF color = list ? ListView_GetBkColor(list) : CLR_NONE;
  if (color == CLR_NONE || color == CLR_DEFAULT) {
    return Theme::Current().PanelBrush();
  }
  return CachedBrush(color);
}

LRESULT PaintGridToolbar(
    HWND toolbar,
    NMTBCUSTOMDRAW* draw,
    HBRUSH surface
) {
  if (!toolbar || !draw) {
    return CDRF_DODEFAULT;
  }
  switch (draw->nmcd.dwDrawStage) {
  case CDDS_PREPAINT:
    FillRect(draw->nmcd.hdc, &draw->nmcd.rc, surface);
    return CDRF_NOTIFYITEMDRAW;
  case CDDS_ITEMPREPAINT:
    draw->nmcd.uItemState &= ~(CDIS_HOT | CDIS_CHECKED | CDIS_SELECTED);
    return TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES | TBCDRF_NOOFFSET;
  default:
    break;
  }
  return CDRF_DODEFAULT;
}

} // namespace regkit::appearance
