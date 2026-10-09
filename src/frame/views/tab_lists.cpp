// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"

#include <unordered_set>

#include "frame/window/shortcut_bindings.h"
#include "win32/translation.h"

namespace regkit
{
using namespace window_detail;

namespace
{

struct StableListSelection
{
    std::unordered_set<std::wstring> selected;
    std::wstring focused;
};

void AppendIdentityPart(std::wstring* key, const std::wstring& part)
{
    key->push_back(L'|');
    key->append(std::to_wstring(part.size()));
    key->push_back(L':');
    key->append(part);
}

std::wstring ValueRowIdentity(const ListRow& row)
{
    std::wstring key = std::to_wstring(static_cast<long long>(row.kind));
    AppendIdentityPart(&key, row.extra);
    return key;
}

std::wstring SearchResultIdentity(const search::Result& result)
{
    return std::to_wstring(result.row_id);
}

std::wstring CompareRowIdentity(const search::compare::Row& row)
{
    std::wstring key = row.is_key ? L"1" : L"0";
    AppendIdentityPart(&key, row.first_key_path);
    AppendIdentityPart(&key, row.second_key_path);
    AppendIdentityPart(&key, row.value_name);
    return key;
}

std::wstring HistoryEntryIdentity(const HistoryEntry& entry)
{
    std::wstring key = std::to_wstring(entry.timestamp);
    AppendIdentityPart(&key, entry.action);
    AppendIdentityPart(&key, entry.key_path);
    AppendIdentityPart(&key, entry.value_name);
    return key;
}

template <typename KeyAt>
StableListSelection CaptureListSelection(HWND list, KeyAt key_at)
{
    StableListSelection state;
    if (!list)
    {
        return state;
    }
    state.selected.reserve(static_cast<size_t>(ListView_GetSelectedCount(list)));
    int index = -1;
    while ((index = ListView_GetNextItem(list, index, LVNI_SELECTED)) >= 0)
    {
        std::wstring key = key_at(index);
        if (!key.empty())
        {
            state.selected.emplace(std::move(key));
        }
    }
    index = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
    if (index >= 0)
    {
        state.focused = key_at(index);
    }
    return state;
}

template <typename KeyAt>
void RestoreListSelection(HWND list, const StableListSelection& state, KeyAt key_at)
{
    if (!list)
    {
        return;
    }
    if (state.selected.empty() && state.focused.empty())
    {
        return;
    }
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    int focused_index = -1;
    const int count = ListView_GetItemCount(list);
    for (int index = 0; index < count; ++index)
    {
        std::wstring key = key_at(index);
        UINT state_mask = 0;
        if (state.selected.find(key) != state.selected.end())
        {
            state_mask |= LVIS_SELECTED;
        }
        if (!state.focused.empty() && key == state.focused)
        {
            state_mask |= LVIS_FOCUSED;
            focused_index = index;
        }
        if (state_mask != 0)
        {
            ListView_SetItemState(list, index, state_mask, state_mask);
        }
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    if (focused_index >= 0)
    {
        ListView_EnsureVisible(list, focused_index, FALSE);
    }
    RedrawWindow(list, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
}

} // namespace

void MainWindow::Impl::SortValueList(int column, bool toggle)
{
    if (column < 0 || static_cast<size_t>(column) >= browse_.columns().items.size())
    {
        return;
    }
    appearance::UpdateListSortState(column, toggle, &browse_.columns().sort_column, &browse_.columns().sort_ascending);

    if (value_list_loading_ && browse_.current_node())
    {
        UpdateValueListForNode(browse_.current_node());
        return;
    }

    StableListSelection selection = CaptureListSelection(browse_.values().hwnd(), [this](int index) {
        const ListRow* row = browse_.values().RowAt(index);
        return row ? ValueRowIdentity(*row) : std::wstring();
    });
    auto& rows = browse_.values().rows();
    if (browse_.columns().sort_column == kValueColData)
    {
        bool needs_data = false;
        for (const auto& row : rows)
        {
            if (row.kind == rowkind::kValue && (!row.data_ready || row.data_preview))
            {
                needs_data = true;
                break;
            }
        }
        if (needs_data && browse_.current_node())
        {
            UpdateValueListForNode(browse_.current_node());
            return;
        }
        for (auto& row : rows)
        {
            EnsureValueRowData(&row);
        }
    }
    const bool was_updating = updating_value_list_;
    updating_value_list_ = true;
    SortValueRows(&rows, browse_.columns().sort_column, browse_.columns().sort_ascending);
    browse_.values().RebuildFilter();
    RestoreListSelection(browse_.values().hwnd(), selection, [this](int index) {
        const ListRow* row = browse_.values().RowAt(index);
        return row ? ValueRowIdentity(*row) : std::wstring();
    });
    updating_value_list_ = was_updating;
    if (!was_updating)
    {
        UpdateStatus();
    }

    appearance::UpdateListViewSort(browse_.values().hwnd(), browse_.columns().sort_column, browse_.columns().sort_ascending);
}

void MainWindow::Impl::SortHistoryList(int column, bool toggle)
{
    if (!history_list_ || column < 0)
    {
        return;
    }
    appearance::UpdateListSortState(column, toggle, &history_sort_column_, &history_sort_ascending_);

    auto history_key_at = [this](int index) {
        const auto& entries = change_history_.entries();
        if (index < 0 || static_cast<size_t>(index) >= entries.size())
        {
            return std::wstring();
        }
        return HistoryEntryIdentity(entries[static_cast<size_t>(index)]);
    };
    StableListSelection selection = CaptureListSelection(history_list_, history_key_at);
    change_history_.Sort(history_sort_column_, history_sort_ascending_);
    RebuildHistoryList();
    RestoreListSelection(history_list_, selection, history_key_at);

    appearance::UpdateListViewSort(history_list_, history_sort_column_, history_sort_ascending_);
}

void MainWindow::Impl::SortSearchTabResults(SearchTab* tab)
{
    if (!tab)
    {
        return;
    }
    tab->sort_dirty = false;
    if (tab->is_compare && tab->sort_column < 0)
    {
        return;
    }
    const int shown_index = SearchIndexFromTab(TabCtrl_GetCurSel(tab_));
    const bool shown = shown_index >= 0 && static_cast<size_t>(shown_index) < search_tabs_.size() && &search_tabs_[static_cast<size_t>(shown_index)] == tab;
    auto key_at = [tab](int row) {
        if (row < 0)
        {
            return std::wstring();
        }
        if (tab->is_compare)
        {
            return static_cast<size_t>(row) < tab->compare_rows.size() ? CompareRowIdentity(tab->compare_rows[static_cast<size_t>(row)]) : std::wstring();
        }
        return static_cast<size_t>(row) < tab->results.size() ? SearchResultIdentity(tab->results[static_cast<size_t>(row)]) : std::wstring();
    };
    const StableListSelection selection = shown ? CaptureListSelection(search_results_list_, key_at) : StableListSelection();
    if (!tab->is_compare && tab->max_results > 0 && tab->results.size() > tab->max_results)
    {
        search::SortResults(&tab->results, 0, true);
        tab->results.resize(static_cast<size_t>(tab->max_results));
    }
    if (tab->is_compare)
    {
        search::compare::SortRows(&tab->compare_rows, tab->sort_column, tab->sort_ascending);
    }
    else if (tab->sort_column < 0)
    {
        search::SortResults(&tab->results, 0, true);
    }
    else if (tab->sort_column == 3 && std::any_of(tab->results.begin(), tab->results.end(), [](const search::Result& result) { return result.data_state == search::DataState::kNotLoaded; }))
    {
        QueueSearchSort(tab);
        return;
    }
    else
    {
        search::SortResults(&tab->results, tab->sort_column, tab->sort_ascending);
    }
    if (shown)
    {
        RestoreListSelection(search_results_list_, selection, key_at);
        RedrawWindow(search_results_list_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
    }
}

void MainWindow::Impl::SortSearchResults(int column, bool toggle)
{
    if (!search_results_list_ || column < 0)
    {
        return;
    }
    int sel = TabCtrl_GetCurSel(tab_);
    int index = SearchIndexFromTab(sel);
    if (index < 0 || static_cast<size_t>(index) >= search_tabs_.size())
    {
        return;
    }
    EnsureSearchTabResultsLoaded(index);
    auto& tab = search_tabs_[static_cast<size_t>(index)];
    appearance::UpdateListSortState(column, toggle, &tab.sort_column, &tab.sort_ascending);
    SortSearchTabResults(&tab);
    appearance::UpdateListViewSort(search_results_list_, tab.sort_column, tab.sort_ascending);
}

void MainWindow::Impl::ClearHistoryItems(bool delete_cache)
{
    if (!history_list_)
    {
        return;
    }
    change_history_.entries().clear();
    ListView_DeleteAllItems(history_list_);

    if (delete_cache)
    {
        std::wstring path = HistoryCachePath();
        if (!path.empty())
        {
            DeleteFileW(path.c_str());
        }
    }
}

void MainWindow::Impl::RemoveSelectedHistoryItems()
{
    if (!history_list_)
    {
        return;
    }
    auto& entries = change_history_.entries();
    std::vector<int> selected;
    for (int index = ListView_GetNextItem(history_list_, -1, LVNI_SELECTED); index >= 0;
         index = ListView_GetNextItem(history_list_, index, LVNI_SELECTED))
    {
        if (static_cast<size_t>(index) < entries.size())
        {
            selected.push_back(index);
        }
    }
    if (selected.empty())
    {
        return;
    }
    for (auto it = selected.rbegin(); it != selected.rend(); ++it)
    {
        entries.erase(entries.begin() + *it);
    }
    ListView_SetItemState(history_list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    RebuildHistoryList();
    if (HistoryStaysInMemory())
    {
        return;
    }
    changes::WriteHistoryFile(HistoryCachePath(), entries);
}

void MainWindow::Impl::RebuildHistoryList()
{
    if (!history_list_)
    {
        return;
    }
    const int count = static_cast<int>(change_history_.entries().size());
    ListView_SetItemCountEx(history_list_, count, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    RedrawWindow(history_list_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
}

void MainWindow::Impl::RefreshHistory()
{
    // other instances append to the same cache
    const std::wstring path = HistoryCachePath();
    std::wstring content;
    if (history_loaded_ && !history_cache_failed_ && !HistoryStaysInMemory() && !path.empty() &&
        (util::ReadTextFile(path, &content) || util::IsMissing(path)))
    {
        change_history_.Replace(std::move(changes::ParseHistory(content).entries), static_cast<size_t>(history_max_rows_));
        change_history_.Sort(history_sort_column_, history_sort_ascending_);
        ListView_SetItemState(history_list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    }
    RebuildHistoryList();
}

void MainWindow::Impl::ResetNavigationState()
{
    browse_.ResetNavigation();
    UpdateNavigationButtons();
}

void MainWindow::Impl::UpdateTabText(const std::wstring& text)
{
    if (!tab_)
    {
        return;
    }
    int index = TabCtrl_GetCurSel(tab_);
    if (IsSearchTabIndex(index) || IsRegFileTabIndex(index))
    {
        index = FindFirstRegistryTabIndex();
    }
    if (index < 0)
    {
        return;
    }
    TCITEMW item = {};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(text.c_str());
    TabCtrl_SetItem(tab_, index, &item);
    UpdateTabWidth();
    InvalidateRect(tab_, nullptr, FALSE);
}

// session_ stays on the last registry tab while a search tab is shown
void MainWindow::Impl::MarkOfflineDirty()
{
    MarkSessionDirty(*session_);
}

// .reg tab or hive a change went to, which may not be the tab shown when it lands
void MainWindow::Impl::MarkSessionDirty(RegistrySession& session)
{
    for (TabEntry& entry : tabs_)
    {
        if (entry.kind == TabEntry::Kind::kRegFile && entry.session.get() == &session)
        {
            entry.reg_file_dirty = true;
            return;
        }
    }
    if (session.mode == RegistryMode::kOffline)
    {
        session.offline_dirty = true;
    }
}

bool MainWindow::Impl::ConfirmCloseTab(int tab_index)
{
    if (!tab_ || tab_index < 0 || static_cast<size_t>(tab_index) >= tabs_.size())
    {
        return false;
    }
    TabEntry& entry = tabs_[static_cast<size_t>(tab_index)];
    if (entry.kind == TabEntry::Kind::kRegFile && entry.reg_file_dirty)
    {
        std::wstring message = util::Tr(L"The registry file has unsaved changes.\nSave "
                                        L"before closing the tab?");
        int result =
            ui::PromptChoice(hwnd_, message, util::Tr(L"Unsaved Changes"), util::Tr(L"Save"), util::Tr(L"Don't Save"), util::Tr(L"Cancel"), {70, 100, 70});
        if (result == IDCANCEL)
        {
            return false;
        }
        if (result == IDNO)
        {
            return true;
        }
        if (SaveRegFileTab(tab_index))
        {
            entry.reg_file_dirty = false;
            return true;
        }
        return false;
    }
    if (entry.kind != TabEntry::Kind::kRegistry || !entry.session)
    {
        return true;
    }
    if (!ConfirmOfflineChanges(*entry.session, util::Tr(L"The offline registry has unsaved changes.\n"
                                                        L"Save before closing the tab?")))
    {
        return false;
    }
    return true;
}

bool MainWindow::Impl::ConfirmOfflineChanges(RegistrySession& session, const wchar_t* message)
{
    if (session.mode != RegistryMode::kOffline || !session.offline_dirty)
    {
        return true;
    }
    const int result = ui::PromptChoice(hwnd_, message, util::Tr(L"Unsaved Changes"), util::Tr(L"Save"), util::Tr(L"Don't Save"), util::Tr(L"Cancel"), {70, 100, 70});
    return result == IDNO || (result == IDYES && SaveOfflineRegistry(session));
}
void MainWindow::Impl::CloseTab(int tab_index)
{
    if (!tab_)
    {
        return;
    }
    int count = TabCtrl_GetItemCount(tab_);
    if (count <= 1 || tab_index < 0 || tab_index >= count)
    {
        return;
    }
    if (IsSearchTabIndex(tab_index))
    {
        CloseSearchTab(tab_index);
        return;
    }
    const int registry_tab_count = static_cast<int>(std::count_if(
        tabs_.begin(),
        tabs_.end(),
        [](const TabEntry& entry) { return entry.kind == TabEntry::Kind::kRegistry; }
    ));
    if (registry_tab_count <= 1 && static_cast<size_t>(tab_index) < tabs_.size() &&
        tabs_[static_cast<size_t>(tab_index)].kind == TabEntry::Kind::kRegistry)
    {
        return;
    }
    const auto& closing = tabs_[static_cast<size_t>(tab_index)].session;
    // the closed tab's session takes its hive or .reg roots with it and the save prompt needs the replace's changes committed
    if (replace_result_pending_ && closing && closing != local_session_ && replace_target_.lock() == closing)
    {
        StopReplace();
    }
    if (!ConfirmCloseTab(tab_index))
    {
        return;
    }
    if (closing)
    {
        OfferRemoteServiceRestore(*closing);
    }

    if (IsRegFileTabIndex(tab_index))
    {
        TabEntry& entry = tabs_[static_cast<size_t>(tab_index)];
        if (entry.reg_file_loading && !entry.reg_file_session_key.empty())
        {
            auto it = reg_file_parse_sessions_.find(entry.reg_file_session_key);
            if (it != reg_file_parse_sessions_.end() && it->second)
            {
                it->second->work.CancelAndJoin();
                reg_file_parse_sessions_.erase(it);
            }
        }
    }
    const int previous_index = TabCtrl_GetCurSel(tab_);
    TabEntry closed = std::move(tabs_[static_cast<size_t>(tab_index)]);
    tabs_.erase(tabs_.begin() + tab_index);
    TabCtrl_DeleteItem(tab_, tab_index);

    if (active_search_tab_index_ == tab_index)
    {
        active_search_tab_index_ = -1;
    }
    else if (active_search_tab_index_ > tab_index)
    {
        --active_search_tab_index_;
    }

    SelectTabAfterClose(tab_index, previous_index);
    if (closed.tree && std::none_of(tabs_.begin(), tabs_.end(), [&](const TabEntry& tab) { return tab.tree == closed.tree; }))
    {
        // search tab shown next keeps a tree active, a registry tab's own takes over so paths still resolve
        if (closed.tree == &browse_.tree())
        {
            const auto owner = std::find_if(tabs_.begin(), tabs_.end(), [](const TabEntry& tab) { return tab.tree != nullptr; });
            if (owner != tabs_.end())
            {
                const auto owner_session = owner->session;
                ActivateTabTree(static_cast<int>(owner - tabs_.begin()));
                if (owner_session)
                {
                    session_ = owner_session;
                }
            }
            else
            {
                // no tab has a tree yet, the next one shown takes this one over
                closed.tree->Clear();
            }
        }
        browse_.RemoveTree(closed.tree);
    }
    closed.tree = nullptr;
    // released once no tree shows them, a released root would reach the live registry
    ReleaseRegFileRoots(&closed);
    RefreshRegistryTabLabels();
    ApplyViewVisibility();
    UpdateSearchResultsView();
    UpdateStatus();
}

void MainWindow::Impl::SelectTabAfterClose(int closed_index, int previous_index)
{
    const int count = tab_ ? TabCtrl_GetItemCount(tab_) : 0;
    if (count <= 0)
    {
        return;
    }
    const bool closed_active = previous_index == closed_index;
    const int next = closed_active                   ? std::min(closed_index, count - 1)
                     : previous_index > closed_index ? previous_index - 1
                                                     : previous_index;
    if (closed_active)
    {
        search_results_view_tab_index_ = -1;
    }
    TabCtrl_SetCurSel(tab_, next);
    if (closed_active)
    {
        ApplyTabSelection(next);
    }
}

void MainWindow::Impl::SelectTabIndex(int index)
{
    if (!tab_)
    {
        return;
    }
    const int current = TabCtrl_GetCurSel(tab_);
    if (current != index)
    {
        CaptureRegistryTabState(current, false);
    }
    TabCtrl_SetCurSel(tab_, index);
}

int MainWindow::Impl::AddRegistryTab(RegistryMode mode, const wchar_t* label)
{
    TCITEMW item = {};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(label);
    const int index = TabCtrl_GetItemCount(tab_);
    TabCtrl_InsertItem(tab_, index, &item);
    TabEntry entry;
    entry.kind = TabEntry::Kind::kRegistry;
    entry.registry_mode = mode;
    tabs_.push_back(std::move(entry));
    UpdateTabWidth();
    suppress_tab_change_ = true;
    SelectTabIndex(index);
    suppress_tab_change_ = false;
    ActivateTabTree(index);
    return index;
}

void MainWindow::Impl::OpenLocalRegistryTab(REGSAM view)
{
    if (!tab_)
    {
        return;
    }
    const int index = AddRegistryTab(RegistryMode::kLocal, util::Tr(L"Local Registry"));
    tabs_[static_cast<size_t>(index)].registry_view = view;
    RefreshRegistryTabLabels();
    if (view)
    {
        ShowSession(LocalViewSession(view));
    }
    else
    {
        SwitchToLocalRegistry();
    }
    RestoreRegistryTabState(index);
    ApplyViewVisibility();
    UpdateSearchResultsView();
    UpdateStatus();
}

int MainWindow::Impl::CurrentRegistryTabIndex() const
{
    if (!tab_)
    {
        return -1;
    }
    int index = TabCtrl_GetCurSel(tab_);
    if (index < 0)
    {
        return -1;
    }
    if (!IsSearchTabIndex(index) && !IsRegFileTabIndex(index))
    {
        return index;
    }
    return FindFirstRegistryTabIndex();
}

void MainWindow::Impl::UpdateRegistryTabEntry(RegistryMode mode, const std::wstring& offline_path, const std::wstring& remote_machine)
{
    int index = CurrentRegistryTabIndex();
    if (index < 0 || static_cast<size_t>(index) >= tabs_.size())
    {
        return;
    }
    TabEntry& entry = tabs_[static_cast<size_t>(index)];
    if (entry.kind != TabEntry::Kind::kRegistry)
    {
        return;
    }
    entry.registry_mode = mode;
    entry.offline_path = offline_path;
    entry.remote_machine = remote_machine;
}

void MainWindow::Impl::UpdateTabWidth()
{
    if (!tab_)
    {
        return;
    }
    int count = TabCtrl_GetItemCount(tab_);
    if (count <= 0)
    {
        return;
    }
    tab_height_ = tab_strip_.Refit(kTabMinWidth);
    if (hwnd_)
    {
        RECT rect = {};
        GetClientRect(hwnd_, &rect);
        if (rect.right > 0 && rect.bottom > 0)
        {
            LayoutControls(rect.right, rect.bottom);
        }
    }
}

void MainWindow::Impl::BuildAccelerators()
{
    if (accelerators_)
    {
        DestroyAcceleratorTable(accelerators_);
        accelerators_ = nullptr;
    }
    std::vector<ACCEL> accels;
    accels.reserve(_countof(frame::kShortcutBindings) + 9);
    for (const auto& binding : frame::kShortcutBindings)
    {
        accels.push_back(
            {static_cast<BYTE>(FVIRTKEY | binding.modifiers), binding.key, static_cast<WORD>(binding.command)}
        );
    }
    for (int i = 0; i < 9; ++i)
    {
        accels.push_back({FVIRTKEY | FCONTROL, static_cast<WORD>('1' + i), static_cast<WORD>(cmd::kTabSelectBase + i)});
    }
    accelerators_ = CreateAcceleratorTableW(accels.data(), static_cast<int>(accels.size()));
}

void MainWindow::Impl::ActivateTabIndex(int index)
{
    if (!tab_ || index < 0 || index >= TabCtrl_GetItemCount(tab_) || index == TabCtrl_GetCurSel(tab_))
    {
        return;
    }
    SelectTabIndex(index);
    ApplyTabSelection(index);
    ApplyViewVisibility();
    UpdateSearchResultsView();
    UpdateStatus();
}

bool MainWindow::Impl::HandleTabCommand(int command_id)
{
    if (!tab_)
    {
        return true;
    }
    if (command_id >= cmd::kTabSelectBase && command_id <= cmd::kTabSelectMax)
    {
        ActivateTabIndex(command_id - cmd::kTabSelectBase);
        return true;
    }
    const int count = TabCtrl_GetItemCount(tab_);
    switch (command_id)
    {
    case cmd::kTabClose:
        CloseTab(TabCtrl_GetCurSel(tab_));
        return true;
    case cmd::kTabNext:
    case cmd::kTabPrevious:
        if (count > 1)
        {
            const int step = command_id == cmd::kTabNext ? 1 : count - 1;
            ActivateTabIndex((TabCtrl_GetCurSel(tab_) + step) % count);
        }
        return true;
    default:
        return false;
    }
}

bool MainWindow::Impl::SelectAllInFocusedList()
{
    HWND focus = GetFocus();
    if (!focus)
    {
        return false;
    }
    if (focus != browse_.values().hwnd() && focus != history_list_ && focus != search_results_list_)
    {
        return false;
    }
    int count = ListView_GetItemCount(focus);
    if (count <= 0)
    {
        return true;
    }
    const bool value_list = focus == browse_.values().hwnd();
    const bool was_updating = updating_value_list_;
    if (value_list)
    {
        updating_value_list_ = true;
    }
    ListView_SetItemState(focus, -1, LVIS_SELECTED, LVIS_SELECTED);
    ListView_SetItemState(focus, 0, LVIS_FOCUSED, LVIS_FOCUSED);
    ListView_EnsureVisible(focus, 0, FALSE);
    if (value_list)
    {
        updating_value_list_ = was_updating;
        if (!was_updating)
        {
            UpdateStatus();
        }
    }
    return true;
}

bool MainWindow::Impl::InvertSelectionInFocusedList()
{
    HWND focus = GetFocus();
    if (!focus)
    {
        return false;
    }
    if (focus != browse_.values().hwnd() && focus != history_list_ && focus != search_results_list_)
    {
        return false;
    }
    int count = ListView_GetItemCount(focus);
    if (count <= 0)
    {
        return true;
    }
    const bool value_list = focus == browse_.values().hwnd();
    const bool was_updating = updating_value_list_;
    if (value_list)
    {
        updating_value_list_ = true;
    }
    SendMessageW(focus, WM_SETREDRAW, FALSE, 0);
    int first_selected = -1;
    for (int i = 0; i < count; ++i)
    {
        UINT state = ListView_GetItemState(focus, i, LVIS_SELECTED);
        if (state & LVIS_SELECTED)
        {
            ListView_SetItemState(focus, i, 0, LVIS_SELECTED);
        }
        else
        {
            ListView_SetItemState(focus, i, LVIS_SELECTED, LVIS_SELECTED);
            if (first_selected < 0)
            {
                first_selected = i;
            }
        }
    }
    if (first_selected < 0)
    {
        first_selected = 0;
    }
    ListView_SetItemState(focus, first_selected, LVIS_FOCUSED, LVIS_FOCUSED);
    ListView_EnsureVisible(focus, first_selected, FALSE);
    SendMessageW(focus, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(focus, nullptr, TRUE);
    if (value_list)
    {
        updating_value_list_ = was_updating;
        if (!was_updating)
        {
            UpdateStatus();
        }
    }
    return true;
}

} // namespace regkit
