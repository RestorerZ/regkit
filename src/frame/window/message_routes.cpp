// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"

#include "regfile/registry_transfer.h"
#include "win32/text_transform.h"
#include "win32/translation.h"
#include "trace/trace_paths.h"

namespace regkit
{

using namespace window_detail;

namespace
{

bool IsLocalFixedPath(const std::wstring& path)
{
    if (path.size() < 3 || path[1] != L':' || !iswalpha(path[0]) || (path[2] != L'\\' && path[2] != L'/'))
    {
        return false;
    }
    const std::wstring root = path.substr(0, 3);
    const UINT drive_type = GetDriveTypeW(root.c_str());
    return drive_type == DRIVE_FIXED || drive_type == DRIVE_RAMDISK;
}

bool AcceptHandoffFile(HWND owner, const std::wstring& path)
{
    if (!util::IsProcessPrivileged() || !util::IsUacEnabled())
    {
        return true;
    }
    bool local = IsLocalFixedPath(path);
    DWORD attributes = FILE_ATTRIBUTE_DIRECTORY;
    for (std::wstring component = path; local && component.size() > 3; component.resize(component.find_last_of(L"\\/")))
    {
        const DWORD component_attributes = GetFileAttributesW(component.c_str());
        local = component_attributes != INVALID_FILE_ATTRIBUTES &&
                (component_attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        attributes = component.size() == path.size() ? component_attributes : attributes;
    }
    if (!local || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        ui::ShowWarning(
            owner,
            util::Tr(L"RegKit is running with elevated rights and only opens local files handed to it by another instance.")
        );
        return false;
    }
    return ui::PromptKeyChoice(owner, util::Tr(L"Another RegKit instance asked this elevated window to open a .reg file.\n\nOpen it?"), path, util::Tr(L"Open .reg File"), util::Tr(L"Open"), L"", util::Tr(L"Cancel")) == IDYES;
}

bool IsSiblingRegKitWindow(HWND sender)
{
    DWORD sender_pid = 0;
    if (!sender || !IsWindow(sender) || !GetWindowThreadProcessId(sender, &sender_pid) || sender_pid == 0)
    {
        return false;
    }
    const std::wstring sender_image = util::GetProcessImagePath(sender_pid);
    const std::wstring own_image = util::GetModulePath();
    return !sender_image.empty() && !own_image.empty() && util::EqualsInsensitive(sender_image, own_image);
}

} // namespace

LRESULT MainWindow::Impl::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    std::optional<LRESULT> result;
    switch (frame::ClassifyMessage(message))
    {
    case frame::MessageArea::kLifecycle:
        result = HandleLifecycleMessage(message, wparam, lparam);
        break;
    case frame::MessageArea::kLayoutInput:
        result = HandleLayoutInputMessage(message, wparam, lparam);
        break;
    case frame::MessageArea::kWorker:
        result = HandleWorkerMessage(message, wparam, lparam);
        break;
    case frame::MessageArea::kExternal:
        result = HandleExternalMessage(message, wparam, lparam);
        break;
    case frame::MessageArea::kAppearance:
        result = HandleAppearanceMessage(message, wparam, lparam);
        break;
    case frame::MessageArea::kBrowse:
        result = HandleBrowseMessage(message, wparam, lparam);
        break;
    case frame::MessageArea::kUnknown:
        break;
    }
    return result ? *result : DefWindowProcW(hwnd_, message, wparam, lparam);
}

std::optional<LRESULT> MainWindow::Impl::HandleLifecycleMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_CREATE:
        return OnCreate() ? 0 : -1;
    case WM_DESTROY:
        OnDestroy();
        PostQuitMessage(0);
        return 0;
    case WM_NCPAINT:
        {
            LRESULT result = DefWindowProcW(hwnd_, message, wparam, lparam);
            PaintMenuBarSeparator();
            return result;
        }
    case WM_NCACTIVATE:
        {
            LRESULT result = DefWindowProcW(hwnd_, message, wparam, lparam);
            PaintMenuBarSeparator();
            return result;
        }
    case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            if (info)
            {
                info->ptMinTrackSize.x = std::max<LONG>(info->ptMinTrackSize.x, 400);
                info->ptMinTrackSize.y = std::max<LONG>(info->ptMinTrackSize.y, 200);
            }
            return 0;
        }
    case WM_SIZE:
        OnSize(LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_DPICHANGED:
        {
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested)
            {
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            UpdateUIFont();
            ReloadThemeIcons();
            return 0;
        }
    case WM_DPICHANGED_AFTERPARENT:
        UpdateUIFont();
        ReloadThemeIcons();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_ACTIVE && browse_.tree().hwnd())
        {
            if (last_focus_)
            {
                break;
            }
            int sel = TabCtrl_GetCurSel(tab_);
            if (sel >= 0 && static_cast<size_t>(sel) < tabs_.size() &&
                tabs_[static_cast<size_t>(sel)].kind == TabEntry::Kind::kRegistry)
            {
                SetFocus(browse_.tree().hwnd());
            }
        }
        break;
    case WM_CLOSE:
        {
            for (int index = static_cast<int>(tabs_.size()) - 1; index >= 0; --index)
            {
                if (!ConfirmCloseTab(index))
                {
                    restart_on_close_ = false;
                    reset_settings_on_close_ = false;
                    cache_clear_on_close_.reset();
                    return 0;
                }
            }
            if (reset_settings_on_close_)
            {
                if (!RestartAfterSettingsReset())
                {
                    reset_settings_on_close_ = false;
                    return 0;
                }
            }
            else if (cache_clear_on_close_)
            {
                if (!RestartAfterCacheClear(*cache_clear_on_close_))
                {
                    cache_clear_on_close_.reset();
                    return 0;
                }
            }
            else if (restart_on_close_)
            {
                if (!RestartCurrentInstance())
                {
                    restart_on_close_ = false;
                    return 0;
                }
            }
            else
            {
                SaveSettings();
            }
            DestroyWindow(hwnd_);
            return 0;
        }
    default:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<LRESULT> MainWindow::Impl::HandleLayoutInputMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)wparam;
    switch (message)
    {
    case WM_LBUTTONDOWN:
        {
            const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const int width = content_rect_.right - content_rect_.left;
            const int height = content_rect_.bottom - content_rect_.top;
            if (tree_splitter_.Hit(pt))
            {
                tree_splitter_.Begin(hwnd_, pt, settings_.tree_width, kMinTreeWidth, width - kMinValueListWidth - kSplitterWidth);
                return 0;
            }
            if (history_splitter_.Hit(pt))
            {
                history_splitter_.Begin(hwnd_, pt, settings_.history_height, kMinHistoryHeight, height - kHistoryMaxPadding);
                return 0;
            }
            break;
        }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (tree_splitter_.End(hwnd_) || history_splitter_.End(hwnd_))
        {
            RECT rect = {};
            GetClientRect(hwnd_, &rect);
            LayoutControls(rect.right, rect.bottom);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        {
            const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (tree_splitter_.dragging())
            {
                DragSplitter(&tree_splitter_, &settings_.tree_width, pt);
                return 0;
            }
            if (history_splitter_.dragging())
            {
                DragSplitter(&history_splitter_, &settings_.history_height, pt);
                return 0;
            }
            break;
        }
    case WM_SETCURSOR:
        {
            POINT pt = {};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            for (const ui::Splitter* splitter : {&tree_splitter_, &history_splitter_})
            {
                if (splitter->dragging() || splitter->Hit(pt))
                {
                    SetCursor(splitter->Cursor());
                    return TRUE;
                }
            }
            break;
        }
    default:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<LRESULT> MainWindow::Impl::HandleWorkerMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case frame::message_id::kSearchResults:
    case frame::message_id::kSearchPreviewRequest:
    case frame::message_id::kSearchPreviewReady:
    case frame::message_id::kSearchSortReady:
    case frame::message_id::kSearchTabLoadReady:
    case frame::message_id::kSearchProgress:
    case frame::message_id::kSearchFailed:
    case frame::message_id::kReplaceReady:
        return HandleSearchWorkerMessage(message, wparam, lparam);
    case frame::message_id::kLoadTraces:
    case frame::message_id::kLoadDefaults:
    case frame::message_id::kTraceLoadReady:
    case frame::message_id::kDefaultLoadReady:
    case frame::message_id::kDeferredStartup:
    case frame::message_id::kStartupCacheReady:
    case frame::message_id::kUpdateCheckReady:
        return HandleLoadWorkerMessage(message, wparam, lparam);
    case frame::message_id::kRegFileLoadReady:
        return HandleRegFileWorkerMessage(message, wparam, lparam);
    case frame::message_id::kTraceParseBatch:
        return HandleTraceWorkerMessage(message, wparam, lparam);
    case frame::message_id::kDefaultParseBatch:
        return HandleDefaultWorkerMessage(message, wparam, lparam);
    case frame::message_id::kValueListReady:
    case frame::message_id::kValuePreviewReady:
    case frame::message_id::kValuePreviewRequest:
        return HandleValueWorkerMessage(message, wparam, lparam);
    default:
        return std::nullopt;
    }
}

MainWindow::Impl::SearchTab* MainWindow::Impl::SearchTabByGeneration(uint64_t generation)
{
    for (auto& tab : search_tabs_)
    {
        if (tab.run && tab.generation == generation)
        {
            return &tab;
        }
    }
    return nullptr;
}

MainWindow::Impl::SearchTab* MainWindow::Impl::ShownSearchTab()
{
    const int index = tab_ ? SearchIndexFromTab(TabCtrl_GetCurSel(tab_)) : -1;
    return index >= 0 && static_cast<size_t>(index) < search_tabs_.size() ? &search_tabs_[static_cast<size_t>(index)] : nullptr;
}

void MainWindow::Impl::FinishSearch(SearchTab* tab)
{
    tab->run->session.Join();
    tab->duration_ms = std::max<uint64_t>(GetTickCount64() - tab->run->start_tick, 1);
    tab->run.reset();
    if (tab->sort_dirty)
    {
        SortSearchTabResults(tab);
    }
    if (ShownSearchTab() == tab)
    {
        search_last_refresh_tick_ = GetTickCount64();
        UpdateSearchResultsView();
    }
    ApplyViewVisibility();
    UpdateStatus();
}

std::optional<LRESULT> MainWindow::Impl::HandleSearchWorkerMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case frame::message_id::kSearchResults:
        {
            SearchTab* tab = SearchTabByGeneration(static_cast<uint64_t>(wparam));
            if (!tab)
            {
                return 0;
            }
            SearchRun& run = *tab->run;
            run.posted.store(false);
            const uint64_t start_tick = GetTickCount64();
            bool appended = false;
            bool more_pending = false;
            bool producer_done = false;
            for (;;)
            {
                std::vector<search::Result> rows;
                {
                    std::lock_guard<std::mutex> lock(run.mutex);
                    producer_done = run.producer_done;
                    if (run.batches.empty())
                    {
                        break;
                    }
                    rows = std::move(run.batches.front());
                    run.batches.pop_front();
                    run.pending_rows -= rows.size();
                    more_pending = !run.batches.empty();
                }
                run.space.notify_all();
                for (auto& row : rows)
                {
                    row.row_id = tab->next_row_id++;
                }
                tab->results.insert(tab->results.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
                appended = true;
                if (GetTickCount64() - start_tick >= kSearchResultsMaxMs)
                {
                    break;
                }
            }
            tab->sort_dirty = tab->sort_dirty || appended;
            if (more_pending && !run.posted.exchange(true) && !PostMessageW(hwnd_, frame::message_id::kSearchResults, wparam, 0))
            {
                run.posted.store(false);
            }
            if (appended && ShownSearchTab() == tab)
            {
                const uint64_t now = GetTickCount64();
                if (now - search_last_refresh_tick_ >= kSearchResultsRefreshMs)
                {
                    search_last_refresh_tick_ = now;
                    UpdateSearchResultsView();
                    UpdateStatus();
                }
            }
            if (!more_pending && producer_done)
            {
                FinishSearch(tab);
            }
            return 0;
        }
    case frame::message_id::kSearchPreviewRequest:
        {
            search_preview_request_posted_ = false;
            if (!search_results_list_)
            {
                return 0;
            }
            const int top = ListView_GetTopIndex(search_results_list_);
            const int page = ListView_GetCountPerPage(search_results_list_);
            const int count = ListView_GetItemCount(search_results_list_);
            QueueSearchPreviews(std::max(0, top), std::min(count - 1, top + std::max(page, 1)));
            return 0;
        }
    case frame::message_id::kSearchPreviewReady:
        {
            auto owned = work::TakePayload<SearchPreviewPayload>(lparam);
            if (!owned)
            {
                return 0;
            }
            if (owned->tab_index < 0 || static_cast<size_t>(owned->tab_index) >= search_tabs_.size())
            {
                return 0;
            }
            SearchTab& tab = search_tabs_[static_cast<size_t>(owned->tab_index)];
            if (tab.generation != owned->generation)
            {
                return 0;
            }
            int first = -1;
            int last = -1;
            for (auto& item : owned->items)
            {
                int index = -1;
                if (item.index >= 0 && static_cast<size_t>(item.index) < tab.results.size() &&
                    tab.results[static_cast<size_t>(item.index)].row_id == item.row_id)
                {
                    index = item.index;
                }
                else
                {
                    for (size_t i = 0; i < tab.results.size(); ++i)
                    {
                        if (tab.results[i].row_id == item.row_id)
                        {
                            index = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (index < 0)
                {
                    continue;
                }
                search::Result& row = tab.results[static_cast<size_t>(index)];
                row.data_text = std::move(item.preview);
                row.type = item.type;
                row.data_size = item.data_size;
                row.data_state = search::DataState::kLoaded;
                first = first < 0 ? index : std::min(first, index);
                last = std::max(last, index);
            }
            if (first >= 0 && search_results_list_ && SearchIndexFromTab(TabCtrl_GetCurSel(tab_)) == owned->tab_index)
            {
                ListView_RedrawItems(search_results_list_, first, last);
            }
            return 0;
        }
    case frame::message_id::kSearchSortReady:
        {
            auto owned = work::TakePayload<SearchSortPayload>(lparam);
            if (!owned)
            {
                return 0;
            }
            if (owned->tab_index < 0 || static_cast<size_t>(owned->tab_index) >= search_tabs_.size())
            {
                return 0;
            }
            SearchTab& tab = search_tabs_[static_cast<size_t>(owned->tab_index)];
            if (tab.generation != owned->generation || tab.results.size() != owned->rows.size())
            {
                return 0;
            }
            tab.results = std::move(owned->rows);
            if (SearchIndexFromTab(TabCtrl_GetCurSel(tab_)) == owned->tab_index)
            {
                UpdateSearchResultsView();
            }
            return 0;
        }
    case frame::message_id::kSearchTabLoadReady:
        ApplySearchTabLoad(work::TakePayload<SearchTabLoadPayload>(lparam));
        return 0;
    case frame::message_id::kSearchProgress:
        if (SearchTab* tab = SearchTabByGeneration(static_cast<uint64_t>(wparam)))
        {
            tab->run->progress_posted.store(false);
            if (ShownSearchTab() == tab)
            {
                UpdateStatus();
            }
        }
        return 0;
    case frame::message_id::kSearchFailed:
        if (SearchTab* tab = SearchTabByGeneration(static_cast<uint64_t>(wparam)))
        {
            CancelSearch(tab);
            const std::wstring detail = search::regex::StatusText(static_cast<search::regex::Status>(lparam));
            ui::ShowError(hwnd_, detail.empty() ? std::wstring(util::Tr(L"The find text isn't a valid regular expression.")) : detail);
        }
        return 0;
    case frame::message_id::kReplaceReady:
        ApplyReplacePayload(work::TakePayload<ReplacePayload>(lparam));
        return 0;
    default:
        return std::nullopt;
    }
}

std::optional<LRESULT> MainWindow::Impl::HandleLoadWorkerMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)wparam;
    switch (message)
    {
    case frame::message_id::kLoadTraces:
        StartTraceLoadWorker();
        return 0;
    case frame::message_id::kLoadDefaults:
        StartDefaultLoadWorker();
        return 0;
    case frame::message_id::kTraceLoadReady:
        {
            auto owned = work::TakePayload<TraceLoadPayload>(lparam);
            if (!owned)
            {
                return 0;
            }
            if (!trace_load_session_.IsCurrent(owned->generation))
            {
                return 0;
            }
            trace_load_session_.Join();
            active_traces_ = std::move(owned->traces);
            trace_selection_cache_ = std::move(owned->selection_cache);
            RefreshTreeSelection();
            UpdateValueListForNode(browse_.current_node());
            return 0;
        }
    case frame::message_id::kDefaultLoadReady:
        {
            auto owned = work::TakePayload<DefaultLoadPayload>(lparam);
            if (!owned)
            {
                return 0;
            }
            if (!default_load_session_.IsCurrent(owned->generation))
            {
                return 0;
            }
            default_load_session_.Join();
            active_defaults_ = std::move(owned->defaults);
            UpdateValueListForNode(browse_.current_node());
            return 0;
        }
    case frame::message_id::kDeferredStartup:
        RunDeferredStartup();
        return 0;
    case frame::message_id::kStartupCacheReady:
        ApplyStartupCachePayload(work::TakePayload<StartupCachePayload>(lparam));
        return 0;
    case frame::message_id::kUpdateCheckReady:
        {
            updates_.Apply(work::TakePayload<frame::UpdateCheckPayload>(lparam).get());
            return 0;
        }
    default:
        return std::nullopt;
    }
}

std::optional<LRESULT> MainWindow::Impl::HandleRegFileWorkerMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)wparam;
    switch (message)
    {
    case frame::message_id::kRegFileLoadReady:
        {
            auto owned = work::TakePayload<RegFileParsePayload>(lparam);
            if (!owned)
            {
                return 0;
            }
            auto session_it = reg_file_parse_sessions_.find(owned->source_lower);
            if (session_it == reg_file_parse_sessions_.end())
            {
                return 0;
            }
            if (!session_it->second || !session_it->second->work.IsCurrent(owned->generation))
            {
                return 0;
            }
            session_it->second->work.Join();
            reg_file_parse_sessions_.erase(session_it);
            if (owned->cancelled)
            {
                return 0;
            }

            int tab_index = -1;
            for (size_t i = 0; i < tabs_.size(); ++i)
            {
                const TabEntry& entry = tabs_[i];
                if (entry.kind != TabEntry::Kind::kRegFile)
                {
                    continue;
                }
                if (entry.reg_file_session_key == owned->source_lower)
                {
                    tab_index = static_cast<int>(i);
                    break;
                }
            }
            if (tab_index < 0 || static_cast<size_t>(tab_index) >= tabs_.size())
            {
                return 0;
            }
            TabEntry& entry = tabs_[static_cast<size_t>(tab_index)];
            entry.reg_file_loading = false;
            if (!owned->error.empty())
            {
                ui::ShowError(hwnd_, owned->error.c_str());
                UpdateStatus();
                return 0;
            }
            if (entry.reg_file_dirty)
            {
                UpdateStatus();
                return 0;
            }

            ReleaseRegFileRoots(&entry);
            std::vector<TabEntry::RegFileRoot> roots;
            roots.reserve(owned->roots.size());
            for (auto& parsed : owned->roots)
            {
                if (!parsed.data)
                {
                    continue;
                }
                TabEntry::RegFileRoot root;
                root.name = parsed.name;
                root.data = parsed.data;
                root.root = RegistryStore::RegisterVirtualRoot(root.name, root.data);
                if (root.root)
                {
                    roots.push_back(std::move(root));
                }
            }
            entry.reg_file_roots = std::move(roots);
            entry.reg_file_dirty = false;
            if (tab_ && TabCtrl_GetCurSel(tab_) == tab_index)
            {
                SyncRegFileTabSelection();
                ApplyViewVisibility();
                UpdateStatus();
            }
            return 0;
        }
    default:
        return std::nullopt;
    }
}

std::optional<LRESULT> MainWindow::Impl::HandleTraceWorkerMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)wparam;
    switch (message)
    {
    case frame::message_id::kTraceParseBatch:
        {
            auto owned = work::TakePayload<TraceParseBatch>(lparam);
            if (!owned)
            {
                return 0;
            }
            auto it = trace_parse_sessions_.find(owned->source_lower);
            if (it == trace_parse_sessions_.end())
            {
                return 0;
            }
            TraceParseSession* session = it->second.get();
            if (!session || !session->work.IsCurrent(owned->generation))
            {
                return 0;
            }
            const bool touches_current =
                browse_.current_node() &&
                owned->affected_keys.find(TracePathLowerForNode(*browse_.current_node())) != owned->affected_keys.end();
            if (session->dialog && IsWindow(session->dialog) && !owned->entries.empty())
            {
                auto dialog_entries = std::make_unique<std::vector<KeyValueDialogEntry>>(std::move(owned->entries));
                TraceDialogPostEntries(session->dialog, dialog_entries.release());
            }
            if (session->added_to_active && touches_current && browse_.current_node())
            {
                uint64_t now = GetTickCount64();
                if (owned->done || (now - last_trace_refresh_tick_) >= 100)
                {
                    last_trace_refresh_tick_ = now;
                    UpdateValueListForNode(browse_.current_node());
                }
            }
            if (owned->done)
            {
                session->parsing_done = true;
                if (session->data)
                {
                    std::unique_lock<std::shared_mutex> data_lock(*session->data->mutex);
                    trace::Selection normalized = session->selection;
                    trace::NormalizeSelection(*session->data, &normalized);
                    session->selection = normalized;
                    trace_selection_cache_[session->source_lower] = normalized;
                    if (session->added_to_active)
                    {
                        for (auto& trace : active_traces_)
                        {
                            if (EqualsInsensitive(trace.source_path, session->source_path))
                            {
                                trace.selection = std::make_shared<trace::Selection>(normalized);
                                break;
                            }
                        }
                    }
                }
                if (!owned->error.empty())
                {
                    HWND error_owner = session->dialog && IsWindow(session->dialog) ? session->dialog : hwnd_;
                    ui::ShowError(error_owner, owned->error.c_str());
                    if (session->dialog && IsWindow(session->dialog))
                    {
                        PostMessageW(session->dialog, WM_CLOSE, 0, 0);
                    }
                    if (session->added_to_active)
                    {
                        active_traces_.erase(std::remove_if(active_traces_.begin(), active_traces_.end(), [&](const ActiveTrace& trace) {
                                                 return EqualsInsensitive(trace.source_path, session->source_path);
                                             }),
                                             active_traces_.end());
                        trace_selection_cache_.erase(session->source_lower);
                        SaveActiveTraces();
                        SaveTraceSettings();
                        RefreshTreeSelection();
                        UpdateValueListForNode(browse_.current_node());
                        SaveSettings();
                        session->added_to_active = false;
                    }
                }
                else if (session->dialog && IsWindow(session->dialog))
                {
                    TraceDialogPostDone(session->dialog, true);
                }
                if (session->added_to_active && browse_.current_node())
                {
                    UpdateValueListForNode(browse_.current_node());
                }
                session->work.Join();
                if (!session->dialog || !IsWindow(session->dialog))
                {
                    trace_parse_sessions_.erase(it);
                }
            }
            return 0;
        }
    default:
        return std::nullopt;
    }
}

std::optional<LRESULT> MainWindow::Impl::HandleDefaultWorkerMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)wparam;
    switch (message)
    {
    case frame::message_id::kDefaultParseBatch:
        {
            auto owned = work::TakePayload<DefaultParseBatch>(lparam);
            if (!owned)
            {
                return 0;
            }
            auto it = default_parse_sessions_.find(owned->source_lower);
            if (it == default_parse_sessions_.end())
            {
                return 0;
            }
            DefaultParseSession* session = it->second.get();
            if (!session || !session->work.IsCurrent(owned->generation))
            {
                return 0;
            }
            bool touches_current = false;
            if (browse_.current_node())
            {
                std::wstring path = SourceLookupPath(*browse_.current_node());
                std::wstring normalized = trace::NormalizeKeyPathBasic(path);
                if (normalized.empty())
                {
                    normalized = path;
                }
                touches_current = owned->affected_keys.find(ToLower(normalized)) != owned->affected_keys.end();
            }
            if (session->dialog && IsWindow(session->dialog) && !owned->entries.empty())
            {
                auto dialog_entries = std::make_unique<std::vector<KeyValueDialogEntry>>(std::move(owned->entries));
                TraceDialogPostEntries(session->dialog, dialog_entries.release());
            }
            if (session->added_to_active && touches_current && browse_.current_node())
            {
                uint64_t now = GetTickCount64();
                if (owned->done || (now - last_default_refresh_tick_) >= 100)
                {
                    last_default_refresh_tick_ = now;
                    UpdateValueListForNode(browse_.current_node());
                }
            }
            if (owned->done)
            {
                session->parsing_done = true;
                if (!owned->error.empty())
                {
                    if (session->show_errors)
                    {
                        HWND error_owner = session->dialog && IsWindow(session->dialog) ? session->dialog : hwnd_;
                        ui::ShowError(error_owner, owned->error.c_str());
                    }
                    if (session->dialog && IsWindow(session->dialog))
                    {
                        PostMessageW(session->dialog, WM_CLOSE, 0, 0);
                    }
                    if (session->added_to_active)
                    {
                        active_defaults_.erase(std::remove_if(active_defaults_.begin(), active_defaults_.end(), [&](const ActiveDefault& defaults) {
                                                   return EqualsInsensitive(defaults.source_path, session->source_path);
                                               }),
                                               active_defaults_.end());
                        SaveActiveDefaults();
                        UpdateValueListForNode(browse_.current_node());
                        SaveSettings();
                        session->added_to_active = false;
                    }
                }
                else if (session->dialog && IsWindow(session->dialog))
                {
                    TraceDialogPostDone(session->dialog, true);
                }
                if (session->added_to_active && browse_.current_node())
                {
                    UpdateValueListForNode(browse_.current_node());
                }
                session->work.Join();
                if (!session->dialog || !IsWindow(session->dialog))
                {
                    default_parse_sessions_.erase(it);
                }
            }
            return 0;
        }
    default:
        return std::nullopt;
    }
}

std::optional<LRESULT> MainWindow::Impl::HandleValueWorkerMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)wparam;
    switch (message)
    {
    case frame::message_id::kValuePreviewRequest:
        {
            value_preview_request_posted_ = false;
            HWND list = browse_.values().hwnd();
            if (!list)
            {
                return 0;
            }
            const int top = ListView_GetTopIndex(list);
            const int page = ListView_GetCountPerPage(list);
            const int count = ListView_GetItemCount(list);
            QueueValuePreviews(std::max(0, top), std::min(count - 1, top + std::max(page, 1)));
            return 0;
        }
    case frame::message_id::kValuePreviewReady:
        {
            auto owned = work::TakePayload<ValuePreviewPayload>(lparam);
            if (!owned)
            {
                return 0;
            }
            if (owned->generation != value_list_generation_.load())
            {
                return 0;
            }
            int first = -1;
            int last = -1;
            for (const auto& item : owned->items)
            {
                int index = item.index;
                ListRow* row = browse_.values().MutableRowAt(index);
                if (!row || row->extra != item.name)
                {
                    row = nullptr;
                    const int count = static_cast<int>(browse_.values().RowCount());
                    for (int i = 0; i < count; ++i)
                    {
                        ListRow* candidate = browse_.values().MutableRowAt(i);
                        if (candidate && candidate->kind == rowkind::kValue && candidate->extra == item.name)
                        {
                            row = candidate;
                            index = i;
                            break;
                        }
                    }
                }
                if (!row)
                {
                    continue;
                }
                row->data = item.preview;
                row->value_type = item.type;
                row->value_data_size = item.size;
                if (row->type.empty())
                {
                    row->type = value_format::TypeName(item.type);
                }
                row->size_value = item.size;
                row->has_size = true;
                if (row->size.empty() && item.size > 0)
                {
                    row->size = std::to_wstring(item.size);
                }
                row->data_ready = true;
                browse_.values().InvalidateFilterCache(row);
                first = first < 0 ? index : std::min(first, index);
                last = std::max(last, index);
            }
            browse_.values().RefreshFilter();
            if (first >= 0 && browse_.values().hwnd())
            {
                ListView_RedrawItems(browse_.values().hwnd(), first, last);
            }
            return 0;
        }
    case frame::message_id::kValueListReady:
        {
            auto owned = work::TakePayload<ValueListPayload>(lparam);
            if (!owned)
            {
                return 0;
            }
            if (owned->generation != value_list_generation_.load())
            {
                return 0;
            }
            HWND list_hwnd = browse_.values().hwnd();
            const bool merge = owned->generation == merge_value_list_generation_ && browse_.current_node();
            if (list_hwnd && !merge)
            {
                SendMessageW(list_hwnd, WM_SETREDRAW, FALSE, 0);
            }
            bool merged = false;
            if (merge)
            {
                ApplyValueComments(&owned->rows);
                merged = browse_.values().MergeRows(std::move(owned->rows));
            }
            else
            {
                browse_.values().SetRows(std::move(owned->rows));
            }
            if (!merged)
            {
                RefreshValueListComments();
            }
            current_key_count_ = owned->key_count;
            current_value_count_ = owned->value_count;
            if (!merged && list_hwnd && !jump_ui_batch_active_)
            {
                SendMessageW(list_hwnd, WM_SETREDRAW, TRUE, 0);
                RedrawWindow(list_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
            }
            value_list_loading_ = false;
            UpdateStatus();
            StartPendingValueListRename();
            if (!merged && !retained_value_key_path_.empty() && browse_.current_node() &&
                EqualsInsensitive(registry_path::Build(*browse_.current_node()), retained_value_key_path_) &&
                !SelectValueByName(retained_value_name_))
            {
                SelectListRowAtIndex(browse_.values().hwnd(), retained_value_index_);
            }
            retained_value_name_.clear();
            retained_value_key_path_.clear();
            retained_value_index_ = -1;
            if (!pending_value_selection_key_.empty() && browse_.current_node() &&
                EqualsInsensitive(registry_path::Build(*browse_.current_node()), pending_value_selection_key_))
            {
                RestoreValueSelection();
            }
            if (!pending_value_name_.empty())
            {
                SelectValueByName(pending_value_name_);
                pending_value_name_.clear();
            }
            if (!pending_external_value_name_.empty() && browse_.current_node())
            {
                std::wstring current_path = registry_path::Build(*browse_.current_node());
                if (EqualsInsensitive(current_path, pending_external_value_key_path_))
                {
                    const bool selected = SelectValueByName(pending_external_value_name_);
                    pending_external_value_key_path_.clear();
                    pending_external_value_name_.clear();
                    const int command = pending_value_command_;
                    pending_value_command_ = 0;
                    if (selected && command != 0)
                    {
                        FocusPane(browse_.values().hwnd());
                        PostMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
                    }
                }
            }
            return 0;
        }
    default:
        return std::nullopt;
    }
}

std::optional<LRESULT> MainWindow::Impl::HandleExternalMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_DROPFILES:
        {
            HDROP drop = reinterpret_cast<HDROP>(wparam);
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> reg_paths;
            std::wstring offline_candidate;
            for (UINT index = 0; index < count; ++index)
            {
                UINT path_len = DragQueryFileW(drop, index, nullptr, 0);
                if (path_len == 0)
                {
                    continue;
                }
                std::wstring path(path_len + 1, L'\0');
                if (DragQueryFileW(drop, index, path.data(), static_cast<UINT>(path.size())) == 0)
                {
                    continue;
                }
                path.resize(path_len);
                if (util::HasFileExtension(path, L".reg"))
                {
                    reg_paths.push_back(path);
                }
                else if (offline_candidate.empty())
                {
                    offline_candidate = path;
                }
            }
            DragFinish(drop);
            if (!reg_paths.empty())
            {
                for (const auto& path : reg_paths)
                {
                    OpenRegFileTab(path);
                }
            }
            if (!offline_candidate.empty())
            {
                LoadOfflineRegistryFromPath(offline_candidate, true);
            }
            return 0;
        }
    case frame::message_id::kTreeRedraw:
        FlushTreeRedraw();
        return 0;
    case frame::message_id::kRegistryChanged:
        if (wparam == key_watcher_.generation())
        {
            const ULONGLONG elapsed = GetTickCount64() - auto_refresh_tick_;
            SetTimer(hwnd_, kAutoRefreshTimerId, elapsed >= 750 ? 250 : static_cast<UINT>(1000 - elapsed), nullptr);
        }
        return 0;
    case WM_TIMER:
        if (wparam == kAutoRefreshTimerId)
        {
            ApplyAutoRefresh();
            return 0;
        }
        if (wparam == kStatusMessageTimerId)
        {
            KillTimer(hwnd_, kStatusMessageTimerId);
            status_message_.clear();
            UpdateStatus();
            return 0;
        }
        if (wparam == kCompatJumpTimerId)
        {
            FlushExternalNavigation();
            return 0;
        }
        if (wparam == kTreeStateTimerId)
        {
            KillTimer(hwnd_, kTreeStateTimerId);
            CaptureTreeStateNow();
            return 0;
        }
        break;
    case WM_COPYDATA:
        {
            auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
            if (!data)
            {
                return 0;
            }
            if ((data->dwData != kExternalJumpCopyDataId && data->dwData != kEditRegFileCopyDataId) || !data->lpData ||
                data->cbData < sizeof(wchar_t) || data->cbData > kExternalMessageMaxBytes)
            {
                return 0;
            }
            if (!IsSiblingRegKitWindow(reinterpret_cast<HWND>(wparam)))
            {
                return 0;
            }
            size_t length = data->cbData / sizeof(wchar_t);
            const wchar_t* text = reinterpret_cast<const wchar_t*>(data->lpData);
            auto target = std::make_unique<std::wstring>(text, text + length);
            while (!target->empty() && target->back() == L'\0')
            {
                target->pop_back();
            }
            if (target->empty() || (data->dwData == kEditRegFileCopyDataId && !util::HasFileExtension(*target, L".reg") && !IsHiveFile(*target)))
            {
                return 0;
            }
            if (!PostMessageW(hwnd_, frame::message_id::kExternalHandoff, data->dwData, reinterpret_cast<LPARAM>(target.get())))
            {
                return 0;
            }
            target.release();
            return TRUE;
        }
    case frame::message_id::kExternalHandoff:
        {
            const std::unique_ptr<std::wstring> target(reinterpret_cast<std::wstring*>(lparam));
            ShowWindow(hwnd_, SW_RESTORE);
            SetForegroundWindow(hwnd_);
            if (wparam == kEditRegFileCopyDataId)
            {
                if (AcceptHandoffFile(hwnd_, *target))
                {
                    OpenFile(*target);
                }
                return 0;
            }
            if (deferred_startup_complete_)
            {
                if (!NavigateToExternalJump(*target))
                {
                    ui::ShowWarning(hwnd_, util::TrDetail(L"Registry path not found.", *target));
                }
            }
            else
            {
                QueueExternalJump(*target);
            }
            FocusAddressBarForExternalJump(true);
            return 0;
        }
    case WM_SETFOCUS:
        if (last_focus_ && IsWindow(last_focus_) && IsChild(hwnd_, last_focus_) && IsWindowVisible(last_focus_) &&
            IsWindowEnabled(last_focus_))
        {
            FocusPane(last_focus_);
            return 0;
        }
        break;
    case frame::message_id::kAddressEnter:
        NavigateToAddress();
        return 0;
    case frame::message_id::kFocusAddressBar:
        FocusAddressBarForExternalJump(false);
        return 0;
    default:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<LRESULT> MainWindow::Impl::HandleAppearanceMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_ERASEBKGND:
        {
            return 1;
        }
    case WM_PAINT:
        PaintBuffered(hwnd_);
        return 0;
    case WM_PRINTCLIENT:
        OnPrintClient(reinterpret_cast<HDC>(wparam));
        return 0;
    case WM_SETTINGCHANGE:
        {
            if (applying_theme_ || theme_mode_ != ThemeMode::kSystem)
            {
                return 0;
            }
            if (!Theme::UpdateFromSystem())
            {
                return 0;
            }
            applying_theme_ = true;
            Theme::Current().ApplyToWindow(hwnd_);
            ApplyThemeToChildren();
            ReloadThemeIcons();
            if (hwnd_)
            {
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            applying_theme_ = false;
            return 0;
        }
    case WM_THEMECHANGED:
        if (applying_theme_ || theme_mode_ != ThemeMode::kSystem)
        {
            return 0;
        }
        if (!Theme::UpdateFromSystem())
        {
            return 0;
        }
        applying_theme_ = true;
        Theme::Current().ApplyToWindow(hwnd_);
        ApplyThemeToChildren();
        ReloadThemeIcons();
        if (hwnd_)
        {
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        applying_theme_ = false;
        return 0;
    case WM_CTLCOLORSTATIC:
        {
            HDC hdc = reinterpret_cast<HDC>(wparam);
            HWND target = reinterpret_cast<HWND>(lparam);
            const Theme& theme = Theme::Current();
            COLORREF color = theme.TextColor();
            COLORREF background = theme.PanelColor();
            HBRUSH brush = theme.PanelBrush();
            if (target == history_label_ || target == tree_header_)
            {
                background = theme.HeaderColor();
                brush = theme.HeaderBrush();
            }
            SetTextColor(hdc, color);
            SetBkColor(hdc, background);
            return reinterpret_cast<LRESULT>(brush);
        }
    case WM_INITMENUPOPUP:
        UpdateMenuState(reinterpret_cast<HMENU>(wparam));
        return 0;
    case WM_MENUSELECT:
        {
            HMENU menu = reinterpret_cast<HMENU>(lparam);
            const UINT flags = HIWORD(wparam);
            const UINT position = LOWORD(wparam);
            if ((flags & MF_POPUP) != 0 && menu && GetSubMenu(menu, static_cast<int>(position)) == regedit_favorites_menu_)
            {
                RefreshRegEditFavoritesMenu();
            }
            return 0;
        }
    case WM_CTLCOLOREDIT:
        {
            HDC hdc = reinterpret_cast<HDC>(wparam);
            SetTextColor(hdc, Theme::Current().TextColor());
            SetBkColor(hdc, Theme::Current().FieldColor());
            return reinterpret_cast<LRESULT>(Theme::Current().FieldBrush());
        }
    case WM_DRAWITEM:
        {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (draw && draw->CtlType == ODT_MENU)
            {
                OnDrawMenuItem(draw);
                return TRUE;
            }
            if (draw && draw->CtlType == ODT_BUTTON)
            {
                DrawPanelButton(draw);
                return TRUE;
            }
            if (draw && draw->CtlType == ODT_STATIC && (draw->CtlID == kTreeHeaderId || draw->CtlID == kHistoryLabelId))
            {
                const Theme& theme = Theme::Current();
                HDC hdc = draw->hDC;
                RECT rect = draw->rcItem;
                UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
                FillRect(hdc, &rect, theme.HeaderBrush());

                wchar_t text[128] = {};
                GetWindowTextW(draw->hwndItem, text, static_cast<int>(_countof(text)));
                HFONT old_font = nullptr;
                if (ui_font_)
                {
                    old_font = reinterpret_cast<HFONT>(SelectObject(hdc, ui_font_));
                }
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, theme.TextColor());
                RECT text_rect = rect;
                text_rect.left += kHeaderTextPadding;
                text_rect.right -= kHeaderTextPadding;
                DrawTextW(hdc, text, -1, &text_rect, format);
                if (old_font)
                {
                    SelectObject(hdc, old_font);
                }
                return TRUE;
            }
            break;
        }
    case WM_MEASUREITEM:
        {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (measure && measure->CtlType == ODT_MENU)
            {
                OnMeasureMenuItem(measure);
                return TRUE;
            }
            break;
        }
    default:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<LRESULT> MainWindow::Impl::HandleBrowseMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_COMMAND:
        {
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kTreeHeaderCloseId)
            {
                settings_.show_tree = false;
                ApplyViewVisibility();
                return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kHistoryHeaderCloseId)
            {
                settings_.show_history = false;
                SaveSettings();
                ApplyViewVisibility();
                return 0;
            }
            if (HIWORD(wparam) == 0 && appearance::HandleListViewCommand(hwnd_, LOWORD(wparam)))
            {
                return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kAddressGoId)
            {
                NavigateToAddress();
                return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kFilterClearId)
            {
                ClearValueFilter(true);
                return 0;
            }
            if (HIWORD(wparam) == EN_CHANGE && LOWORD(wparam) == kAddressEditId)
            {
                UpdateGoButtonState();
                return 0;
            }
            if (HIWORD(wparam) == EN_CHANGE && LOWORD(wparam) == kFilterEditId)
            {
                const std::wstring buffer = util::WindowText(browse_.filter());
                bool needs_full_data = !buffer.empty();
                if (needs_full_data)
                {
                    needs_full_data =
                        std::any_of(browse_.values().rows().begin(), browse_.values().rows().end(), [](const ListRow& row) { return row.kind == rowkind::kValue && !row.data_ready; });
                }
                browse_.values().SetFilter(buffer);
                if (needs_full_data && browse_.current_node())
                {
                    UpdateValueListForNode(browse_.current_node());
                }
                UpdateStatus();
                return 0;
            }
            if (HIWORD(wparam) <= 1 && HandleMenuCommand(LOWORD(wparam)))
            {
                return 0;
            }
            return 0;
        }
    case WM_CONTEXTMENU:
        {
            HWND source = reinterpret_cast<HWND>(wparam);
            if (source != browse_.tree().hwnd() && source != browse_.values().hwnd() && source != history_list_ && source != search_results_list_)
            {
                break;
            }
            POINT screen_pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (screen_pt.x == -1 && screen_pt.y == -1)
            {
                RECT rect = {};
                const HTREEITEM selected = source == browse_.tree().hwnd() ? TreeView_GetSelection(source) : nullptr;
                const int row = selected ? -1 : ListView_GetNextItem(source, -1, LVNI_FOCUSED | LVNI_SELECTED);
                if (selected)
                {
                    TreeView_EnsureVisible(source, selected);
                }
                else if (row >= 0)
                {
                    ListView_EnsureVisible(source, row, FALSE);
                }
                if (!(selected ? TreeView_GetItemRect(source, selected, &rect, TRUE) : row >= 0 && ListView_GetItemRect(source, row, &rect, LVIR_LABEL)))
                {
                    GetClientRect(source, &rect);
                    rect.bottom = rect.top + 32;
                }
                screen_pt = {rect.left + 8, (rect.top + rect.bottom) / 2};
                ClientToScreen(source, &screen_pt);
            }
            if (source == browse_.tree().hwnd())
            {
                ShowTreeContextMenu(screen_pt);
                return 0;
            }
            if (source == browse_.values().hwnd())
            {
                ShowValueContextMenu(screen_pt);
                return 0;
            }
            if (source == history_list_)
            {
                ShowHistoryContextMenu(screen_pt);
                return 0;
            }
            ShowSearchResultContextMenu(screen_pt);
            return 0;
        }
    case WM_NOTIFY:
        return HandleNotification(lparam);
    default:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace regkit
