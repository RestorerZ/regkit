// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <windows.h>

#include <commctrl.h>

#include "win32/text_transform.h"

#include <initializer_list>
#include <string>
#include <vector>

namespace regkit::appearance
{
class DialogResizer;
}

namespace regkit::editors::dialog_support
{

struct ListColumn
{
    const wchar_t* title = nullptr;
    int width = 100;
};

void Initialize(HWND dialog, HFONT* owned_font, std::initializer_list<int> bordered_edits);
void AllowNewlines(HWND dialog, int control_id);
void ReleaseFont(HFONT* font);
bool HandleThemeMessage(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam, INT_PTR* result, const appearance::DialogResizer* resizer = nullptr);
inline std::wstring ReadText(HWND dialog, int control_id)
{
    return util::DialogText(dialog, control_id);
}

void SetupListView(HWND list, DWORD extra_styles, std::initializer_list<ListColumn> columns);
bool HandleListViewNotify(HWND dialog, const NMHDR* header, INT_PTR* result);
std::wstring ListViewText(HWND list, int item, int subitem);
std::wstring ToDisplayText(const std::wstring& text);
std::wstring FromDisplayText(const std::wstring& text);
std::wstring SingleLine(const std::wstring& text);
bool Matches(const std::wstring& text, const std::wstring& filter);
void SetComboItems(HWND combo, const std::vector<std::wstring>& items);
void FitDroppedWidth(HWND combo);
void MatchComboHeights(HWND dialog, int edit_id, std::initializer_list<int> combos);

} // namespace regkit::editors::dialog_support
