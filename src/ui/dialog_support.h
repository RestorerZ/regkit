// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <windows.h>

#include <commctrl.h>

#include "win32/text_transform.h"

#include <win32ui/list_view/list_view.h>

#include <initializer_list>
#include <string>

namespace regkit::editors::dialog_support
{

// Consolas at dialog font size for hex/decoded text, caller must release it
HFONT ApplyMonoFont(HWND dialog, std::initializer_list<int> controls, HFONT dialog_font = nullptr);
// dialog receives WM_SETFONT before controls on DPI changes, update mono font with it
void FollowDialogFont(HWND dialog, HFONT dialog_font, HFONT* mono_font, std::initializer_list<int> controls);
void ReleaseFont(HFONT* font);
inline std::wstring ReadText(HWND dialog, int control_id)
{
    return util::DialogText(dialog, control_id);
}

// shared dialog list with hooks or RegKit defaults, hooks must outlive it
// columns use DIPs and fit translated titles, adds grid button
void SetupListView(HWND list, const win32ui::ListViewHooks* hooks, DWORD extra_styles, std::initializer_list<win32ui::ListColumn> columns);
std::wstring ToDisplayText(const std::wstring& text);
std::wstring FromDisplayText(const std::wstring& text);
std::wstring SingleLine(const std::wstring& text);
bool Matches(const std::wstring& text, const std::wstring& filter);

} // namespace regkit::editors::dialog_support
