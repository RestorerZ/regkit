// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <commctrl.h>

#include <string>

#include "win32/text_transform.h"

#include <win32ui/list_view/list_view.h>

namespace regkit::appearance
{

using ListGridChangedCallback = void (*)(void* context, bool enabled);

void SetListGridChangedCallback(ListGridChangedCallback callback, void* context);

int FetchListViewItemText(HWND list, int index, int column, std::wstring* buffer);
// selected item data as an index, -1 without selection
int SelectedItemIndex(HWND list);

// dialog list theme, translated labels, grid icon and shared grid setting
// row_menu replaces Copy and Select All with a dialog menu
win32ui::ListViewHooks DialogListHooks(bool (*row_menu)(void* context, HWND list, int row, POINT screen) = nullptr);
// appends Copy and Select All to dialog menus, RunRowMenuCommand handles them
void AppendRowMenuCommands(HMENU menu, HWND list);
bool RunRowMenuCommand(HWND list, int command);

} // namespace regkit::appearance
