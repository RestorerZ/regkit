// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <windows.h>
#include <commctrl.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace regkit::editors::dialog_support {

struct ListColumn {
  const wchar_t* title = nullptr;
  int width = 100;
};

void Initialize(HWND dialog, HFONT* owned_font, std::initializer_list<int> bordered_edits);
void AllowNewlines(HWND dialog, int control_id);
void ReleaseFont(HFONT* font);
bool HandleThemeMessage(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam, INT_PTR* result);
std::wstring ReadText(HWND dialog, int control_id);

void SetupListView(HWND list, DWORD extra_styles, std::initializer_list<ListColumn> columns);
void RefreshListViewTheme(HWND list);
bool HandleListViewNotify(HWND dialog, const NMHDR* header, INT_PTR* result);
bool ShowColumnMenu(HWND list, POINT screen);
std::wstring ListViewText(HWND list, int item, int subitem);
std::wstring ToDisplayText(const std::wstring& text);
std::wstring FromDisplayText(const std::wstring& text);
std::wstring SingleLine(const std::wstring& text);
bool Matches(const std::wstring& text, const std::wstring& filter);
void FitDroppedWidth(HWND combo);
void SetGridLines(bool enabled);
bool GridLines();
void SetGridIcon(const std::wstring& path);
using GridLinesSink = void (*)(void* context, bool enabled);
void SetGridLinesSink(GridLinesSink sink, void* context);
void LayoutGridToggles(HWND dialog);
bool HandleGridToggle(HWND dialog, int command_id);
void ReleaseDialogLists(HWND dialog);

} // namespace regkit::editors::dialog_support
