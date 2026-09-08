// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "appearance/dialog_layout.h"

#include "appearance/default_font.h"
#include "appearance/theme.h"
#include "appearance/gdi_cache.h"
#include "win32/window_metrics.h"

#include <algorithm>
#include <commctrl.h>

#ifndef DCX_USESTYLE
#define DCX_USESTYLE 0x00010000
#endif
#ifndef DCX_NODELETERGN
#define DCX_NODELETERGN 0x00040000
#endif
#ifndef HRGN_FULL
#define HRGN_FULL reinterpret_cast<HRGN>(1)
#endif

namespace regkit::appearance {

namespace {

constexpr UINT_PTR kThemedBorderSubclassId = 31;

LRESULT CALLBACK ThemedBorderProc(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam, UINT_PTR id, DWORD_PTR) {
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(hwnd, ThemedBorderProc, id);
    return DefSubclassProc(hwnd, message, wparam, lparam);
  }
  if (message != WM_NCPAINT) {
    return DefSubclassProc(hwnd, message, wparam, lparam);
  }
  const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
  HRGN region = reinterpret_cast<HRGN>(wparam);
  UINT flags = DCX_WINDOW | DCX_CACHE | DCX_USESTYLE;
  if (region == HRGN_FULL) {
    region = nullptr;
  } else if (region) {
    flags |= DCX_INTERSECTRGN | DCX_NODELETERGN;
  }
  HDC hdc = GetDCEx(hwnd, region, flags);
  if (!hdc) {
    return result;
  }
  RECT frame = {};
  if (GetWindowRect(hwnd, &frame)) {
    OffsetRect(&frame, -frame.left, -frame.top);
    FrameRect(hdc, &frame, CachedBrush(Theme::Current().BorderColor()));
  }
  ReleaseDC(hwnd, hdc);
  return result;
}

} // namespace

void AttachThemedBorder(HWND control) {
  if (!control ||
      GetWindowSubclass(control, ThemedBorderProc, kThemedBorderSubclassId,
                        nullptr)) {
    return;
  }
  if (!SetWindowSubclass(control, ThemedBorderProc, kThemedBorderSubclassId,
                         0)) {
    return;
  }
  SetWindowLongPtrW(control, GWL_EXSTYLE,
                    GetWindowLongPtrW(control, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
  SetWindowLongPtrW(control, GWL_STYLE,
                    GetWindowLongPtrW(control, GWL_STYLE) | WS_BORDER);
  SetWindowPos(control, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}


void SetControlFont(HWND control, HFONT font) {
  if (control && font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
}

void Place(HWND control, int x, int y, int width, int height) {
  if (control) {
    SetWindowPos(control, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void RestoreDialogOwner(HWND owner, bool* restored) {
  if (!owner || !restored || *restored) {
    return;
  }
  EnableWindow(owner, TRUE);
  SetActiveWindow(owner);
  SetForegroundWindow(owner);
  *restored = true;
}

void PositionDialog(HWND dialog, HWND owner, int width, int height) {
  RECT owner_rect = {};
  if (owner && GetWindowRect(owner, &owner_rect)) {
    const int owner_width = owner_rect.right - owner_rect.left;
    const int owner_height = owner_rect.bottom - owner_rect.top;
    const int x = owner_rect.left + std::max(0, (owner_width - width) / 2);
    const int y = owner_rect.top + std::max(0, (owner_height - height) / 2);
    SetWindowPos(dialog, nullptr, x, y, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return;
  }
  SetWindowPos(dialog, nullptr, 0, 0, width, height,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void CenterWindow(HWND window, HWND owner) {
  RECT rect = {};
  if (!window || !GetWindowRect(window, &rect)) {
    return;
  }
  RECT target = {};
  if ((!owner || !GetWindowRect(owner, &target)) &&
      !SystemParametersInfoW(SPI_GETWORKAREA, 0, &target, 0)) {
    return;
  }
  const LONG width = rect.right - rect.left;
  const LONG height = rect.bottom - rect.top;
  SetWindowPos(window, nullptr,
               target.left + std::max<LONG>(0, (target.right - target.left - width) / 2),
               target.top + std::max<LONG>(0, (target.bottom - target.top - height) / 2),
               0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RefreshDialogFont(HWND window, HFONT* owned_font, UINT dpi) {
  if (!window || !owned_font) {
    return;
  }
  HFONT font = ui::DefaultUIFont(dpi);
  if (!font) {
    return;
  }
  SetControlFont(window, font);
  EnumChildWindows(
      window,
      [](HWND child, LPARAM param) -> BOOL {
        SetControlFont(child, reinterpret_cast<HFONT>(param));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(font));
  if (*owned_font) {
    DeleteObject(*owned_font);
  }
  *owned_font = font;
}

void ApplyDpiChange(HWND window, LPARAM suggested_rect) {
  const RECT* rect = reinterpret_cast<const RECT*>(suggested_rect);
  if (!window || !rect) {
    return;
  }
  SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
               rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void CenterEditText(HWND edit, HFONT font, int left_pad, int right_pad) {
  if (!edit || !font) {
    return;
  }
  RECT rect = {};
  GetClientRect(edit, &rect);
  HDC hdc = GetDC(edit);
  if (!hdc) {
    return;
  }
  HFONT old = reinterpret_cast<HFONT>(SelectObject(hdc, font));
  TEXTMETRICW tm = {};
  const bool ok = GetTextMetricsW(hdc, &tm) != FALSE;
  SelectObject(hdc, old);
  ReleaseDC(edit, hdc);
  if (!ok) {
    return;
  }
  const int line = static_cast<int>(tm.tmHeight + tm.tmExternalLeading);
  const int pad = std::max(0, (static_cast<int>(rect.bottom - rect.top) - line) / 2);
  rect.left += left_pad;
  rect.right -= right_pad;
  rect.top += pad;
  rect.bottom = rect.top + line;
  SendMessageW(edit, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&rect));
}

void FitDialogHeight(HWND dialog, int client_height) {
  if (!dialog || client_height <= 0) {
    return;
  }
  RECT client = {};
  if (!GetClientRect(dialog, &client) ||
      client.bottom - client.top == client_height) {
    return;
  }
  RECT want = {0, 0, client.right - client.left, client_height};
  win32::AdjustWindowRectForDpi(&want,
                               static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)),
                               static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)),
                               win32::DpiForWindow(dialog));
  SetWindowPos(dialog, nullptr, 0, 0, want.right - want.left,
               want.bottom - want.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void RunModalLoop(HWND dialog) {
  MSG msg = {};
  while (IsWindow(dialog)) {
    const BOOL available = GetMessageW(&msg, nullptr, 0, 0);
    if (available == -1) {
      break;
    }
    if (available == 0) {
      PostQuitMessage(static_cast<int>(msg.wParam));
      break;
    }
    if (!IsDialogMessageW(dialog, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
}

void DialogResizer::Attach(HWND dialog, std::initializer_list<AnchorRule> rules) {
  items_.clear();
  client_ = {};
  min_window_ = {};
  if (!dialog) {
    return;
  }

  RECT client = {};
  RECT window = {};
  if (!GetClientRect(dialog, &client) || !GetWindowRect(dialog, &window)) {
    return;
  }
  client_ = {client.right - client.left, client.bottom - client.top};
  min_window_ = {window.right - window.left, window.bottom - window.top};

  items_.reserve(rules.size());
  for (const AnchorRule& rule : rules) {
    HWND control = GetDlgItem(dialog, rule.id);
    RECT rect = {};
    if (!control || !GetWindowRect(control, &rect)) {
      continue;
    }
    MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
    items_.push_back({rule.id, rule.anchors, rect});
  }
}

void DialogResizer::Apply(HWND dialog) const {
  if (!dialog || items_.empty() || client_.cx <= 0 || client_.cy <= 0) {
    return;
  }
  RECT client = {};
  if (!GetClientRect(dialog, &client)) {
    return;
  }
  const LONG dx = (client.right - client.left) - client_.cx;
  const LONG dy = (client.bottom - client.top) - client_.cy;

  HDWP defer = BeginDeferWindowPos(static_cast<int>(items_.size()));
  for (const Item& item : items_) {
    HWND control = GetDlgItem(dialog, item.id);
    if (!control) {
      continue;
    }
    const LONG left = (item.anchors & kAnchorLeft) ? item.rect.left : item.rect.left + dx;
    const LONG right = (item.anchors & kAnchorRight) ? item.rect.right + dx : item.rect.right;
    const LONG top = (item.anchors & kAnchorTop) ? item.rect.top : item.rect.top + dy;
    const LONG bottom = (item.anchors & kAnchorBottom) ? item.rect.bottom + dy : item.rect.bottom;
    const int width = static_cast<int>(std::max<LONG>(0, right - left));
    const int height = static_cast<int>(std::max<LONG>(0, bottom - top));
    if (defer) {
      defer = DeferWindowPos(defer, control, nullptr, static_cast<int>(left), static_cast<int>(top), width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
      SetWindowPos(control, nullptr, static_cast<int>(left), static_cast<int>(top), width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  if (defer) {
    EndDeferWindowPos(defer);
  }
  InvalidateRect(dialog, nullptr, TRUE);
}

void DialogResizer::ClampMinSize(MINMAXINFO* info) const {
  if (!info || min_window_.cx <= 0 || min_window_.cy <= 0) {
    return;
  }
  info->ptMinTrackSize.x = std::max<LONG>(info->ptMinTrackSize.x, min_window_.cx);
  info->ptMinTrackSize.y = std::max<LONG>(info->ptMinTrackSize.y, min_window_.cy);
}

} // namespace regkit::appearance
