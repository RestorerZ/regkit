// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/dialog_support.h"
#include "win32/text_transform.h"

#include "ui/list_view_support.h"
#include "ui/theme.h"

#include <win32ui/gdi/fonts.h>
#include <win32ui/window_metrics.h>

#include <algorithm>
#include <string>
#include <vector>

#include <commctrl.h>

namespace regkit::editors::dialog_support
{

HFONT ApplyMonoFont(HWND dialog, std::initializer_list<int> controls, HFONT dialog_font)
{
    // dialog font size follows custom font, Windows text size and DPI
    LOGFONTW logfont = {};
    const HFONT current = dialog_font ? dialog_font : reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
    const int height = current && GetObjectW(current, sizeof(logfont), &logfont) ? logfont.lfHeight : win32ui::FontHeight(9, win32ui::DpiForWindow(dialog));
    const HFONT font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_MODERN, L"Consolas");
    for (const int id : controls)
    {
        if (font)
        {
            SendDlgItemMessageW(dialog, id, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
    }
    return font;
}

void FollowDialogFont(HWND dialog, HFONT dialog_font, HFONT* mono_font, std::initializer_list<int> controls)
{
    if (!mono_font || !*mono_font)
    {
        return;
    }
    if (const HFONT font = ApplyMonoFont(dialog, controls, dialog_font))
    {
        DeleteObject(*mono_font);
        *mono_font = font;
    }
}

void ReleaseFont(HFONT* font)
{
    if (font && *font)
    {
        DeleteObject(*font);
        *font = nullptr;
    }
}

void SetupListView(HWND list, const win32ui::ListViewHooks* hooks, DWORD extra_styles, std::initializer_list<win32ui::ListColumn> columns)
{
    if (!list)
    {
        return;
    }
    static const win32ui::ListViewHooks defaults = appearance::DialogListHooks();
    // theme before enabling check boxes so state images use the right style
    win32ui::ApplyThemeToWindow(list);
    win32ui::AttachDialogListView(list, hooks ? hooks : &defaults, extra_styles);
    const UINT dpi = win32ui::DpiForWindow(list);
    std::vector<win32ui::ListColumn> shown;
    for (const win32ui::ListColumn& column : columns)
    {
        const int width = std::max(column.width_dip, win32ui::Unscale(ListView_GetStringWidth(list, column.title.c_str()), dpi) + 16);
        shown.push_back({column.title, width, width});
    }
    win32ui::SetListColumns(list, shown);
    win32ui::ShowListGridButton(list);
}

std::wstring ToDisplayText(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r'))
        {
            out.push_back(L'\r');
        }
        out.push_back(text[i]);
    }
    return out;
}

std::wstring FromDisplayText(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t c : text)
    {
        if (c != L'\r')
        {
            out.push_back(c);
        }
    }
    return out;
}

std::wstring SingleLine(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());
    bool space = false;
    for (const wchar_t c : text)
    {
        if (c == L'\r' || c == L'\n' || c == L'\t')
        {
            space = !out.empty();
            continue;
        }
        if (space)
        {
            out.push_back(L' ');
            space = false;
        }
        out.push_back(c);
    }
    return out;
}

bool Matches(const std::wstring& text, const std::wstring& filter)
{
    return filter.empty() || util::ContainsInsensitive(text, filter);
}

} // namespace regkit::editors::dialog_support
