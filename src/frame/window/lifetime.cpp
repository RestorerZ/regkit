// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"

#include "ui/dialog_layout.h"
#include "ui/list_header.h"
#include "win32/translation.h"

namespace regkit
{
using namespace window_detail;

bool MainWindow::Impl::OnCreate()
{
    if (util::IsProcessPrivileged() && util::IsUacEnabled())
    {
        ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    }
    updates_.Attach(hwnd_, [this](const std::wstring& text) { SetStatusMessage(text); });
    win32::SetMissingDesktopPrompt([](HWND owner, const std::wstring& path) {
        return ui::PromptKeyChoice(owner, util::Tr(L"SYSTEM has no Desktop folder, so Windows reports \"Location is not available\" in file dialogs. Create this folder to prevent the error?"), path, util::Tr(L"Create Folder"), util::Tr(L"Create"), L"", util::Tr(L"Cancel")) == IDYES;
    });
    ui_font_ = CreateUIFont();
    icon_font_ = CreateIconFont(10);
    custom_font_ = DefaultLogFont();
    LoadSettings();
    if (theme_mode_ == ThemeMode::kCustom)
    {
        LoadThemePresets();
    }
    ApplySavedWindowPlacement();
    if (theme_mode_ != ThemeMode::kCustom || !ApplyThemePresetByName(settings_.theme_preset, false))
    {
        Theme::SetMode(theme_mode_);
        ApplySystemTheme();
    }
    UpdateUIFont();
    BuildMenus();
    BuildAccelerators();

    toolbar_.Create(hwnd_, instance_, kToolbarId);

    std::vector<TBBUTTON> buttons;
    buttons.push_back({0, cmd::kRegistryLocal, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({1, cmd::kRegistryNetwork, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({2, cmd::kRegistryOffline, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({6, kToolbarSepGroup1, TBSTATE_ENABLED, BTNS_SEP, {0}, 0, 0});
    buttons.push_back({3, cmd::kEditFind, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({4, cmd::kEditReplace, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({6, kToolbarSepGroup2, TBSTATE_ENABLED, BTNS_SEP, {0}, 0, 0});
    buttons.push_back({5, cmd::kEditUndo, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({6, cmd::kEditRedo, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({7, cmd::kEditCopy, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({8, cmd::kEditPaste, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({9, cmd::kEditDelete, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({10, cmd::kViewRefresh, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({6, kToolbarSepGroup3, TBSTATE_ENABLED, BTNS_SEP, {0}, 0, 0});
    buttons.push_back({11, cmd::kNavBack, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({12, cmd::kNavForward, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    buttons.push_back({13, cmd::kNavUp, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0});
    toolbar_.AddButtons(buttons);

    browse::CreateRequest browse_request;
    browse_request.parent = hwnd_;
    browse_request.instance = instance_;
    browse_request.address_id = kAddressEditId;
    browse_request.go_id = kAddressGoId;
    browse_request.filter_id = kFilterEditId;
    browse_request.tree_id = kTreeId;
    browse_request.values_id = kValueListId;
    browse_request.address_proc = AddressEditProc;
    browse_request.address_subclass_id = kAddressSubclassId;
    browse_request.filter_proc = FilterEditProc;
    browse_request.filter_subclass_id = kFilterSubclassId;
    browse_request.tree_proc = TreeViewProc;
    browse_request.tree_subclass_id = kTreeViewSubclassId;
    browse_request.values_proc = ListViewProc;
    browse_request.values_subclass_id = kListViewSubclassId;
    browse_request.callback_context = reinterpret_cast<DWORD_PTR>(this);
    if (!browse_.Create(browse_request))
    {
        return false;
    }
    appearance::ConfigureListView(browse_.values().hwnd());
    // keep a hidden regedit tree for tools that navigate it through tree messages
    regedit_compat_tree_.Create(hwnd_, instance_, kRegEditCompatTreeId, false, false);
    if (!regedit_compat_tree_.hwnd())
    {
        return false;
    }
    regedit_compat_tree_.SetRegEditLayout(true);
    regedit_compat_tree_.SetRootLabel(L"Computer");
    regedit_compat_tree_.PopulateRoots(RegistryStore::DefaultRoots(false));
    if (!SetWindowSubclass(regedit_compat_tree_.hwnd(), TreeViewProc, kTreeViewSubclassId, reinterpret_cast<DWORD_PTR>(this)))
    {
        return false;
    }
    TreeView_SelectItem(regedit_compat_tree_.hwnd(), TreeView_GetRoot(regedit_compat_tree_.hwnd()));
    SetWindowPos(regedit_compat_tree_.hwnd(), HWND_TOP, -32000, -32000, 1, 1, SWP_NOACTIVATE);
    ShowWindow(regedit_compat_tree_.hwnd(), SW_HIDE);

    value_tooltip_ =
        CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, instance_, nullptr);
    if (value_tooltip_)
    {
        TOOLINFOW info = {};
        info.cbSize = sizeof(info);
        info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        info.hwnd = hwnd_;
        info.uId = reinterpret_cast<UINT_PTR>(browse_.values().hwnd());
        info.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(value_tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        info.uId = reinterpret_cast<UINT_PTR>(browse_.go_button());
        info.lpszText = const_cast<wchar_t*>(util::Tr(L"Go"));
        SendMessageW(value_tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        SendMessageW(value_tooltip_, TTM_SETMAXTIPWIDTH, 0, kValueTooltipMaxWidth);
        SetDarkWindowTheme(value_tooltip_, Theme::UseDarkMode());
    }

    tab_ =
        CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS | TCS_FOCUSNEVER, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabId)), instance_, nullptr);
    ApplyFont(tab_, ui_font_);
    tab_strip_.Attach(tab_, [](void* context, int index) { static_cast<MainWindow::Impl*>(context)->CloseTab(index); }, this);

    tree_header_ = CreateWindowExW(0, L"STATIC", util::Tr(L"Key Tree"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT | SS_OWNERDRAW, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTreeHeaderId)), instance_, nullptr);
    tree_close_btn_ =
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTreeHeaderCloseId)), instance_, nullptr);
    filter_clear_btn_ =
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_CLIPSIBLINGS | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilterClearId)), instance_, nullptr);
    SetWindowPos(tree_close_btn_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    browse_.tree().SetIconResolver([this](const RegistryNode& node) { return KeyIconIndex(node, nullptr, nullptr); });
    browse_.tree().SetVirtualChildProvider(
        [this](const RegistryNode& node, const std::unordered_set<std::wstring>& existing_lower, std::vector<std::wstring>* out) { AppendTraceChildren(node, existing_lower, out); }
    );
    search_results_list_ = CreateWindowExW(
        0,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
        0,
        0,
        0,
        0,
        hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchResultsListId)),
        instance_,
        nullptr
    );
    LoadTabs();

    history_label_ = CreateWindowExW(
        0,
        L"STATIC",
        util::Tr(L"History"),
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT | SS_OWNERDRAW,
        0,
        0,
        0,
        0,
        hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHistoryLabelId)),
        instance_,
        nullptr
    );
    history_close_btn_ =
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHistoryHeaderCloseId)), instance_, nullptr);
    SetWindowPos(history_close_btn_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    status_bar_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusBarId)), instance_, nullptr);
    if (status_bar_)
    {
        int parts[4] = {0, 0, 0, 0};
        SendMessageW(status_bar_, SB_SETPARTS, 4, reinterpret_cast<LPARAM>(parts));
    }
    search_progress_ =
        CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_MARQUEE, 0, 0, 0, 0, status_bar_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchProgressId)), instance_, nullptr);
    if (search_progress_)
    {
        SendMessageW(search_progress_, PBM_SETMARQUEE, TRUE, 30);
        SendMessageW(search_progress_, PBM_SETRANGE32, 0, 1);
        ShowWindow(search_progress_, SW_HIDE);
    }
    history_list_ = CreateWindowExW(
        0,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
        0,
        0,
        0,
        0,
        hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHistoryListId)),
        instance_,
        nullptr
    );

    appearance::ConfigureListView(history_list_);
    appearance::ConfigureListView(search_results_list_);
    if (value_tooltip_)
    {
        TOOLINFOW tip = {};
        tip.cbSize = sizeof(tip);
        tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tip.hwnd = hwnd_;
        tip.lpszText = LPSTR_TEXTCALLBACKW;
        tip.uId = reinterpret_cast<UINT_PTR>(search_results_list_);
        SendMessageW(value_tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
        tip.uId = reinterpret_cast<UINT_PTR>(history_list_);
        SendMessageW(value_tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    SendMessageW(search_results_list_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    SendMessageW(history_list_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    SetWindowSubclass(history_list_, ListViewProc, kListViewSubclassId, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(search_results_list_, ListViewProc, kListViewSubclassId, reinterpret_cast<DWORD_PTR>(this));

    appearance::AttachThemedBorder(tree_header_);
    appearance::AttachThemedBorder(browse_.tree().hwnd());
    appearance::AttachThemedBorder(history_label_);
    appearance::AttachThemedBorder(browse_.address());
    appearance::AttachThemedBorder(browse_.go_button());
    UpdateGoButtonState();
    appearance::AttachThemedBorder(browse_.filter());

    ApplyUIFontToControls();

    CreateValueColumns();
    CreateHistoryColumns();
    CreateSearchColumns();
    ApplyThemeToChildren();
    SetValueGridEnabled(settings_.show_value_grid, false);
    if (toolbar_.hwnd())
    {
        UpdateUndoButtons();
        if (settings_.read_only)
        {
            SendMessageW(toolbar_.hwnd(), TB_SETSTATE, cmd::kEditPaste, 0);
            SendMessageW(toolbar_.hwnd(), TB_SETSTATE, cmd::kEditDelete, 0);
        }
    }

    browse_.roots() = RegistryStore::DefaultRoots(settings_.show_extra_hives);
    AppendRealRegistryRoot(&browse_.roots());
    browse_.tree().SetRegEditLayout(false);
    browse_.tree().SetRootLabel(TreeRootLabel(), TreeRootIcon());
    browse_.tree().PopulateRoots(browse_.roots());

    int initial_tab = tab_ ? TabCtrl_GetCurSel(tab_) : -1;
    if (initial_tab >= 0 && util::IsProcessPrivileged() && !IsLocalRegistryTabIndex(initial_tab))
    {
        const int local_tab = FindLocalRegistryTabIndex();
        if (local_tab < 0)
        {
            OpenLocalRegistryTab();
        }
        else
        {
            suppress_tab_change_ = true;
            TabCtrl_SetCurSel(tab_, local_tab);
            suppress_tab_change_ = false;
        }
        initial_tab = TabCtrl_GetCurSel(tab_);
    }
    if (initial_tab >= 0)
    {
        ApplyTabSelection(initial_tab);
    }
    else
    {
        SelectDefaultTreeItem();
    }
    StartValueListWorker();

    ApplyViewVisibility();
    ApplyAlwaysOnTop();
    UpdateStatus();
    return true;
}

void MainWindow::Impl::RunDeferredStartup()
{
    if (deferred_startup_complete_)
    {
        return;
    }
    deferred_startup_complete_ = true;
    const bool has_external_jump = !queued_external_jump_target_.empty();
    // external jumps & saved tab state take priority over the global tree state
    bool use_global_tree_state = (!has_external_jump && settings_.save_tree_state);
    if (use_global_tree_state && tab_)
    {
        int active_tab = TabCtrl_GetCurSel(tab_);
        if (active_tab >= 0 && static_cast<size_t>(active_tab) < tabs_.size())
        {
            const TabEntry& entry = tabs_[static_cast<size_t>(active_tab)];
            if (entry.kind == TabEntry::Kind::kRegistry &&
                (!entry.selected_path.empty() || !entry.expanded_paths.empty()))
            {
                use_global_tree_state = false;
            }
        }
    }
    startup_tree_restore_pending_ = use_global_tree_state;

    EnableAddressAutoComplete();
    ReloadThemeIcons();
    FlushTreeRedraw();
    ShowWindow(hwnd_, pending_show_cmd_);
    UpdateWindow(hwnd_);

    UpdateSearchResultsView();
    if (has_external_jump)
    {
        tree_state_restored_ = true;
    }
    else if (!use_global_tree_state)
    {
        tree_state_restored_ = true;
    }
    StartStartupCacheLoad(use_global_tree_state);
    ApplyQueuedExternalJump();
    StartTreeStateWorker();
    MarkTreeStateDirty();

    BuildMenus();
    UpdateStatus();
    if (settings_.auto_check_updates)
    {
        updates_.Check(true);
    }
}

void MainWindow::Impl::StartStartupCacheLoad(bool include_tree_state)
{
    StopStartupCacheLoad();
    const bool load_tree_state = include_tree_state && settings_.save_tree_state;
    int history_max_rows = history_max_rows_;
    int history_sort_column = history_sort_column_;
    bool history_sort_ascending = history_sort_ascending_;
    const HWND hwnd = hwnd_;
    startup_cache_session_.Start(L"StartupCacheThread", [this, load_tree_state, history_max_rows, history_sort_column, history_sort_ascending, hwnd](uint64_t generation, const std::atomic_bool& cancel) {
        auto payload = std::make_unique<StartupCachePayload>();
        payload->generation = generation;

        std::wstring comments_content;
        const std::wstring defaults_path =
            util::JoinPath(util::GetModuleDirectory(), L"assets\\comments\\default-comments.jsonc");
        if (util::ReadTextFile(defaults_path, &comments_content, nullptr, util::kMaxCommentFileBytes) &&
            (!changes::ParseComments(comments_content, &payload->default_comments) ||
             !changes::ValidateCatalog(payload->default_comments)))
        {
            payload->default_comments.clear();
        }
        const std::wstring comments_path = CommentsPath();
        if (!comments_path.empty() && GetFileAttributesW(comments_path.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            payload->comments_unreadable = !util::ReadTextFile(comments_path, &comments_content, nullptr, util::kMaxCommentFileBytes) ||
                                           !changes::ParseComments(comments_content, &payload->user_comments);
        }
        payload->comments_loaded = true;
        if (cancel.load())
        {
            return;
        }

        std::wstring history_path = HistoryCachePath();
        std::wstring history_content;
        if (!history_path.empty() && util::ReadTextFile(history_path, &history_content))
        {
            changes::ChangeHistory history;
            history.Replace(std::move(changes::ParseHistory(history_content).entries), static_cast<size_t>(history_max_rows));
            history.Sort(history_sort_column, history_sort_ascending);
            payload->history_entries = std::move(history.entries());
            payload->history_loaded = true;
        }
        else
        {
            payload->history_loaded = true;
        }
        if (cancel.load())
        {
            return;
        }

        if (load_tree_state)
        {
            std::wstring tree_path = TreeStatePath();
            std::wstring tree_content;
            if (!tree_path.empty() && util::ReadTextFile(tree_path, &tree_content, nullptr, util::kMaxStateFileBytes))
            {
                workspace::TreeState state = workspace::ParseTreeState(tree_content);
                payload->tree_selected_path = std::move(state.selected_path);
                payload->tree_expanded_paths = std::move(state.expanded_paths);
            }
            payload->tree_state_loaded = true;
        }

        if (cancel.load())
        {
            return;
        }
        if (hwnd && IsWindow(hwnd))
        {
            work::PostPayload(hwnd, frame::message_id::kStartupCacheReady, static_cast<WPARAM>(generation), payload);
        }
    });
}

void MainWindow::Impl::StopStartupCacheLoad()
{
    startup_cache_session_.CancelAndJoin();
}

void MainWindow::Impl::ApplyStartupCachePayload(std::unique_ptr<StartupCachePayload> owned)
{
    if (!owned)
    {
        return;
    }
    // ignore results from cancelled/replaced startup load
    if (!startup_cache_session_.IsCurrent(owned->generation))
    {
        return;
    }
    startup_cache_session_.Join();

    if (owned->comments_loaded)
    {
        default_comments_.Clear();
        default_comments_.Merge(owned->default_comments);
        changes::ValueComments loaded;
        loaded.Merge(owned->user_comments);
        // comments added during startup override older copies loaded from disk
        loaded.Merge(value_comments_.rules());
        value_comments_ = std::move(loaded);
        comments_unreadable_ = owned->comments_unreadable;
        comments_loaded_ = true;
        if (comments_unreadable_)
        {
            ui::PromptKeyChoice(
                hwnd_,
                util::Tr(L"The comments file couldn't be read, so comment changes won't be saved until it is fixed or removed."),
                CommentsPath(),
                util::Tr(L"Comments"),
                util::Tr(L"OK"),
                L"",
                L""
            );
        }
        RefreshValueListComments();
        UpdateSearchResultsView();
    }

    if (owned->history_loaded)
    {
        std::vector<HistoryEntry> pending_session_entries;
        if (!history_loaded_ && !change_history_.entries().empty())
        {
            pending_session_entries = change_history_.entries();
        }
        if (!change_history_.entries().empty())
        {
            owned->history_entries.insert(owned->history_entries.end(), change_history_.entries().begin(), change_history_.entries().end());
        }
        change_history_.Replace(std::move(owned->history_entries), static_cast<size_t>(history_max_rows_));
        change_history_.Sort(history_sort_column_, history_sort_ascending_);
        history_loaded_ = true;
        for (const auto& entry : pending_session_entries)
        {
            AppendHistoryCache(entry);
        }
        RebuildHistoryList();
    }

    if (owned->tree_state_loaded)
    {
        saved_tree_state_.selected_path = std::move(owned->tree_selected_path);
        saved_tree_state_.expanded_paths = std::move(owned->tree_expanded_paths);
        if (startup_tree_restore_pending_ && !tree_state_restored_)
        {
            applying_startup_tree_restore_ = true;
            RestoreTreeState();
            applying_startup_tree_restore_ = false;
        }
        else if (!tree_state_restored_)
        {
            tree_state_restored_ = true;
        }
        startup_tree_restore_pending_ = false;
    }
}

void MainWindow::Impl::OnDestroy()
{
    if (IsWindow(key_handles_window_))
    {
        DestroyWindow(key_handles_window_);
    }
    key_handles_window_ = nullptr;
    appearance::SetListGridChangedCallback(nullptr, nullptr);
    appearance::ReleaseListViews(hwnd_);
    if (hwnd_)
    {
        RemovePropW(hwnd_, kRegKitWindowProperty);
    }
    EndJumpUiBatch();
    // stop workers before releasing controls & resources they may still reference
    StopStartupCacheLoad();
    StopReplace();
    StopTraceParseSessions();
    StopDefaultParseSessions();
    StopRegFileParseSessions();
    StopTraceLoadWorker();
    StopDefaultLoadWorker();
    StopValueListWorker();
    const bool clearing_tree_state = cache_clear_on_close_ && (*cache_clear_on_close_ == CacheKind::kAll ||
                                                               *cache_clear_on_close_ == CacheKind::kTreeState);
    if (clearing_tree_state)
    {
        tree_state_saver_.Stop();
    }
    else
    {
        StopTreeStateWorker();
    }
    for (auto& tab : search_tabs_)
    {
        tab.run.reset();
    }
    updates_.Cancel();
    DiscardWorkerMessages();
    for (auto& entry : tabs_)
    {
        if (entry.kind == TabEntry::Kind::kRegFile)
        {
            ReleaseRegFileRoots(&entry);
        }
    }
    if (!restart_on_close_ && !reset_settings_on_close_)
    {
        if (settings_.clear_tabs_on_exit)
        {
            ClearTabsCache();
        }
        else if (settings_.save_tab_kinds != 0 && !SaveTabs())
        {
            ui::ShowError(hwnd_, util::Tr(L"The open tabs couldn't be saved for the next session."));
        }
    }
    ClearHistoryItems(false);
    if (settings_.clear_history_on_exit)
    {
        std::wstring history_path = HistoryCachePath();
        if (!history_path.empty())
        {
            DeleteFileW(history_path.c_str());
        }
    }
    for (TabEntry& tab : tabs_)
    {
        tab.session.reset();
    }
    session_ = local_session_;
    if (ui_font_ && ui_font_owned_)
    {
        DeleteObject(ui_font_);
    }
    ui_font_ = nullptr;
    ui_font_owned_ = false;
    if (icon_font_)
    {
        DeleteObject(icon_font_);
        icon_font_ = nullptr;
    }
    if (tree_images_)
    {
        ImageList_Destroy(tree_images_);
        tree_images_ = nullptr;
    }
    if (list_images_)
    {
        ImageList_Destroy(list_images_);
        list_images_ = nullptr;
    }
    if (address_go_icon_)
    {
        DestroyIcon(address_go_icon_);
        address_go_icon_ = nullptr;
    }
    if (accelerators_)
    {
        DestroyAcceleratorTable(accelerators_);
        accelerators_ = nullptr;
    }
    menu_items_.clear();
}

void MainWindow::Impl::DiscardWorkerMessages()
{
    if (!hwnd_)
    {
        return;
    }
    MSG message = {};
    const UINT payload_messages[] = {frame::message_id::kTraceLoadReady, frame::message_id::kDefaultLoadReady, frame::message_id::kStartupCacheReady, frame::message_id::kRegFileLoadReady, frame::message_id::kTraceParseBatch, frame::message_id::kDefaultParseBatch, frame::message_id::kValueListReady, frame::message_id::kReplaceReady, frame::message_id::kValuePreviewReady, frame::message_id::kSearchPreviewReady, frame::message_id::kSearchSortReady, frame::message_id::kSearchTabLoadReady, frame::message_id::kUpdateCheckReady};
    for (const UINT id : payload_messages)
    {
        while (PeekMessageW(&message, hwnd_, id, id, PM_REMOVE))
        {
            work::TakePayload<work::MoveOnly>(message.lParam).reset();
        }
    }
    while (PeekMessageW(&message, hwnd_, frame::message_id::kExternalHandoff, frame::message_id::kExternalHandoff, PM_REMOVE))
    {
        delete reinterpret_cast<std::wstring*>(message.lParam);
    }
}

} // namespace regkit
