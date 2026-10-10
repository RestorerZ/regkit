// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "dialogs/query_dialog.h"

#include "dialogs/query_prompts.h"
#include "win32/text_transform.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <vector>

#include <commctrl.h>
#include <windowsx.h>

#include "dialogs/value_editor.h"
#include "records/escaped_fields.h"
#include "registry/registry_store.h"
#include "resource.h"
#include "ui/autocomplete.h"
#include "ui/dialog_support.h"
#include "ui/feedback.h"
#include "win32/file_text.h"
#include "win32/shell_paths.h"
#include "win32/translation.h"

namespace regkit
{

namespace
{

namespace dialog_support = editors::dialog_support;

struct SearchDialogState
{
    HWND dialog = nullptr;
    HFONT font = nullptr;
    SearchDialogResult* out = nullptr;
    SearchSources sources;
    bool recursive = true;
    std::vector<std::wstring> history;
    std::vector<std::wstring> root_names;
    std::vector<bool> root_selected;
    std::vector<DWORD> data_types;
};

HWND Item(const SearchDialogState* state, int id)
{
    return GetDlgItem(state->dialog, id);
}

constexpr std::pair<int, uint32_t> kAnomalyBoxes[] = {
    {IDC_FIND_NUL_NAMES, search::kAnomalyNulName},
    {IDC_FIND_ODD_NAMES, search::kAnomalyOddName},
    {IDC_FIND_INTEGER_SIZE, search::kAnomalyIntegerSize},
    {IDC_FIND_STRING_END, search::kAnomalyStringEnd},
    {IDC_FIND_MULTI_STRING, search::kAnomalyMultiString},
    {IDC_FIND_UNKNOWN_TYPE, search::kAnomalyUnknownType},
    {IDC_FIND_BROKEN_LINKS, search::kAnomalyBrokenLink},
    {IDC_FIND_VIRTUAL_STORE, search::kAnomalyVirtualStore},
};

std::wstring SearchHistoryPath()
{
    std::wstring folder = util::GetCacheFolder();
    if (folder.empty())
    {
        return L"";
    }
    return util::JoinPath(folder, L"search_history.txt");
}

std::vector<std::wstring> LoadSearchHistory()
{
    std::vector<std::wstring> items;
    std::wstring content;
    const std::wstring path = SearchHistoryPath();
    if (!path.empty() && util::ReadTextFile(path, &content, nullptr, util::kMaxStateFileBytes))
    {
        for (const std::wstring_view line : record_fields::Lines(content))
        {
            if (!line.empty())
            {
                items.emplace_back(line);
            }
        }
    }
    return items;
}

void SaveSearchHistory(const std::vector<std::wstring>& items)
{
    std::wstring content;
    for (const auto& item : items)
    {
        content.append(item).append(L"\n");
    }
    const std::wstring path = SearchHistoryPath();
    if (!path.empty() && !content.empty())
    {
        util::WriteTextFile(path, content, false);
    }
}

void UpdateHistoryList(std::vector<std::wstring>* items, const std::wstring& entry)
{
    if (!items || entry.empty())
    {
        return;
    }
    std::erase_if(*items, [&](const std::wstring& item) { return util::EqualsInsensitive(item, entry); });
    items->insert(items->begin(), entry);
    const size_t max_items = 20;
    if (items->size() > max_items)
    {
        items->resize(max_items);
    }
}

void UpdateScopeComboText(SearchDialogState* state)
{
    if (!state || !Item(state, IDC_FIND_SCOPE_ROOTS))
    {
        return;
    }
    size_t total = state->root_selected.size();
    size_t selected = 0;
    std::wstring first;
    for (size_t i = 0; i < state->root_selected.size(); ++i)
    {
        if (state->root_selected[i])
        {
            ++selected;
            if (first.empty() && i < state->root_names.size())
            {
                first = state->root_names[i];
            }
        }
    }
    std::wstring text;
    if (selected == 0)
    {
        text = util::Tr(L"No top level keys");
    }
    else if (selected == total)
    {
        text = util::Tr(L"All top level keys");
    }
    else if (selected == 1)
    {
        text = first;
    }
    else
    {
        text = util::Tr(L"Multiple keys");
    }
    SendMessageW(Item(state, IDC_FIND_SCOPE_ROOTS), CB_RESETCONTENT, 0, 0);
    SendMessageW(Item(state, IDC_FIND_SCOPE_ROOTS), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(Item(state, IDC_FIND_SCOPE_ROOTS), CB_SETCURSEL, 0, 0);
}

void ShowRootSelectionMenu(HWND owner, SearchDialogState* state)
{
    if (!owner || !state || !Item(state, IDC_FIND_SCOPE_ROOTS))
    {
        return;
    }
    RECT rect = {};
    GetWindowRect(Item(state, IDC_FIND_SCOPE_ROOTS), &rect);
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < state->root_names.size(); ++i)
    {
        UINT flags = MF_STRING;
        if (i < state->root_selected.size() && state->root_selected[i])
        {
            flags |= MF_CHECKED;
        }
        AppendMenuW(menu, flags, static_cast<UINT>(1000 + i), state->root_names[i].c_str());
    }
    int cmd =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rect.left, rect.bottom, 0, owner, nullptr);
    DestroyMenu(menu);
    if (cmd >= 1000)
    {
        size_t index = static_cast<size_t>(cmd - 1000);
        if (index < state->root_selected.size())
        {
            state->root_selected[index] = !state->root_selected[index];
            UpdateScopeComboText(state);
        }
    }
}

bool ParseUint64(const std::wstring& text, uint64_t* out)
{
    *out = 0;
    return record_fields::ParseUnsigned(text, UINT64_MAX, out);
}

bool GetDateTimeValue(HWND control, FILETIME* out)
{
    if (!control || !out)
    {
        return false;
    }
    SYSTEMTIME local = {};
    DWORD result = static_cast<DWORD>(SendMessageW(control, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&local)));
    if (result != GDT_VALID)
    {
        return false;
    }
    SYSTEMTIME utc = {};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc))
    {
        return false;
    }
    return SystemTimeToFileTime(&utc, out) != 0;
}

void SetDateTimeValue(HWND control, const FILETIME& value)
{
    if (!control)
    {
        return;
    }
    SYSTEMTIME utc = {};
    SYSTEMTIME local = {};
    if (!FileTimeToSystemTime(&value, &utc))
    {
        return;
    }
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
    {
        return;
    }
    SendMessageW(control, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&local));
}

std::vector<std::wstring> SplitExcludePaths(const std::wstring& text)
{
    std::vector<std::wstring> items;
    std::wstring current;
    for (wchar_t ch : text)
    {
        if (ch == L'\r' || ch == L'\n' || ch == L';' || ch == L',')
        {
            if (!current.empty())
            {
                items.push_back(current);
                current.clear();
            }
        }
        else
        {
            current.push_back(ch);
        }
    }
    if (!current.empty())
    {
        items.push_back(current);
    }
    for (auto& item : items)
    {
        item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](wchar_t c) { return c != L' '; }));
        while (!item.empty() && item.back() == L' ')
        {
            item.pop_back();
        }
    }
    items.erase(std::remove_if(items.begin(), items.end(), [](const std::wstring& value) { return value.empty(); }), items.end());
    return items;
}

std::wstring JoinExcludePaths(const std::vector<std::wstring>& items)
{
    std::wstring out;
    for (const auto& item : items)
    {
        if (item.empty())
        {
            continue;
        }
        if (!out.empty())
        {
            out.append(L", ");
        }
        out.append(item);
    }
    return out;
}

bool IsChecked(HWND button)
{
    return Button_GetCheck(button) == BST_CHECKED;
}

void SetChecked(HWND button, bool checked)
{
    Button_SetCheck(button, checked ? BST_CHECKED : BST_UNCHECKED);
}

void UpdateDialogEnableState(SearchDialogState* state)
{
    if (!state)
    {
        return;
    }

    bool scope_top = IsChecked(Item(state, IDC_FIND_SCOPE_TOP));
    bool scope_key = IsChecked(Item(state, IDC_FIND_SCOPE_KEY));
    bool standard_roots = IsChecked(Item(state, IDC_FIND_ROOT_KEYS));
    bool enable_roots = scope_top;
    EnableWindow(Item(state, IDC_FIND_SCOPE_ROOTS), enable_roots && standard_roots);
    EnableWindow(Item(state, IDC_FIND_SCOPE_EDIT), scope_key);
    EnableWindow(Item(state, IDC_FIND_BROWSE), scope_key);
    EnableWindow(Item(state, IDC_FIND_RECURSIVE), scope_key);

    bool search_data = IsChecked(Item(state, IDC_FIND_DATA));
    EnableWindow(Item(state, IDC_FIND_DATA_TYPES), search_data);
    EnableWindow(Item(state, IDC_FIND_MIN_SIZE), search_data);
    EnableWindow(Item(state, IDC_FIND_MAX_SIZE), search_data);

    bool min_checked = IsChecked(Item(state, IDC_FIND_MIN_SIZE));
    bool max_checked = IsChecked(Item(state, IDC_FIND_MAX_SIZE));
    EnableWindow(Item(state, IDC_FIND_MIN_SIZE_EDIT), search_data && min_checked);
    EnableWindow(Item(state, IDC_FIND_MAX_SIZE_EDIT), search_data && max_checked);

    bool exclude_checked = IsChecked(Item(state, IDC_FIND_EXCLUDE));
    EnableWindow(Item(state, IDC_FIND_EXCLUDE_EDIT), exclude_checked);

    bool limit_checked = IsChecked(Item(state, IDC_FIND_LIMIT));
    EnableWindow(Item(state, IDC_FIND_LIMIT_EDIT), limit_checked);
    EnableWindow(Item(state, IDC_FIND_EXCLUDE_BUTTON), exclude_checked);

    EnableWindow(Item(state, IDC_FIND_TRACE), state->sources.traces);
    EnableWindow(Item(state, IDC_FIND_DEFAULTS), state->sources.defaults);
    EnableWindow(Item(state, IDC_FIND_REGISTRY), state->sources.registry_root);
    EnableWindow(Item(state, IDC_FIND_OFFLINE), state->sources.offline);
    EnableWindow(Item(state, IDC_FIND_REG_FILES), state->sources.reg_files);
    EnableWindow(Item(state, IDC_FIND_REMOTE), state->sources.remote);
}

void LoadInitialState(SearchDialogState* state)
{
    state->history = LoadSearchHistory();
    dialog_support::SetComboItems(Item(state, IDC_FIND_WHAT), state->history);
    if (state->out && (!state->out->criteria.query.empty() || state->out->criteria.anomalies))
    {
        SetWindowTextW(Item(state, IDC_FIND_WHAT), state->out->criteria.query.c_str());
    }
    else if (!state->history.empty())
    {
        SetWindowTextW(Item(state, IDC_FIND_WHAT), state->history.front().c_str());
    }

    auto roots = RegistryStore::DefaultRoots(state->sources.extra_hives);
    state->root_names.clear();
    state->root_selected.clear();
    state->root_names.reserve(roots.size());
    for (const auto& root : roots)
    {
        state->root_names.push_back(root.path_name);
    }
    state->root_selected.assign(state->root_names.size(), true);
    if (state->out && !state->out->root_paths.empty())
    {
        state->root_selected.assign(state->root_names.size(), false);
        for (size_t i = 0; i < state->root_names.size(); ++i)
        {
            for (const auto& path : state->out->root_paths)
            {
                if (util::EqualsInsensitive(state->root_names[i], path))
                {
                    state->root_selected[i] = true;
                    break;
                }
            }
        }
    }
    UpdateScopeComboText(state);

    if (state->out)
    {
        state->data_types = state->out->criteria.allowed_types;
        state->recursive = state->out->criteria.recursive;
    }
    const SearchDialogResult* initial = state->out;
    if (initial)
    {
        SetChecked(Item(state, IDC_FIND_KEYS), initial->criteria.search_keys);
        SetChecked(Item(state, IDC_FIND_VALUES), initial->criteria.search_values);
        SetChecked(Item(state, IDC_FIND_DATA), initial->criteria.search_data);
        SetChecked(Item(state, IDC_FIND_COMMENTS), initial->criteria.search_comments);
        SetChecked(Item(state, IDC_FIND_CASE), initial->criteria.match_case);
        SetChecked(Item(state, IDC_FIND_WHOLE), initial->criteria.match_whole);
        SetChecked(Item(state, IDC_FIND_REGEX), initial->criteria.use_regex);
        SetChecked(Item(state, IDC_FIND_SKIP_LINKS), initial->criteria.skip_links);
        for (const auto& [id, kind] : kAnomalyBoxes)
        {
            SetChecked(Item(state, id), (initial->criteria.anomalies & kind) != 0);
        }
        if (initial->criteria.use_min_size)
        {
            SetChecked(Item(state, IDC_FIND_MIN_SIZE), true);
            SetWindowTextW(Item(state, IDC_FIND_MIN_SIZE_EDIT), std::to_wstring(initial->criteria.min_size).c_str());
        }
        if (initial->criteria.use_max_size)
        {
            SetChecked(Item(state, IDC_FIND_MAX_SIZE), true);
            SetWindowTextW(Item(state, IDC_FIND_MAX_SIZE_EDIT), std::to_wstring(initial->criteria.max_size).c_str());
        }
        if (initial->criteria.use_modified_from)
        {
            SetDateTimeValue(Item(state, IDC_FIND_MODIFIED_FROM), initial->criteria.modified_from);
        }
        if (initial->criteria.use_modified_to)
        {
            SetDateTimeValue(Item(state, IDC_FIND_MODIFIED_TO), initial->criteria.modified_to);
        }
        bool standard_hives = initial->search_standard_hives;
        bool registry_root = initial->search_registry_root;
        bool trace_values = initial->search_trace_values;
        if (!state->sources.registry_root)
        {
            registry_root = false;
        }
        if (!state->sources.traces)
        {
            trace_values = false;
        }
        SetChecked(Item(state, IDC_FIND_ROOT_KEYS), standard_hives);
        SetChecked(Item(state, IDC_FIND_REGISTRY), registry_root);
        SetChecked(Item(state, IDC_FIND_TRACE), trace_values);
        SetChecked(Item(state, IDC_FIND_DEFAULTS), initial->search_default_data && state->sources.defaults);
        SetChecked(Item(state, IDC_FIND_OFFLINE), initial->search_offline_hives && state->sources.offline);
        SetChecked(Item(state, IDC_FIND_REG_FILES), initial->search_reg_files && state->sources.reg_files);
        SetChecked(Item(state, IDC_FIND_REMOTE), initial->search_remote_registry && state->sources.remote);
        bool scope_top = initial->scope == SearchScope::kEntireRegistry;
        SetChecked(Item(state, IDC_FIND_SCOPE_TOP), scope_top);
        SetChecked(Item(state, IDC_FIND_SCOPE_KEY), !scope_top);
        if (!initial->start_key.empty())
        {
            SetWindowTextW(Item(state, IDC_FIND_SCOPE_EDIT), initial->start_key.c_str());
        }
        bool new_tab = initial->result_mode == SearchResultMode::kNewTab;
        SetChecked(Item(state, IDC_FIND_RESULT_REUSE), !new_tab);
        SetChecked(Item(state, IDC_FIND_RESULT_NEW), new_tab);
        SetChecked(Item(state, IDC_FIND_RESULT_TAB), initial->open_in_new_tab);
        const bool limited = initial->criteria.max_results > 0;
        SetChecked(Item(state, IDC_FIND_LIMIT), limited);
        SetWindowTextW(Item(state, IDC_FIND_LIMIT_EDIT), std::to_wstring(limited ? initial->criteria.max_results : 1000).c_str());
        SendMessageW(Item(state, IDC_FIND_LIMIT_EDIT), EM_SETSEL, 0, 0);
        EnableWindow(Item(state, IDC_FIND_LIMIT_EDIT), limited);
    }
    else
    {
        SetChecked(Item(state, IDC_FIND_KEYS), false);
        SetChecked(Item(state, IDC_FIND_VALUES), true);
        SetChecked(Item(state, IDC_FIND_DATA), true);
        SetChecked(Item(state, IDC_FIND_ROOT_KEYS), true);
        SetChecked(Item(state, IDC_FIND_REGISTRY), false);
        SetChecked(Item(state, IDC_FIND_TRACE), state->sources.traces);
        SetChecked(Item(state, IDC_FIND_SCOPE_TOP), true);
        SetChecked(Item(state, IDC_FIND_RESULT_REUSE), true);
        SetChecked(Item(state, IDC_FIND_LIMIT), true);
    }
    SetChecked(Item(state, IDC_FIND_RECURSIVE), state->recursive);
}

bool ReadSearchResult(HWND hwnd, SearchDialogState* state, SearchDialogResult* out)
{
    std::wstring query_text = util::WindowText(Item(state, IDC_FIND_WHAT));
    uint32_t anomalies = 0;
    for (const auto& [id, kind] : kAnomalyBoxes)
    {
        anomalies |= IsChecked(Item(state, id)) ? kind : 0u;
    }
    if (query_text.empty() && !anomalies)
    {
        ui::ShowWarning(hwnd, util::Tr(L"Enter a search term."));
        return false;
    }
    if (IsChecked(Item(state, IDC_FIND_REGEX)) && query_text.size() > search::regex::kMaxPatternLength)
    {
        ui::ShowWarning(hwnd, util::Tr(L"The regular expression is too long."));
        return false;
    }
    bool keys = IsChecked(Item(state, IDC_FIND_KEYS));
    bool values = IsChecked(Item(state, IDC_FIND_VALUES));
    bool data = IsChecked(Item(state, IDC_FIND_DATA));
    bool comments = IsChecked(Item(state, IDC_FIND_COMMENTS));
    bool default_data = state->sources.defaults && IsChecked(Item(state, IDC_FIND_DEFAULTS));
    if (!keys && !values && !data && !comments && !default_data && !anomalies)
    {
        ui::ShowWarning(hwnd, util::Tr(L"Select at least one search option."));
        return false;
    }
    bool standard_hives = IsChecked(Item(state, IDC_FIND_ROOT_KEYS));
    bool registry_root = IsChecked(Item(state, IDC_FIND_REGISTRY));
    bool trace_values = IsChecked(Item(state, IDC_FIND_TRACE));
    bool offline_hives = state->sources.offline && IsChecked(Item(state, IDC_FIND_OFFLINE));
    bool reg_files = state->sources.reg_files && IsChecked(Item(state, IDC_FIND_REG_FILES));
    bool remote_registry = state->sources.remote && IsChecked(Item(state, IDC_FIND_REMOTE));
    if (!state->sources.registry_root)
    {
        registry_root = false;
    }
    if (!state->sources.traces)
    {
        trace_values = false;
    }
    if (!standard_hives && !registry_root && !trace_values && !offline_hives && !reg_files && !remote_registry)
    {
        ui::ShowWarning(hwnd, util::Tr(L"Select at least one search source."));
        return false;
    }

    SearchDialogResult& result = *out;
    result.criteria.query = query_text;
    result.criteria.search_keys = keys;
    result.criteria.search_values = values;
    result.criteria.search_data = data;
    result.criteria.search_comments = comments;
    result.criteria.match_case = IsChecked(Item(state, IDC_FIND_CASE));
    result.criteria.match_whole = IsChecked(Item(state, IDC_FIND_WHOLE));
    result.criteria.use_regex = IsChecked(Item(state, IDC_FIND_REGEX));
    result.criteria.skip_links = IsChecked(Item(state, IDC_FIND_SKIP_LINKS));
    result.criteria.anomalies = anomalies;
    if (data)
    {
        result.criteria.allowed_types = state->data_types;
        if (IsChecked(Item(state, IDC_FIND_MIN_SIZE)))
        {
            wchar_t buffer[64] = {};
            GetWindowTextW(Item(state, IDC_FIND_MIN_SIZE_EDIT), buffer, static_cast<int>(_countof(buffer)));
            uint64_t value = 0;
            if (!ParseUint64(buffer, &value))
            {
                ui::ShowWarning(hwnd, util::Tr(L"Enter a valid minimum data size."));
                return false;
            }
            result.criteria.use_min_size = true;
            result.criteria.min_size = value;
        }
        if (IsChecked(Item(state, IDC_FIND_MAX_SIZE)))
        {
            wchar_t buffer[64] = {};
            GetWindowTextW(Item(state, IDC_FIND_MAX_SIZE_EDIT), buffer, static_cast<int>(_countof(buffer)));
            uint64_t value = 0;
            if (!ParseUint64(buffer, &value))
            {
                ui::ShowWarning(hwnd, util::Tr(L"Enter a valid maximum data size."));
                return false;
            }
            result.criteria.use_max_size = true;
            result.criteria.max_size = value;
        }
    }
    FILETIME modified_from = {};
    FILETIME modified_to = {};
    bool has_modified_from = GetDateTimeValue(Item(state, IDC_FIND_MODIFIED_FROM), &modified_from);
    bool has_modified_to = GetDateTimeValue(Item(state, IDC_FIND_MODIFIED_TO), &modified_to);
    if (has_modified_from)
    {
        result.criteria.use_modified_from = true;
        result.criteria.modified_from = modified_from;
    }
    if (has_modified_to)
    {
        result.criteria.use_modified_to = true;
        result.criteria.modified_to = modified_to;
    }
    if (result.criteria.use_min_size && result.criteria.use_max_size &&
        result.criteria.min_size > result.criteria.max_size)
    {
        ui::ShowWarning(hwnd, util::Tr(L"Minimum data size can't exceed maximum data size."));
        return false;
    }
    if (has_modified_from && has_modified_to && CompareFileTime(&modified_from, &modified_to) > 0)
    {
        ui::ShowWarning(hwnd, util::Tr(L"Modified date range is invalid."));
        return false;
    }
    result.search_standard_hives = standard_hives;
    result.search_registry_root = registry_root;
    result.search_trace_values = trace_values;
    result.search_default_data = default_data;
    result.search_offline_hives = offline_hives;
    result.search_reg_files = reg_files;
    result.search_remote_registry = remote_registry;

    bool scope_top = IsChecked(Item(state, IDC_FIND_SCOPE_TOP));
    result.scope = scope_top ? SearchScope::kEntireRegistry : SearchScope::kCurrentKey;
    state->recursive = IsChecked(Item(state, IDC_FIND_RECURSIVE));
    result.criteria.recursive = scope_top ? true : state->recursive;
    result.result_mode = IsChecked(Item(state, IDC_FIND_RESULT_NEW)) ? SearchResultMode::kNewTab : SearchResultMode::kReuseTab;
    result.open_in_new_tab = IsChecked(Item(state, IDC_FIND_RESULT_TAB));
    if (IsChecked(Item(state, IDC_FIND_LIMIT)))
    {
        wchar_t limit_text[32] = {};
        GetWindowTextW(Item(state, IDC_FIND_LIMIT_EDIT), limit_text, static_cast<int>(_countof(limit_text)));
        uint64_t limit = 0;
        if (!ParseUint64(limit_text, &limit) || limit == 0)
        {
            ui::ShowWarning(hwnd, util::Tr(L"Enter a valid result limit."));
            return false;
        }
        result.criteria.max_results = limit;
    }
    else
    {
        result.criteria.max_results = 0;
    }

    if (IsChecked(Item(state, IDC_FIND_EXCLUDE)))
    {
        result.criteria.exclude_paths = SplitExcludePaths(util::WindowText(Item(state, IDC_FIND_EXCLUDE_EDIT)));
    }

    result.root_paths.clear();
    result.start_key.clear();
    if (scope_top)
    {
        if (standard_hives)
        {
            for (size_t i = 0; i < state->root_selected.size(); ++i)
            {
                if (state->root_selected[i] && i < state->root_names.size())
                {
                    result.root_paths.push_back(state->root_names[i]);
                }
            }
            if (result.root_paths.empty())
            {
                ui::ShowWarning(hwnd, util::Tr(L"Select at least one top level key."));
                return false;
            }
        }
    }
    else
    {
        result.start_key = util::WindowText(Item(state, IDC_FIND_SCOPE_EDIT));
    }

    return true;
}

INT_PTR CALLBACK SearchDialogProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<SearchDialogState*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (msg == WM_INITDIALOG)
    {
        state = reinterpret_cast<SearchDialogState*>(lparam);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        state->dialog = hwnd;
        const std::wstring time_format = util::LocalTimeFormat();
        for (const int id : {IDC_FIND_MODIFIED_FROM, IDC_FIND_MODIFIED_TO})
        {
            SendDlgItemMessageW(hwnd, id, DTM_SETFORMAT, 0, reinterpret_cast<LPARAM>(time_format.c_str()));
            SendDlgItemMessageW(hwnd, id, DTM_SETSYSTEMTIME, GDT_NONE, 0);
        }
        appearance::AttachAutoComplete(Item(state, IDC_FIND_SCOPE_EDIT), appearance::SuggestKeys);
        ui::AddTooltip(
            hwnd,
            Item(state, IDC_FIND_REGEX),
            util::Tr(L"PCRE syntax: ^ $ anchors, character classes, greedy, lazy (*?) and possessive (*+) quantifiers,\n"
                     L"(?<name>...) groups, lookaround (?=...) (?<=...), backreferences \\1 and Unicode classes \\p{L}, \\w, "
                     L"\\X.\n"
                     L"Matching is unicode aware and ignores case unless 'Match case' is set.")
        );
        SetDlgItemTextW(hwnd, IDC_FIND_LIMIT_EDIT, L"1000");
        LoadInitialState(state);
        UpdateDialogEnableState(state);
        dialog_support::Initialize(hwnd, &state->font, {IDC_FIND_SCOPE_EDIT, IDC_FIND_MIN_SIZE_EDIT, IDC_FIND_MAX_SIZE_EDIT, IDC_FIND_EXCLUDE_EDIT, IDC_FIND_LIMIT_EDIT});
        SetFocus(Item(state, IDC_FIND_WHAT));
        SendDlgItemMessageW(hwnd, IDC_FIND_WHAT, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
        return FALSE;
    }
    if (!state)
    {
        return FALSE;
    }
    if (msg == WM_DESTROY)
    {
        dialog_support::ReleaseFont(&state->font);
        return TRUE;
    }
    INT_PTR themed = 0;
    if (dialog_support::HandleThemeMessage(hwnd, msg, wparam, lparam, &themed))
    {
        return themed;
    }
    switch (msg)
    {
    case WM_COMMAND:
        {
            if (HIWORD(wparam) == CBN_DROPDOWN && LOWORD(wparam) == IDC_FIND_SCOPE_ROOTS)
            {
                ShowRootSelectionMenu(hwnd, state);
                SendMessageW(Item(state, IDC_FIND_SCOPE_ROOTS), CB_SHOWDROPDOWN, FALSE, 0);
                return TRUE;
            }
            if (HIWORD(wparam) == BN_CLICKED)
            {
                switch (LOWORD(wparam))
                {
                case IDC_FIND_SCOPE_TOP:
                case IDC_FIND_SCOPE_KEY:
                case IDC_FIND_DATA:
                case IDC_FIND_MIN_SIZE:
                case IDC_FIND_MAX_SIZE:
                case IDC_FIND_ROOT_KEYS:
                case IDC_FIND_REGISTRY:
                case IDC_FIND_TRACE:
                case IDC_FIND_OFFLINE:
                case IDC_FIND_REG_FILES:
                case IDC_FIND_REMOTE:
                case IDC_FIND_EXCLUDE:
                case IDC_FIND_LIMIT:
                    UpdateDialogEnableState(state);
                    break;
                default:
                    break;
                }
            }
            switch (LOWORD(wparam))
            {
            case IDC_FIND_BROWSE:
                {
                    std::wstring selected;
                    if (ShowBrowseKeyDialog(hwnd, &selected))
                    {
                        if (!selected.empty())
                        {
                            SetWindowTextW(Item(state, IDC_FIND_SCOPE_EDIT), selected.c_str());
                        }
                        SetChecked(Item(state, IDC_FIND_SCOPE_KEY), true);
                        SetChecked(Item(state, IDC_FIND_SCOPE_TOP), false);
                        UpdateDialogEnableState(state);
                    }
                    return TRUE;
                }
            case IDC_FIND_DATA_TYPES:
                query_prompts::ShowDataTypes(hwnd, &state->data_types);
                return TRUE;
            case IDC_FIND_EXCLUDE_BUTTON:
                {
                    editors::TextRequest request;
                    request.title = util::Tr(L"Exclude Keys");
                    request.label = util::Tr(L"Each line should include one key.");
                    request.text = util::JoinLines(SplitExcludePaths(util::WindowText(Item(state, IDC_FIND_EXCLUDE_EDIT))));
                    request.multiline = true;
                    request.browse = ShowBrowseKeyDialog;
                    editors::TextResult result;
                    if (editors::EditText(hwnd, request, &result))
                    {
                        SetWindowTextW(Item(state, IDC_FIND_EXCLUDE_EDIT), JoinExcludePaths(SplitExcludePaths(result.text)).c_str());
                    }
                    return TRUE;
                }
            case IDOK:
                {
                    SearchDialogResult result;
                    if (!ReadSearchResult(hwnd, state, &result))
                    {
                        return TRUE;
                    }
                    UpdateHistoryList(&state->history, result.criteria.query);
                    SaveSearchHistory(state->history);

                    *state->out = std::move(result);
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }
            case IDCANCEL:
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            default:
                break;
            }
            break;
        }
    default:
        break;
    }
    return FALSE;
}

} // namespace

bool ShowBrowseKeyDialog(HWND owner, std::wstring* selected_path)
{
    return query_prompts::ShowRegistryKey(owner, selected_path);
}

bool ShowSearchDialog(HWND owner, SearchDialogResult* result, const SearchSources& available)
{
    SearchDialogState state;
    state.out = result;
    state.sources = available;
    return result && dialog_support::Modal(owner, IDD_FIND, SearchDialogProc, reinterpret_cast<LPARAM>(&state)) == IDOK;
}

} // namespace regkit
