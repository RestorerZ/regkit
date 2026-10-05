// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "dialogs/fields_dialog.h"

#include "ui/dialog_layout.h"
#include "ui/dialog_support.h"
#include "ui/feedback.h"

#include "resource.h"

#include <algorithm>

namespace regkit::editors
{

namespace
{

struct State
{
    const FieldsRequest* request = nullptr;
    std::wstring text;
    HFONT ui_font = nullptr;
    HFONT mono_font = nullptr;
    appearance::DialogResizer resizer;
};

std::wstring FormatFields(const FieldsRequest& request)
{
    std::wstring text;
    for (const auto& [label, value] : request.fields)
    {
        if (!label.empty())
        {
            text.append(label).append(L"\t").append(value);
        }
        text.append(L"\r\n");
    }
    return text;
}

void AlignValues(HWND edit, const FieldsRequest& request)
{
    // measured tab stop keeps values aligned for any script
    HDC dc = GetDC(edit);
    const HGDIOBJ previous = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0)));
    TEXTMETRICW metrics = {};
    GetTextMetricsW(dc, &metrics);
    int width = 0;
    for (const auto& [label, value] : request.fields)
    {
        SIZE size = {};
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &size);
        width = std::max(width, static_cast<int>(size.cx));
    }
    SelectObject(dc, previous);
    ReleaseDC(edit, dc);
    const int stop = MulDiv(width + 2 * metrics.tmAveCharWidth, 4, std::max(1, static_cast<int>(metrics.tmAveCharWidth)));
    SendMessageW(edit, EM_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(&stop));
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG)
    {
        state = reinterpret_cast<State*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        state->text = FormatFields(*state->request);
        SetWindowTextW(dialog, state->request->title.c_str());
        SetDlgItemTextW(dialog, IDC_FIELDS_IDENTIFIER, state->request->identifier.c_str());
        SetDlgItemTextW(dialog, IDC_EDIT, state->text.c_str());
        SetDlgItemTextW(dialog, IDC_FIELDS_ACTION, state->request->action_label.c_str());
        ShowWindow(GetDlgItem(dialog, IDC_FIELDS_ACTION), state->request->action_label.empty() ? SW_HIDE : SW_SHOW);
        EnableWindow(GetDlgItem(dialog, IDC_FIELDS_ACTION), static_cast<bool>(state->request->action));
        dialog_support::Initialize(dialog, &state->ui_font, {IDC_FIELDS_IDENTIFIER, IDC_EDIT});
        state->mono_font =
            CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_MODERN, L"Consolas");
        if (state->mono_font)
        {
            SendDlgItemMessageW(dialog, IDC_EDIT, WM_SETFONT, reinterpret_cast<WPARAM>(state->mono_font), TRUE);
        }
        AlignValues(GetDlgItem(dialog, IDC_EDIT), *state->request);
        using namespace appearance;
        state->resizer.Attach(dialog, {
                                          {IDC_FIELDS_IDENTIFIER, kAnchorLeft | kAnchorTop | kAnchorRight},
                                          {IDC_EDIT, kAnchorLeft | kAnchorTop | kAnchorRight | kAnchorBottom},
                                          {IDC_FIELDS_ACTION, kAnchorLeft | kAnchorBottom},
                                          {IDOK, kAnchorRight | kAnchorBottom},
                                          {IDCANCEL, kAnchorRight | kAnchorBottom},
                                      });
        SetFocus(GetDlgItem(dialog, IDC_EDIT));
        SendDlgItemMessageW(dialog, IDC_EDIT, EM_SETSEL, 0, 0);
        return FALSE;
    }
    if (message == WM_DESTROY)
    {
        if (state)
        {
            dialog_support::ReleaseFont(&state->mono_font);
            dialog_support::ReleaseFont(&state->ui_font);
        }
        return TRUE;
    }
    INT_PTR themed = 0;
    if (dialog_support::HandleThemeMessage(dialog, message, wparam, lparam, &themed, state ? &state->resizer : nullptr))
    {
        return themed;
    }
    if (message != WM_COMMAND || !state)
    {
        return FALSE;
    }
    switch (LOWORD(wparam))
    {
    case IDC_FIELDS_ACTION:
        state->request->action(dialog);
        return TRUE;
    case IDOK:
        ui::CopyTextToClipboard(dialog, state->text);
        return TRUE;
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

void ShowFields(HWND owner, const FieldsRequest& request)
{
    State state;
    state.request = &request;
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_FIELDS), owner, DialogProc, reinterpret_cast<LPARAM>(&state));
}

} // namespace regkit::editors
