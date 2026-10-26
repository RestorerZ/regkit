// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/font_picker.h"

#include <win32ui/dialog/single_line_edit.h>
#include <win32ui/gdi/fonts.h>
#include <win32ui/gdi/buffered_paint.h>

#include <commctrl.h>
#include <commdlg.h>
#include <dlgs.h>
#include <uxtheme.h>

#include "ui/theme.h"
#include "win32/text_transform.h"

namespace regkit
{

namespace
{

struct FontDialogHookState
{
    HFONT sample_font = nullptr;
    LOGFONTW preview_base_font = {};
};

constexpr UINT_PTR kFontDialogSubclassId = 1;
constexpr UINT_PTR kFontDialogSampleSubclassId = 4;
constexpr UINT kFontDialogUpdatePreviewMessage = WM_APP + 101;

LRESULT CALLBACK FontDialogSampleSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR ref_data);

bool IsSameFontChoice(const LOGFONTW& left, const LOGFONTW& right)
{
    return util::EqualsInsensitive(left.lfFaceName, right.lfFaceName) &&
           win32ui::FontPointSize(left) == win32ui::FontPointSize(right) && left.lfWeight == right.lfWeight &&
           !!left.lfItalic == !!right.lfItalic;
}

void UpdateSamplePreview(HWND hwnd, FontDialogHookState* state)
{
    if (!hwnd || !state)
    {
        return;
    }

    HWND size_combo = GetDlgItem(hwnd, cmb3);
    HWND sample_text = GetDlgItem(hwnd, stc5);
    if (!size_combo || !sample_text)
    {
        return;
    }

    LOGFONTW preview_font = state->preview_base_font;

    LOGFONTW selected = {};
    SendMessageW(hwnd, WM_CHOOSEFONT_GETLOGFONT, 0, reinterpret_cast<LPARAM>(&selected));
    if (selected.lfFaceName[0] != L'\0')
    {
        wcsncpy_s(preview_font.lfFaceName, selected.lfFaceName, _TRUNCATE);
        preview_font.lfWeight = selected.lfWeight;
        preview_font.lfItalic = selected.lfItalic;
    }

    wchar_t size_text[32] = {};
    GetWindowTextW(size_combo, size_text, static_cast<int>(_countof(size_text)));
    int point_size = _wtoi(size_text);
    if (point_size > 0)
    {
        preview_font.lfHeight = win32ui::FontHeight(point_size);
    }

    HFONT next_font = CreateFontIndirectW(&preview_font);
    if (!next_font)
    {
        return;
    }

    HFONT previous_font = state->sample_font;
    state->sample_font = next_font;
    ShowWindow(sample_text, SW_SHOWNA);
    SendMessageW(sample_text, WM_SETFONT, reinterpret_cast<WPARAM>(next_font), TRUE);
    SetWindowTextW(sample_text, L"AaBbYyZz");
    InvalidateRect(sample_text, nullptr, TRUE);

    if (previous_font)
    {
        DeleteObject(previous_font);
    }
}

void ApplyComboTheme(HWND combo)
{
    if (!combo)
    {
        return;
    }

    LONG_PTR style = GetWindowLongPtrW(combo, GWL_STYLE);
    LONG_PTR combo_style = style & CBS_DROPDOWNLIST;
    bool is_simple = combo_style == CBS_SIMPLE;

    COMBOBOXINFO info = {sizeof(COMBOBOXINFO)};
    if (!GetComboBoxInfo(combo, &info))
    {
        return;
    }

    if (info.hwndItem)
    {
        win32ui::SetThemeClass(info.hwndItem, L"DarkMode_CFD");
        InvalidateRect(info.hwndItem, nullptr, TRUE);
    }
    if (info.hwndList)
    {
        win32ui::SetThemeClass(info.hwndList, L"DarkMode_Explorer");
        if (!is_simple)
        {
            LONG_PTR list_style = GetWindowLongPtrW(info.hwndList, GWL_STYLE);
            LONG_PTR list_ex_style = GetWindowLongPtrW(info.hwndList, GWL_EXSTYLE);
            if ((list_style & WS_BORDER) != 0)
            {
                SetWindowLongPtrW(info.hwndList, GWL_STYLE, list_style & ~WS_BORDER);
            }
            if ((list_ex_style & WS_EX_CLIENTEDGE) != 0)
            {
                SetWindowLongPtrW(info.hwndList, GWL_EXSTYLE, list_ex_style & ~WS_EX_CLIENTEDGE);
            }
            SetWindowPos(info.hwndList, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        RedrawWindow(info.hwndList, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_FRAME);
    }
    InvalidateRect(combo, nullptr, TRUE);
}

LRESULT CALLBACK FontDialogSampleSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR)
{
    switch (msg)
    {
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, FontDialogSampleSubclassProc, id);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETTEXT:
    case WM_SETFONT:
        {
            LRESULT result = DefSubclassProc(hwnd, msg, wparam, lparam);
            InvalidateRect(hwnd, nullptr, TRUE);
            return result;
        }
    case WM_PAINT:
        win32ui::PaintBuffered(hwnd);
        return 0;
    case WM_PRINTCLIENT:
        {
            HDC hdc = reinterpret_cast<HDC>(wparam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);

            FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));

            HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
            HGDIOBJ old_font = nullptr;
            if (font)
            {
                old_font = SelectObject(hdc, font);
            }

            wchar_t text[128] = {};
            GetWindowTextW(hwnd, text, static_cast<int>(_countof(text)));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));

            RECT text_rect = rc;
            DrawTextW(hdc, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            if (old_font)
            {
                SelectObject(hdc, old_font);
            }
            return 0;
        }
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

void ApplyFontDialogTheme(HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }

    win32ui::ApplyThemeToWindow(hwnd);
    win32ui::AddSelectAllShortcut(hwnd);

    constexpr int kComboIds[] = {cmb1, cmb2, cmb3, cmb5};
    for (int combo_id : kComboIds)
    {
        HWND combo = GetDlgItem(hwnd, combo_id);
        ApplyComboTheme(combo);
    }

    HWND extra_bar = GetDlgItem(hwnd, stc6);
    if (extra_bar)
    {
        ShowWindow(extra_bar, SW_HIDE);
    }

    HWND sample_text = GetDlgItem(hwnd, stc5);
    // font sample uses system window colors
    if (sample_text)
    {
        SetWindowTheme(sample_text, nullptr, nullptr);
    }
    if (sample_text &&
        !GetWindowSubclass(sample_text, FontDialogSampleSubclassProc, kFontDialogSampleSubclassId, nullptr))
    {
        SetWindowSubclass(sample_text, FontDialogSampleSubclassProc, kFontDialogSampleSubclassId, 0);
    }
}

LRESULT CALLBACK FontDialogSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR)
{
    switch (msg)
    {
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, FontDialogSubclassProc, id);
        break;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        if (reinterpret_cast<HWND>(lparam) != GetDlgItem(hwnd, stc5))
        {
            return reinterpret_cast<LRESULT>(win32ui::ThemeControlColor(msg, reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam)));
        }
        break;
    case WM_NOTIFY:
        if (LRESULT result = 0; win32ui::ThemeCustomDraw(reinterpret_cast<NMHDR*>(lparam), &result))
        {
            return result;
        }
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

UINT_PTR CALLBACK FontDialogHookProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        {
            auto* choose = reinterpret_cast<CHOOSEFONTW*>(lparam);
            auto* state = choose ? reinterpret_cast<FontDialogHookState*>(choose->lCustData) : nullptr;
            SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            EnsureSubclass(hwnd, FontDialogSubclassProc, kFontDialogSubclassId);
            ApplyFontDialogTheme(hwnd);
            PostMessageW(hwnd, kFontDialogUpdatePreviewMessage, 0, reinterpret_cast<LPARAM>(state));
            return 0;
        }
    case kFontDialogUpdatePreviewMessage:
        UpdateSamplePreview(hwnd, reinterpret_cast<FontDialogHookState*>(lparam));
        return 0;
    case WM_COMMAND:
        if (HIWORD(wparam) == CBN_DROPDOWN || HIWORD(wparam) == CBN_SELCHANGE)
        {
            HWND combo = reinterpret_cast<HWND>(lparam);
            ApplyComboTheme(combo);
        }
        if (HIWORD(wparam) == CBN_SELCHANGE || HIWORD(wparam) == CBN_EDITCHANGE || HIWORD(wparam) == EN_CHANGE ||
            HIWORD(wparam) == LBN_SELCHANGE)
        {
            auto* dialog_state = reinterpret_cast<FontDialogHookState*>(GetWindowLongPtrW(hwnd, DWLP_USER));
            if (dialog_state)
            {
                PostMessageW(hwnd, kFontDialogUpdatePreviewMessage, 0, reinterpret_cast<LPARAM>(dialog_state));
            }
        }
        break;
    default:
        break;
    }
    return 0;
}

} // namespace

bool ShowFontDialog(HWND owner, const LOGFONTW& default_font, bool use_default, const LOGFONTW& current, FontDialogResult* out)
{
    if (!out)
    {
        return false;
    }

    LOGFONTW chosen = use_default ? default_font : current;
    FontDialogHookState hook_state = {};
    hook_state.preview_base_font = chosen;

    CHOOSEFONTW choose_font = {};
    choose_font.lStructSize = sizeof(choose_font);
    choose_font.hwndOwner = owner;
    choose_font.lpLogFont = &chosen;
    choose_font.Flags = CF_ENABLEHOOK | CF_FORCEFONTEXIST | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS | CF_SCREENFONTS;
    choose_font.lpfnHook = FontDialogHookProc;
    choose_font.lCustData = reinterpret_cast<LPARAM>(&hook_state);

    bool accepted = ChooseFontW(&choose_font) != FALSE;
    if (hook_state.sample_font)
    {
        DeleteObject(hook_state.sample_font);
        hook_state.sample_font = nullptr;
    }
    if (!accepted)
    {
        return false;
    }

    out->use_default = IsSameFontChoice(chosen, default_font);
    out->font = out->use_default ? default_font : chosen;
    return true;
}

} // namespace regkit
