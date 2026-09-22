// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "editors/dialog_support.h"
#include "win32/text_transform.h"
#include "win32/window_metrics.h"

#include "appearance/default_font.h"
#include "appearance/dialog_layout.h"
#include "appearance/feedback.h"
#include "appearance/list_view_support.h"
#include "appearance/theme.h"

#include "resource.h"

#include <algorithm>
#include <string>

#include <commctrl.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <windowsx.h>

namespace regkit::editors::dialog_support
{

namespace
{

constexpr UINT_PTR kSingleLineSubclassId = 2;
constexpr UINT_PTR kListViewSubclassId = 3;
constexpr int kTooltipMaxWidth = 600;
constexpr int kGridToggleId = 4200;

LRESULT CALLBACK SingleLineProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_CHAR && (wparam == L'\r' || wparam == L'\n'))
    {
        return 0;
    }
    if (message == WM_PASTE)
    {
        std::wstring text;
        if (OpenClipboard(window))
        {
            HANDLE handle = GetClipboardData(CF_UNICODETEXT);
            const wchar_t* data = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
            if (data)
            {
                text = data;
                GlobalUnlock(handle);
            }
            CloseClipboard();
        }
        if (text.find_first_of(L"\r\n") == std::wstring::npos)
        {
            return DefSubclassProc(window, message, wparam, lparam);
        }
        for (wchar_t& character : text)
        {
            if (character == L'\r' || character == L'\n')
            {
                character = L' ';
            }
        }
        SendMessageW(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
        return 0;
    }
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(window, SingleLineProc, kSingleLineSubclassId);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK ListViewProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
    {
        SendMessageW(window, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    }
    if (message == WM_UPDATEUISTATE)
    {
        const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        SendMessageW(window, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
        return result;
    }
    if (message == WM_THEMECHANGED)
    {
        InvalidateRect(window, nullptr, TRUE);
    }
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(window, ListViewProc, kListViewSubclassId);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

bool SizeGripRect(HWND dialog, RECT* rect)
{
    if (!rect || (GetWindowLongPtrW(dialog, GWL_STYLE) & WS_THICKFRAME) == 0)
    {
        return false;
    }
    RECT client = {};
    if (!GetClientRect(dialog, &client))
    {
        return false;
    }
    SIZE grip = {};
    HTHEME theme = OpenThemeData(dialog, VSCLASS_STATUS);
    if (theme)
    {
        if (FAILED(GetThemePartSize(theme, nullptr, SP_GRIPPER, 0, nullptr, TS_TRUE, &grip)))
        {
            grip = {};
        }
        CloseThemeData(theme);
    }
    if (grip.cx <= 0 || grip.cy <= 0)
    {
        grip.cx = grip.cy = GetSystemMetrics(SM_CXVSCROLL);
    }
    rect->left = client.right - grip.cx;
    rect->top = client.bottom - grip.cy;
    rect->right = client.right;
    rect->bottom = client.bottom;
    return true;
}

void DrawSizeGrip(HWND dialog, HDC hdc)
{
    RECT grip = {};
    if (!hdc || !SizeGripRect(dialog, &grip))
    {
        return;
    }
    HTHEME theme = OpenThemeData(dialog, VSCLASS_STATUS);
    if (theme)
    {
        DrawThemeBackground(theme, hdc, SP_GRIPPER, 0, &grip, nullptr);
        CloseThemeData(theme);
        return;
    }
    DrawFrameControl(hdc, &grip, DFC_SCROLL, DFCS_SCROLLSIZEGRIP);
}

void ThinBorder(HWND dialog, int id)
{
    const HWND edit = GetDlgItem(dialog, id);
    if (!edit)
    {
        return;
    }
    SetWindowLongPtrW(edit, GWL_EXSTYLE, GetWindowLongPtrW(edit, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
    SetWindowLongPtrW(edit, GWL_STYLE, GetWindowLongPtrW(edit, GWL_STYLE) | WS_BORDER);
    SetWindowPos(edit, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

} // namespace

void Initialize(HWND dialog, HFONT* owned_font, std::initializer_list<int> bordered_edits)
{
    for (const int id : bordered_edits)
    {
        ThinBorder(dialog, id);
    }
    HFONT font = ui::DefaultUIFont(win32::DpiForWindow(dialog));
    if (owned_font)
    {
        *owned_font = font;
    }
    if (font)
    {
        SendMessageW(dialog, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        EnumChildWindows(
            dialog,
            [](HWND child, LPARAM value) {
                SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(value), TRUE);
                wchar_t class_name[16] = {};
                GetClassNameW(child, class_name, _countof(class_name));
                const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
                if (util::EqualsInsensitive(class_name, L"Edit") && (style & ES_MULTILINE) && !(style & ES_READONLY))
                {
                    SetWindowSubclass(child, SingleLineProc, kSingleLineSubclassId, 0);
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font)
        );
    }
    Theme::Current().ApplyToWindow(dialog);
    Theme::Current().ApplyToChildren(dialog);
    regkit::appearance::CenterWindow(dialog, GetWindow(dialog, GW_OWNER));
}

void AllowNewlines(HWND dialog, int control_id)
{
    if (HWND edit = GetDlgItem(dialog, control_id))
    {
        RemoveWindowSubclass(edit, SingleLineProc, kSingleLineSubclassId);
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

bool HandleThemeMessage(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam, INT_PTR* result)
{
    if (!result)
    {
        return false;
    }
    if (message == WM_SETTINGCHANGE)
    {
        if (Theme::UpdateFromSystem())
        {
            Theme::Current().ApplyToWindow(dialog);
            Theme::Current().ApplyToChildren(dialog);
            InvalidateRect(dialog, nullptr, TRUE);
        }
        *result = TRUE;
        return true;
    }
    if (message == WM_ERASEBKGND)
    {
        RECT rect = {};
        GetClientRect(dialog, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect, Theme::Current().BackgroundBrush());
        DrawSizeGrip(dialog, reinterpret_cast<HDC>(wparam));
        *result = TRUE;
        return true;
    }
    if (message == WM_NCHITTEST)
    {
        RECT grip = {};
        if (SizeGripRect(dialog, &grip))
        {
            MapWindowPoints(dialog, nullptr, reinterpret_cast<POINT*>(&grip), 2);
            const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (PtInRect(&grip, pt))
            {
                SetWindowLongPtrW(dialog, DWLP_MSGRESULT, HTBOTTOMRIGHT);
                *result = TRUE;
                return true;
            }
        }
    }
    int color_type = 0;
    switch (message)
    {
    case WM_CTLCOLORDLG:
        color_type = CTLCOLOR_DLG;
        break;
    case WM_CTLCOLORSTATIC:
        color_type = CTLCOLOR_STATIC;
        break;
    case WM_CTLCOLOREDIT:
        color_type = CTLCOLOR_EDIT;
        break;
    case WM_CTLCOLORLISTBOX:
        color_type = CTLCOLOR_LISTBOX;
        break;
    case WM_CTLCOLORBTN:
        color_type = CTLCOLOR_BTN;
        break;
    default:
        return false;
    }
    *result = reinterpret_cast<INT_PTR>(
        Theme::Current().ControlColor(reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam), color_type)
    );
    return true;
}

void SetupListView(HWND list, DWORD extra_styles, std::initializer_list<ListColumn> columns)
{
    if (!list)
    {
        return;
    }
    appearance::ConfigureListView(list, LVS_EX_LABELTIP | extra_styles);

    const UINT dpi = win32::DpiForWindow(list);
    int index = 0;
    for (const ListColumn& column : columns)
    {
        LVCOLUMNW item = {};
        item.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        item.pszText = const_cast<wchar_t*>(column.title);
        item.cx = MulDiv(column.width, static_cast<int>(dpi), 96);
        item.iSubItem = index;
        ListView_InsertColumn(list, index, &item);
        ++index;
    }

    if (HWND tooltip = ListView_GetToolTips(list))
    {
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, MulDiv(kTooltipMaxWidth, static_cast<int>(dpi), 96));
    }
    EnsureSubclass(list, ListViewProc, kListViewSubclassId);
    appearance::RegisterListView(GetParent(list), list, kGridToggleId);
    RefreshListViewTheme(list);
}

void LayoutGridToggles(HWND dialog)
{
    appearance::LayoutListViews(dialog);
}

bool HandleGridToggle(HWND dialog, int command_id)
{
    return appearance::HandleListViewCommand(dialog, command_id);
}

void ReleaseDialogLists(HWND dialog)
{
    appearance::ReleaseListViews(dialog);
}

void RefreshListViewTheme(HWND list)
{
    if (!list)
    {
        return;
    }
    appearance::RefreshListView(list);
}

bool HandleListViewNotify(HWND dialog, const NMHDR* header, INT_PTR* result)
{
    if (!header || !result || header->code != NM_CUSTOMDRAW)
    {
        return false;
    }
    LRESULT feature_result = 0;
    if (appearance::HandleListViewNotify(dialog, header, &feature_result))
    {
        SetWindowLongPtrW(dialog, DWLP_MSGRESULT, feature_result);
        *result = TRUE;
        return true;
    }
    wchar_t class_name[32] = {};
    GetClassNameW(header->hwndFrom, class_name, static_cast<int>(_countof(class_name)));
    if (!util::EqualsInsensitive(class_name, WC_LISTVIEWW))
    {
        return false;
    }
    const HWND list = header->hwndFrom;
    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMHDR*>(header));
    LRESULT drawn = ui::HandleThemedListViewCustomDraw(list, draw);
    drawn = appearance::HandleListGridCustomDraw(list, draw, drawn);
    SetWindowLongPtrW(dialog, DWLP_MSGRESULT, drawn);
    *result = TRUE;
    return true;
}

std::wstring ListViewText(HWND list, int item, int subitem)
{
    wchar_t buffer[1024] = {};
    ListView_GetItemText(list, item, subitem, buffer, static_cast<int>(_countof(buffer)));
    return buffer;
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

void FitDroppedWidth(HWND combo)
{
    if (!combo)
    {
        return;
    }
    const HDC dc = GetDC(combo);
    if (!dc)
    {
        return;
    }
    auto* font = reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0));
    HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    int widest = 0;
    std::wstring text;
    for (int i = 0; i < count; ++i)
    {
        const int length = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0));
        if (length <= 0)
        {
            continue;
        }
        text.resize(static_cast<size_t>(length));
        SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data()));
        SIZE size = {};
        if (GetTextExtentPoint32W(dc, text.c_str(), length, &size) && size.cx > widest)
        {
            widest = size.cx;
        }
    }
    if (previous)
    {
        SelectObject(dc, previous);
    }
    ReleaseDC(combo, dc);
    RECT rect = {};
    GetWindowRect(combo, &rect);
    const int minimum = rect.right - rect.left;
    const int padding = GetSystemMetrics(SM_CXVSCROLL) + 8;
    SendMessageW(combo, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(std::max(minimum, widest + padding)), 0);
}

} // namespace regkit::editors::dialog_support
