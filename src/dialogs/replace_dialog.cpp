// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "dialogs/replace_dialog.h"

#include "dialogs/query_dialog.h"
#include "resource.h"
#include "ui/autocomplete.h"
#include "ui/dialog_support.h"
#include "ui/feedback.h"
#include "win32/translation.h"

namespace regkit
{

namespace
{

namespace dialog_support = editors::dialog_support;

struct State
{
    ReplaceDialogResult* out = nullptr;
    HFONT font = nullptr;
};

constexpr struct
{
    int id;
    bool ReplaceDialogResult::* field;
} kChecks[] = {
    {IDC_REPLACE_RECURSIVE, &ReplaceDialogResult::recursive},
    {IDC_REPLACE_CASE, &ReplaceDialogResult::match_case},
    {IDC_REPLACE_WHOLE, &ReplaceDialogResult::match_whole},
    {IDC_REPLACE_REGEX, &ReplaceDialogResult::use_regex},
    {IDC_REPLACE_KEYS, &ReplaceDialogResult::replace_keys},
    {IDC_REPLACE_VALUES, &ReplaceDialogResult::replace_values},
    {IDC_REPLACE_DATA, &ReplaceDialogResult::replace_data},
    {IDC_REPLACE_DECIMAL, &ReplaceDialogResult::number_decimal},
    {IDC_REPLACE_HEX, &ReplaceDialogResult::number_hex},
};

void UpdateValueDataOptions(HWND dialog)
{
    const bool enabled = IsDlgButtonChecked(dialog, IDC_REPLACE_DATA) == BST_CHECKED;
    for (const int id : {IDC_REPLACE_VALUE_GROUP, IDC_REPLACE_DECIMAL, IDC_REPLACE_HEX})
    {
        EnableWindow(GetDlgItem(dialog, id), enabled);
    }
}

bool Accept(HWND dialog, State* state)
{
    ReplaceDialogResult result;
    result.find_text = dialog_support::ReadText(dialog, IDC_REPLACE_FIND);
    if (result.find_text.empty())
    {
        ui::ShowWarning(dialog, util::Tr(L"Enter text to find."));
        return false;
    }
    result.replace_text = dialog_support::ReadText(dialog, IDC_REPLACE_WITH);
    result.start_key = dialog_support::ReadText(dialog, IDC_REPLACE_KEY);
    for (const auto& check : kChecks)
    {
        result.*check.field = IsDlgButtonChecked(dialog, check.id) == BST_CHECKED;
    }
    if (!result.replace_keys && !result.replace_values && !result.replace_data)
    {
        ui::ShowWarning(dialog, util::Tr(L"Select what should be replaced."));
        return false;
    }
    *state->out = std::move(result);
    return true;
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG)
    {
        state = reinterpret_cast<State*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        const ReplaceDialogResult& initial = *state->out;
        SetDlgItemTextW(dialog, IDC_REPLACE_FIND, initial.find_text.c_str());
        SetDlgItemTextW(dialog, IDC_REPLACE_WITH, initial.replace_text.c_str());
        SetDlgItemTextW(dialog, IDC_REPLACE_KEY, initial.start_key.c_str());
        for (const auto& check : kChecks)
        {
            CheckDlgButton(dialog, check.id, initial.*check.field ? BST_CHECKED : BST_UNCHECKED);
        }
        UpdateValueDataOptions(dialog);
        appearance::AttachAutoComplete(GetDlgItem(dialog, IDC_REPLACE_KEY), appearance::SuggestKeys);
        ui::AddTooltip(
            dialog,
            GetDlgItem(dialog, IDC_REPLACE_REGEX),
            util::Tr(L"PCRE syntax: ^ $ anchors, character classes, greedy, lazy (*?) and possessive (*+) quantifiers,\n"
                     L"(?<name>...) groups, lookaround (?=...) (?<=...), backreferences \\1 and Unicode classes \\p{L}, \\w, "
                     L"\\X.\n"
                     L"Replace with: $1 or ${1} for a group, $<name> or ${name} for a named group, $& for the whole match, $$ "
                     L"for a dollar.")
        );
        dialog_support::Initialize(dialog, &state->font, {IDC_REPLACE_FIND, IDC_REPLACE_WITH, IDC_REPLACE_KEY});
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
    switch (LOWORD(wparam))
    {
    case IDC_REPLACE_DATA:
        UpdateValueDataOptions(dialog);
        return TRUE;
    case IDC_REPLACE_BROWSE:
        {
            std::wstring selected;
            if (ShowBrowseKeyDialog(dialog, &selected) && !selected.empty())
            {
                SetDlgItemTextW(dialog, IDC_REPLACE_KEY, selected.c_str());
            }
            return TRUE;
        }
    case IDOK:
        if (Accept(dialog, state))
        {
            EndDialog(dialog, IDOK);
        }
        return TRUE;
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

bool ShowReplaceDialog(HWND owner, ReplaceDialogResult* result)
{
    State state;
    state.out = result;
    return result && DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_REPLACE), owner, DialogProc, reinterpret_cast<LPARAM>(&state)) == IDOK;
}

} // namespace regkit
