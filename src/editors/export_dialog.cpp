// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "editors/export_dialog.h"

#include "appearance/feedback.h"
#include "editors/dialog_support.h"
#include "win32/file_dialog.h"
#include "win32/shell_paths.h"
#include "win32/translation.h"

#include "resource.h"

#include <algorithm>
#include <utility>

namespace regkit::editors
{

namespace
{

struct State
{
    ExportResult value;
    std::wstring confirmed_path;
    HFONT font = nullptr;
    bool accepted = false;
};

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG)
    {
        state = reinterpret_cast<State*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog, L"RegKit");
        SetDlgItemTextW(dialog, IDC_EXPORT_PATH, state->value.path.c_str());
        CheckDlgButton(dialog, IDC_EXPORT_RANGE_BRANCH, state->value.include_subkeys ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_EXPORT_RANGE_KEY, state->value.include_subkeys ? BST_UNCHECKED : BST_CHECKED);
        for (const wchar_t* item : {util::Tr(L"Don't open"), util::Tr(L"In text editor"), util::Tr(L"In RegKit")})
        {
            SendDlgItemMessageW(dialog, IDC_EXPORT_OPEN_AFTER, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        }
        SendDlgItemMessageW(dialog, IDC_EXPORT_OPEN_AFTER, CB_SETCURSEL, static_cast<WPARAM>(state->value.open_after), 0);
        dialog_support::Initialize(dialog, &state->font, {IDC_EXPORT_PATH});
        return TRUE;
    }
    if (message == WM_DESTROY)
    {
        if (state)
        {
            dialog_support::ReleaseFont(&state->font);
        }
        return TRUE;
    }
    INT_PTR themed = 0;
    if (dialog_support::HandleThemeMessage(dialog, message, wparam, lparam, &themed))
    {
        return themed;
    }
    if (message != WM_COMMAND || !state)
    {
        return FALSE;
    }
    const int id = LOWORD(wparam);
    if (id == IDC_EXPORT_BROWSE && HIWORD(wparam) == BN_CLICKED)
    {
        std::wstring path = dialog_support::ReadText(dialog, IDC_EXPORT_PATH);
        if (ui::ReportFileDialogResult(dialog, win32::ChooseFileToSave(dialog, ui::kRegFileFilter, path.empty() ? nullptr : path.c_str(), &path)))
        {
            state->confirmed_path = path;
            SetDlgItemTextW(dialog, IDC_EXPORT_PATH, path.c_str());
        }
        return TRUE;
    }
    if (id == IDOK)
    {
        state->value.path = util::EnsureFileExtension(dialog_support::ReadText(dialog, IDC_EXPORT_PATH), L".reg");
        if (state->value.path.empty())
        {
            ui::ShowError(dialog, util::Tr(L"Select a destination file."));
            return TRUE;
        }
        if (!ui::ConfirmOverwrite(dialog, state->value.path, state->confirmed_path))
        {
            return TRUE;
        }
        state->value.include_subkeys = IsDlgButtonChecked(dialog, IDC_EXPORT_RANGE_BRANCH) == BST_CHECKED;
        state->value.open_after = static_cast<win32::OpenAfter>(std::max<LRESULT>(0, SendDlgItemMessageW(dialog, IDC_EXPORT_OPEN_AFTER, CB_GETCURSEL, 0, 0)));
        state->accepted = true;
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if (id == IDCANCEL)
    {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

} // namespace

bool ChooseExport(HWND owner, const ExportRequest& request, ExportResult* result)
{
    if (!result)
    {
        return false;
    }
    State state;
    state.value = request;
    const INT_PTR dialog_result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_EXPORT_OPTIONS), owner, DialogProc, reinterpret_cast<LPARAM>(&state));
    if (dialog_result != IDOK || !state.accepted)
    {
        return false;
    }
    *result = std::move(state.value);
    return true;
}

} // namespace regkit::editors
