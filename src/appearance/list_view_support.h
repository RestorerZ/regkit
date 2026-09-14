// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>
#include <commctrl.h>

#include <string>

namespace regkit::appearance {

using ListHeaderMenuCallback = void (*)(HWND list, POINT screen, void* context);
using ListGridChangedCallback = void (*)(void* context, bool enabled);
using ListItemCompareCallback = int (CALLBACK *)(LPARAM left, LPARAM right, int column, void* context);

void ConfigureListView(HWND list, DWORD extra_styles = 0);
void RegisterListView(
    HWND owner,
    HWND list,
    int grid_command,
    ListHeaderMenuCallback header_menu = nullptr,
    void* context = nullptr
);
void ReleaseListViews(HWND owner);
void LayoutListViews(HWND owner);
void RefreshListView(HWND list);

void SetListGridEnabled(bool enabled);
bool ListGridEnabled();
void SetListGridIcon(const std::wstring& path);
void SetListGridChangedCallback(ListGridChangedCallback callback, void* context);

bool HandleListViewCommand(HWND owner, int command);
bool HandleListViewNotify(HWND owner, const NMHDR* header, LRESULT* result);
LRESULT HandleListGridCustomDraw(HWND list, NMLVCUSTOMDRAW* draw, LRESULT result);

bool ShowListColumnMenu(HWND list, POINT screen);
void UpdateListViewSort(HWND list, int column, bool ascending);
void UpdateListSortState(int column, bool toggle, int* sort_column, bool* ascending);
void SortListViewItems(
    HWND list,
    int column,
    bool toggle,
    int* sort_column,
    bool* ascending,
    ListItemCompareCallback compare,
    void* context
);
int ListViewItemData(HWND list, int row, LPARAM* data);
int FindListViewItemByData(HWND list, LPARAM data);

} // namespace regkit::appearance
