// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/search/compare_dialog.h"

#include "frame/window_detail.h"

#include <commctrl.h>
#include <shellapi.h>

#include "regfile/reg_file.h"
#include "registry/registry_path.h"
#include "resource.h"
#include "ui/autocomplete.h"
#include "ui/dialog_support.h"
#include "ui/feedback.h"
#include "win32/file_dialog.h"
#include "win32/translation.h"

namespace regkit::command_detail
{

using namespace window_detail;

namespace
{

const wchar_t* CompareSourceLabel(CompareSourceType type)
{
    switch (type)
    {
    case CompareSourceType::kRegFile:
        return util::Tr(L"Reg File");
    case CompareSourceType::kNetwork:
        return util::Tr(L"Network Registry");
    case CompareSourceType::kOfflineHive:
        return util::Tr(L"Offline Hive");
    default:
        return util::Tr(L"Registry");
    }
}

CompareSourceType CompareSourceFromIndex(int index)
{
    switch (index)
    {
    case 1:
        return CompareSourceType::kRegFile;
    case 2:
        return CompareSourceType::kOfflineHive;
    case 3:
        return CompareSourceType::kNetwork;
    default:
        return CompareSourceType::kRegistry;
    }
}
struct CompareDialogState
{
    CompareDialogDefaults data;
    HFONT ui_font = nullptr;
};

std::vector<std::wstring> ExtractRegFileKeys(const regfile::Document& data)
{
    std::vector<std::wstring> keys = data.key_order;
    if (keys.empty())
    {
        keys.reserve(data.keys.size());
        for (const auto& entry : data.keys)
        {
            keys.push_back(entry.second.path);
        }
    }
    std::sort(keys.begin(), keys.end(), [](const std::wstring& a, const std::wstring& b) { return util::CompareInsensitive(a, b) < 0; });
    return keys;
}

void SetComboSelection(HWND combo, const std::wstring& value)
{
    if (!combo)
    {
        return;
    }
    const LRESULT index = value.empty() ? CB_ERR : SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(value.c_str()));
    if (index != CB_ERR)
    {
        SendMessageW(combo, CB_SETCURSEL, index, 0);
        return;
    }
    if (SendMessageW(combo, CB_GETCOUNT, 0, 0) > 0)
    {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }
}

void ToggleCompareControls(HWND dlg, bool left, CompareSourceType type)
{
    int file_id = left ? IDC_COMPARE_LEFT_FILE : IDC_COMPARE_RIGHT_FILE;
    int label_id = left ? IDC_COMPARE_LEFT_FILE_LABEL : IDC_COMPARE_RIGHT_FILE_LABEL;
    int browse_id = left ? IDC_COMPARE_LEFT_BROWSE : IDC_COMPARE_RIGHT_BROWSE;
    const bool network = type == CompareSourceType::kNetwork;
    const bool local = type == CompareSourceType::kRegistry;
    EnableWindow(GetDlgItem(dlg, file_id), !local);
    EnableWindow(GetDlgItem(dlg, browse_id), !local);
    SetDlgItemTextW(dlg, label_id, network ? util::Tr(L"Computer:") : util::Tr(L"File:"));
}

void PopulateFileKeys(HWND dlg, bool left)
{
    std::wstring file_path = util::DialogText(dlg, left ? IDC_COMPARE_LEFT_FILE : IDC_COMPARE_RIGHT_FILE);
    if (file_path.empty())
    {
        return;
    }
    regfile::Document data;
    std::wstring error;
    if (!regfile::Load(file_path, &data, &error))
    {
        return;
    }
    std::vector<std::wstring> keys = ExtractRegFileKeys(data);
    HWND combo = GetDlgItem(dlg, left ? IDC_COMPARE_LEFT_KEY : IDC_COMPARE_RIGHT_KEY);
    std::wstring current = util::WindowText(combo);
    editors::dialog_support::SetComboItems(combo, keys);
    if (!current.empty())
    {
        SetComboSelection(combo, current);
    }
    else if (!keys.empty())
    {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        SetDlgItemTextW(dlg, left ? IDC_COMPARE_LEFT_KEY : IDC_COMPARE_RIGHT_KEY, keys.front().c_str());
    }
}

INT_PTR CALLBACK CompareDialogProc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<CompareDialogState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg)
    {
    case WM_INITDIALOG:
        {
            state = reinterpret_cast<CompareDialogState*>(lparam);
            SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            if (!state)
            {
                return TRUE;
            }
            for (const int id : {IDC_COMPARE_LEFT_SOURCE, IDC_COMPARE_RIGHT_SOURCE})
            {
                editors::dialog_support::SetComboItems(GetDlgItem(dlg, id), {CompareSourceLabel(CompareSourceType::kRegistry), CompareSourceLabel(CompareSourceType::kRegFile), CompareSourceLabel(CompareSourceType::kOfflineHive), CompareSourceLabel(CompareSourceType::kNetwork)});
            }

            SetComboSelection(GetDlgItem(dlg, IDC_COMPARE_LEFT_SOURCE), CompareSourceLabel(state->data.left.type));
            SetComboSelection(GetDlgItem(dlg, IDC_COMPARE_RIGHT_SOURCE), CompareSourceLabel(state->data.right.type));
            SetDlgItemTextW(dlg, IDC_COMPARE_LEFT_FILE, state->data.left.file_path.c_str());
            SetDlgItemTextW(dlg, IDC_COMPARE_RIGHT_FILE, state->data.right.file_path.c_str());
            SetDlgItemTextW(dlg, IDC_COMPARE_LEFT_KEY, state->data.left.key_path.c_str());
            SetDlgItemTextW(dlg, IDC_COMPARE_RIGHT_KEY, state->data.right.key_path.c_str());
            CheckDlgButton(dlg, IDC_COMPARE_LEFT_RECURSIVE, state->data.left.recursive ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_COMPARE_RIGHT_RECURSIVE, state->data.right.recursive ? BST_CHECKED : BST_UNCHECKED);
            int filter_id = IDC_COMPARE_SHOW_DIFFERENCES;
            if (state->data.filter == search::compare::RowFilter::kMatches)
            {
                filter_id = IDC_COMPARE_SHOW_MATCHING;
            }
            else if (state->data.filter == search::compare::RowFilter::kAll)
            {
                filter_id = IDC_COMPARE_SHOW_BOTH;
            }
            CheckRadioButton(dlg, IDC_COMPARE_SHOW_DIFFERENCES, IDC_COMPARE_SHOW_BOTH, filter_id);

            PopulateFileKeys(dlg, true);
            PopulateFileKeys(dlg, false);
            for (const bool left : {true, false})
            {
                HWND combo = GetDlgItem(dlg, left ? IDC_COMPARE_LEFT_KEY : IDC_COMPARE_RIGHT_KEY);
                HWND source = GetDlgItem(dlg, left ? IDC_COMPARE_LEFT_SOURCE : IDC_COMPARE_RIGHT_SOURCE);
                appearance::AttachAutoComplete(combo, [combo, source](const std::wstring& text) -> std::vector<std::wstring> {
                    switch (CompareSourceFromIndex(static_cast<int>(SendMessageW(source, CB_GETCURSEL, 0, 0))))
                    {
                    case CompareSourceType::kRegistry:
                        return appearance::SuggestKeys(text);
                    case CompareSourceType::kRegFile:
                        return appearance::SuggestComboPaths(combo, text);
                    default:
                        return {};
                    }
                });
            }

            ToggleCompareControls(dlg, true, state->data.left.type);
            ToggleCompareControls(dlg, false, state->data.right.type);
            editors::dialog_support::Initialize(dlg, &state->ui_font, {IDC_COMPARE_LEFT_FILE, IDC_COMPARE_RIGHT_FILE});
            editors::dialog_support::MatchComboHeights(dlg, IDC_COMPARE_LEFT_FILE, {IDC_COMPARE_LEFT_SOURCE, IDC_COMPARE_LEFT_KEY, IDC_COMPARE_RIGHT_SOURCE, IDC_COMPARE_RIGHT_KEY});
            return TRUE;
        }
    case WM_DESTROY:
        if (state)
        {
            editors::dialog_support::ReleaseFont(&state->ui_font);
        }
        return TRUE;
    case WM_COMMAND:
        {
            if (!state)
            {
                return TRUE;
            }
            int id = LOWORD(wparam);
            int code = HIWORD(wparam);
            if (code == CBN_SELCHANGE && (id == IDC_COMPARE_LEFT_SOURCE || id == IDC_COMPARE_RIGHT_SOURCE))
            {
                bool left = id == IDC_COMPARE_LEFT_SOURCE;
                HWND combo = GetDlgItem(dlg, id);
                int sel = combo ? static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)) : 0;
                CompareSourceType type = CompareSourceFromIndex(sel);
                ToggleCompareControls(dlg, left, type);
                SetDlgItemTextW(dlg, left ? IDC_COMPARE_LEFT_FILE : IDC_COMPARE_RIGHT_FILE, L"");
                SetDlgItemTextW(dlg, left ? IDC_COMPARE_LEFT_KEY : IDC_COMPARE_RIGHT_KEY, L"");
                return TRUE;
            }
            if (code == EN_KILLFOCUS && (id == IDC_COMPARE_LEFT_FILE || id == IDC_COMPARE_RIGHT_FILE))
            {
                const bool left = id == IDC_COMPARE_LEFT_FILE;
                HWND source = GetDlgItem(dlg, left ? IDC_COMPARE_LEFT_SOURCE : IDC_COMPARE_RIGHT_SOURCE);
                if (CompareSourceFromIndex(static_cast<int>(SendMessageW(source, CB_GETCURSEL, 0, 0))) == CompareSourceType::kRegFile)
                {
                    PopulateFileKeys(dlg, left);
                }
                return TRUE;
            }
            if (code == BN_CLICKED && (id == IDC_COMPARE_LEFT_BROWSE || id == IDC_COMPARE_RIGHT_BROWSE))
            {
                bool left = id == IDC_COMPARE_LEFT_BROWSE;
                HWND browse_source = GetDlgItem(dlg, left ? IDC_COMPARE_LEFT_SOURCE : IDC_COMPARE_RIGHT_SOURCE);
                const CompareSourceType browse_type = CompareSourceFromIndex(
                    browse_source ? static_cast<int>(SendMessageW(browse_source, CB_GETCURSEL, 0, 0)) : 0
                );
                std::wstring path;
                if (browse_type == CompareSourceType::kNetwork)
                {
                    if (FAILED(win32::ChooseComputer(dlg, &path)) || path.empty())
                    {
                        return TRUE;
                    }
                    SetDlgItemTextW(dlg, left ? IDC_COMPARE_LEFT_FILE : IDC_COMPARE_RIGHT_FILE, path.c_str());
                    return TRUE;
                }
                if (!ui::PromptOpenFile(dlg, browse_type == CompareSourceType::kOfflineHive ? L"Registry Hive Files\0*.*\0\0" : ui::kRegFileFilter, &path))
                {
                    return TRUE;
                }
                SetDlgItemTextW(dlg, left ? IDC_COMPARE_LEFT_FILE : IDC_COMPARE_RIGHT_FILE, path.c_str());
                if (browse_type == CompareSourceType::kOfflineHive)
                {
                    return TRUE;
                }
                regfile::Document data;
                std::wstring error;
                if (regfile::Load(path, &data, &error))
                {
                    std::vector<std::wstring> keys = ExtractRegFileKeys(data);
                    if (keys.empty())
                    {
                        ui::ShowError(dlg, util::Tr(L"No registry keys were found in the .reg file."));
                        return TRUE;
                    }
                    HWND combo = GetDlgItem(dlg, left ? IDC_COMPARE_LEFT_KEY : IDC_COMPARE_RIGHT_KEY);
                    editors::dialog_support::SetComboItems(combo, keys);
                    if (!keys.empty())
                    {
                        SendMessageW(combo, CB_SETCURSEL, 0, 0);
                        SetDlgItemTextW(dlg, left ? IDC_COMPARE_LEFT_KEY : IDC_COMPARE_RIGHT_KEY, keys.front().c_str());
                    }
                }
                else if (!error.empty())
                {
                    ui::ShowError(dlg, error);
                }
                return TRUE;
            }
            if (id == IDOK)
            {
                CompareDialogResult result;
                auto read_side = [&](bool left, CompareDialogSelection* out) -> bool {
                    out->recursive = IsDlgButtonChecked(dlg, left ? IDC_COMPARE_LEFT_RECURSIVE : IDC_COMPARE_RIGHT_RECURSIVE) == BST_CHECKED;
                    HWND source_combo = GetDlgItem(dlg, left ? IDC_COMPARE_LEFT_SOURCE : IDC_COMPARE_RIGHT_SOURCE);
                    int source_index = source_combo ? static_cast<int>(SendMessageW(source_combo, CB_GETCURSEL, 0, 0)) : 0;
                    out->type = CompareSourceFromIndex(source_index);
                    out->key_path =
                        TrimWhitespace(util::WindowText(GetDlgItem(dlg, left ? IDC_COMPARE_LEFT_KEY : IDC_COMPARE_RIGHT_KEY)));
                    if (out->type == CompareSourceType::kRegistry || out->type == CompareSourceType::kNetwork)
                    {
                        if (out->key_path.empty())
                        {
                            ui::ShowError(dlg, util::Tr(L"Registry key is required."));
                            return false;
                        }
                        if (out->type == CompareSourceType::kNetwork)
                        {
                            out->file_path = TrimWhitespace(
                                util::DialogText(dlg, left ? IDC_COMPARE_LEFT_FILE : IDC_COMPARE_RIGHT_FILE)
                            );
                            if (out->file_path.empty())
                            {
                                ui::ShowError(dlg, util::Tr(L"Computer name is required."));
                                return false;
                            }
                        }
                        return true;
                    }
                    out->file_path =
                        TrimWhitespace(util::DialogText(dlg, left ? IDC_COMPARE_LEFT_FILE : IDC_COMPARE_RIGHT_FILE));
                    if (out->file_path.empty())
                    {
                        ui::ShowError(dlg, out->type == CompareSourceType::kOfflineHive ? util::Tr(L"Hive file path is required.") : util::Tr(L"Registry file path is required."));
                        return false;
                    }
                    if (out->type == CompareSourceType::kOfflineHive)
                    {
                        return true;
                    }
                    regfile::Document data;
                    std::wstring error;
                    if (!regfile::Load(out->file_path, &data, &error))
                    {
                        ui::ShowError(dlg, error.empty() ? util::Tr(L"Failed to read registry file.") : error);
                        return false;
                    }
                    std::vector<std::wstring> keys = ExtractRegFileKeys(data);
                    if (keys.empty())
                    {
                        ui::ShowError(dlg, util::Tr(L"No registry keys were found in the .reg file."));
                        return false;
                    }
                    if (out->key_path.empty())
                    {
                        out->key_path = keys.front();
                    }
                    std::wstring key_lower = ToLower(out->key_path);
                    bool found = false;
                    for (const auto& key : keys)
                    {
                        if (util::EqualsInsensitive(key, out->key_path))
                        {
                            found = true;
                            break;
                        }
                        std::wstring key_check = ToLower(key);
                        if (StartsWithInsensitive(key_check, key_lower) || StartsWithInsensitive(key_lower, key_check))
                        {
                            found = true;
                        }
                    }
                    if (!found)
                    {
                        ui::ShowError(dlg, util::Tr(L"The selected key path wasn't found in the .reg file."));
                        return false;
                    }
                    return true;
                };
                if (!read_side(true, &result.left))
                {
                    return TRUE;
                }
                if (!read_side(false, &result.right))
                {
                    return TRUE;
                }
                if (IsDlgButtonChecked(dlg, IDC_COMPARE_SHOW_MATCHING) == BST_CHECKED)
                {
                    result.filter = search::compare::RowFilter::kMatches;
                }
                else if (IsDlgButtonChecked(dlg, IDC_COMPARE_SHOW_BOTH) == BST_CHECKED)
                {
                    result.filter = search::compare::RowFilter::kAll;
                }
                state->data.left = result.left;
                state->data.right = result.right;
                state->data.filter = result.filter;
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL)
            {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
        }
    default:
        break;
    }
    INT_PTR themed = 0;
    return editors::dialog_support::HandleThemeMessage(dlg, msg, wparam, lparam, &themed) ? themed : FALSE;
}
} // namespace

bool ShowCompareDialog(HWND owner, const CompareDialogDefaults& defaults, CompareDialogResult* out)
{
    if (!out)
    {
        return false;
    }
    CompareDialogState state;
    state.data = defaults;
    INT_PTR result = editors::dialog_support::Modal(owner, IDD_COMPARE, CompareDialogProc, reinterpret_cast<LPARAM>(&state));
    if (result != IDOK)
    {
        return false;
    }
    out->left = state.data.left;
    out->right = state.data.right;
    out->filter = state.data.filter;
    return true;
}

} // namespace regkit::command_detail
