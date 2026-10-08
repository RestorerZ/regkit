// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"

#include "registry/key_algorithms.h"

#include "ui/dialog_support.h"

#include "ui/dialog_metrics.h"

namespace regkit
{
using namespace window_detail;

// single line controls get taller when the UI font needs it
int MainWindow::Impl::TextRowHeight(int nominal, int padding) const
{
    const UINT dpi = win32::DpiForWindow(hwnd_);
    TEXTMETRICW metrics = {};
    if (HDC hdc = GetDC(hwnd_))
    {
        const HGDIOBJ old_font = SelectObject(hdc, ui_font_);
        GetTextMetricsW(hdc, &metrics);
        SelectObject(hdc, old_font);
        ReleaseDC(hwnd_, hdc);
    }
    return std::max(appearance::ScaleForDpi(nominal, dpi), static_cast<int>(metrics.tmHeight) + appearance::ScaleForDpi(padding, dpi));
}

void MainWindow::Impl::LayoutContent(bool dragging)
{
    const bool show_search = IsSearchTabSelected();
    const bool show_tree = settings_.show_tree && !show_search;
    const bool show_history = settings_.show_history && !show_search;
    const UINT dpi = win32::DpiForWindow(hwnd_);
    const int header_height = TextRowHeight(kPanelHeaderHeight);
    const int close_size = appearance::ScaleForDpi(kPanelCloseSize, dpi);
    const int close_inset = appearance::ScaleForDpi(kPanelCloseInset, dpi);
    const int left = content_rect_.left;
    const int right = content_rect_.right;
    const int width = right - left;
    const int top = content_rect_.top;
    const int bottom = content_rect_.bottom;

    struct Placement
    {
        HWND hwnd;
        RECT rect;
    };
    Placement placements[7] = {};
    int count = 0;
    auto place = [&](HWND hwnd, int x, int y, int w, int h) {
        if (hwnd)
        {
            placements[count++] = {hwnd, {x, y, x + w, y + h}};
        }
    };
    RECT dirty = {};
    UnionRect(&dirty, &tree_splitter_.rect, &history_splitter_.rect);

    history_splitter_.rect = {};
    if (show_history)
    {
        settings_.history_height = ClampValue(settings_.history_height, kMinHistoryHeight, std::max(kMinHistoryHeight, bottom - top - kHistoryMaxPadding));
        const int history_top = bottom - settings_.history_height;
        place(history_label_, left, history_top, width, header_height);
        place(history_close_btn_, right - close_inset - close_size, history_top + (header_height - close_size) / 2, close_size, close_size);
        place(history_list_, left, history_top + header_height - kPanelBorderOverlap, width, settings_.history_height - header_height + kPanelBorderOverlap);
        history_splitter_.rect = {left, history_top - kHistoryGap - kHistorySplitterHeight, right, history_top - kHistoryGap};
    }
    const int content_height = std::max(0, (show_history ? static_cast<int>(history_splitter_.rect.top) : bottom) - top);

    tree_splitter_.rect = {};
    int list_x = left;
    if (show_tree)
    {
        const int tree_width = ClampValue(settings_.tree_width, kMinTreeWidth, std::max(kMinTreeWidth, width - kMinValueListWidth - kSplitterWidth));
        place(tree_header_, left, top, tree_width, header_height);
        place(tree_close_btn_, left + tree_width - close_inset - close_size, top + (header_height - close_size) / 2, close_size, close_size);
        place(browse_.tree().hwnd(), left, top + header_height - kPanelBorderOverlap, tree_width, std::max(0, content_height - header_height) + kPanelBorderOverlap);
        tree_splitter_.rect = {left + tree_width, top, left + tree_width + kSplitterWidth, top + content_height};
        list_x = left + tree_width + kSplitterWidth;
    }
    if (show_search)
    {
        place(search_results_list_, left, top, width, content_height);
    }
    else
    {
        place(browse_.values().hwnd(), list_x, top, right - list_x, content_height);
    }

    // while dragging, redraw only what the old and new positions cover, otherwise the caller repaints everything
    const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | (dragging ? 0 : SWP_NOREDRAW);
    HDWP batch = BeginDeferWindowPos(count);
    for (int i = 0; i < count; ++i)
    {
        const Placement& p = placements[i];
        RECT old_rect = {};
        if (dragging && GetChildRectInParent(hwnd_, p.hwnd, &old_rect))
        {
            UnionRect(&dirty, &dirty, &old_rect);
            UnionRect(&dirty, &dirty, &p.rect);
        }
        if (batch)
        {
            batch = DeferWindowPos(batch, p.hwnd, nullptr, p.rect.left, p.rect.top, p.rect.right - p.rect.left, p.rect.bottom - p.rect.top, flags);
        }
    }
    // a failed batch is abandoned, so every control is placed again
    if (!batch || !EndDeferWindowPos(batch))
    {
        for (int i = 0; i < count; ++i)
        {
            const Placement& p = placements[i];
            SetWindowPos(p.hwnd, nullptr, p.rect.left, p.rect.top, p.rect.right - p.rect.left, p.rect.bottom - p.rect.top, flags);
        }
    }
    LayoutValueGridToolbar();
    if (dragging)
    {
        UnionRect(&dirty, &dirty, &tree_splitter_.rect);
        UnionRect(&dirty, &dirty, &history_splitter_.rect);
        RedrawWindow(hwnd_, &dirty, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    }
}

void MainWindow::Impl::DragSplitter(ui::Splitter* splitter, int* size, POINT point)
{
    const int next = splitter->Track(point);
    if (next != *size)
    {
        *size = next;
        LayoutContent(true);
    }
}

void MainWindow::Impl::ApplyViewVisibility()
{
    bool show_search = IsSearchTabSelected();
    bool show_tree = settings_.show_tree && !show_search;
    bool show_value = show_value_ && !show_search;
    bool show_history = settings_.show_history && !show_search;
    ShowWindow(toolbar_.hwnd(), settings_.show_toolbar ? SW_SHOW : SW_HIDE);
    ShowWindow(browse_.address(), settings_.show_address_bar ? SW_SHOW : SW_HIDE);
    ShowWindow(browse_.go_button(), settings_.show_address_bar ? SW_SHOW : SW_HIDE);
    ShowWindow(tab_, settings_.show_tab_control ? SW_SHOW : SW_HIDE);
    ShowWindow(browse_.filter(), (show_value && settings_.show_filter_bar) ? SW_SHOW : SW_HIDE);
    ShowWindow(filter_clear_btn_, (show_value && settings_.show_filter_bar) ? SW_SHOW : SW_HIDE);
    ShowWindow(tree_header_, show_tree ? SW_SHOW : SW_HIDE);
    ShowWindow(tree_close_btn_, show_tree ? SW_SHOW : SW_HIDE);
    ShowWindow(browse_.tree().hwnd(), show_tree ? SW_SHOW : SW_HIDE);
    ShowWindow(browse_.values().hwnd(), show_value ? SW_SHOW : SW_HIDE);
    ShowWindow(history_label_, show_history ? SW_SHOW : SW_HIDE);
    ShowWindow(history_close_btn_, show_history ? SW_SHOW : SW_HIDE);
    ShowWindow(history_list_, show_history ? SW_SHOW : SW_HIDE);
    ShowWindow(search_results_list_, show_search ? SW_SHOW : SW_HIDE);
    appearance::LayoutListViews(hwnd_);
    if (show_search && search_results_list_)
    {
        LONG_PTR style = GetWindowLongPtrW(search_results_list_, GWL_STYLE);
        if (style & LVS_SINGLESEL)
        {
            SetWindowLongPtrW(search_results_list_, GWL_STYLE, style & ~LVS_SINGLESEL);
            SetWindowPos(search_results_list_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
    }
    ShowWindow(status_bar_, settings_.show_status_bar ? SW_SHOW : SW_HIDE);
    if (search_progress_)
    {
        const SearchTab* shown = ShownSearchTab();
        const bool show_progress = settings_.show_status_bar && show_search && shown && shown->run && !IsCompareTabSelected();
        SendMessageW(search_progress_, PBM_SETMARQUEE, show_progress, 30);
        ShowWindow(search_progress_, show_progress ? SW_SHOW : SW_HIDE);
    }

    RECT rect = {};
    GetClientRect(hwnd_, &rect);
    LayoutControls(rect.right, rect.bottom);
}

void MainWindow::Impl::ApplyTabSelection(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= tabs_.size())
    {
        return;
    }
    const TabEntry& entry = tabs_[static_cast<size_t>(index)];
    if (entry.kind == TabEntry::Kind::kRegistry)
    {
        SuspendTreeRedraw();
        // a tab restored at startup connects the first time it is shown
        bool shown = false;
        if (entry.session)
        {
            ShowSession(std::shared_ptr<RegistrySession>(entry.session));
            shown = true;
        }
        else if (entry.registry_mode == RegistryMode::kLocal && entry.registry_view)
        {
            ShowSession(LocalViewSession(entry.registry_view));
            shown = true;
        }
        else if (entry.registry_mode == RegistryMode::kRemote && !entry.remote_machine.empty())
        {
            shown = ConnectRemoteRegistry(std::wstring(entry.remote_machine), false);
        }
        else if (entry.registry_mode == RegistryMode::kOffline && !entry.offline_path.empty())
        {
            shown = LoadOfflineRegistryFromPath(std::wstring(entry.offline_path), false);
        }
        if (!shown)
        {
            ShowSession(local_session_);
        }
        RestoreRegistryTabState(index);
    }
    else if (entry.kind == TabEntry::Kind::kSearch)
    {
        EnsureSearchTabResultsLoaded(entry.search_index);
    }
    else if (entry.kind == TabEntry::Kind::kRegFile)
    {
        SyncRegFileTabSelection();
    }
}

constexpr DWORD kMaxHiveValueBytes = 64 * 1024;

void MainWindow::Impl::ResetHiveListCache()
{
    hive_list_loaded_ = false;
    hive_list_.clear();
    hive_roots_.reset();
}

void MainWindow::Impl::EnsureHiveListLoaded()
{
    if (hive_list_loaded_)
    {
        return;
    }
    hive_list_loaded_ = true;
    hive_list_.clear();
    auto hive_roots = std::make_shared<std::unordered_set<std::wstring>>();
    hive_roots_ = hive_roots;

    HKEY hklm = nullptr;
    for (const auto& root : browse_.roots())
    {
        if (EqualsInsensitive(root.display_name, L"HKEY_LOCAL_MACHINE"))
        {
            hklm = root.root;
            break;
        }
    }
    if (!hklm)
    {
        return;
    }
    // cache mounted hive paths for file lookup & root icons
    util::UniqueHKey hive_key;
    if (RegOpenKeyExW(hklm, L"SYSTEM\\CurrentControlSet\\Control\\hivelist", 0, KEY_READ, hive_key.put()) !=
        ERROR_SUCCESS)
    {
        return;
    }
    registry_backend::EnumerateKey(
        registry_backend::RegistryKeyHandle(std::move(hive_key)),
        true,
        true,
        false,
        nullptr,
        [&](const ValueInfo& value, const BYTE* data, DWORD size) {
            if (!data || size == 0 || value.name.empty() || (value.type != REG_SZ && value.type != REG_EXPAND_SZ))
            {
                return true;
            }
            std::wstring path(reinterpret_cast<const wchar_t*>(data), size / sizeof(wchar_t));
            while (!path.empty() && path.back() == L'\0')
            {
                path.pop_back();
            }
            path = NormalizeHiveFilePath(path);
            if (path.empty())
            {
                return true;
            }
            std::wstring name_lower = ToLower(value.name);
            hive_list_.emplace(name_lower, std::move(path));
            hive_roots->insert(std::move(name_lower));
            return true;
        },
        nullptr,
        kMaxHiveValueBytes,
        nullptr
    );
}

std::wstring MainWindow::Impl::LookupHivePath(const RegistryNode& node, bool* is_root)
{
    std::wstring nt_path = registry_path::BuildNative(node);
    if (nt_path.empty() && !node.root_name.empty())
    {
        // nodes without a predefined handle still name their root
        RegistryNode named = node;
        named.root = registry_path::RootFromName(node.root_name);
        nt_path = named.root == HKEY_CLASSES_ROOT ? registry_path::JoinSubkey(L"\\REGISTRY\\MACHINE\\SOFTWARE\\Classes", node.subkey) : registry_path::BuildNative(named);
    }
    return LookupNativeHivePath(nt_path, is_root);
}

std::wstring MainWindow::Impl::LookupNativeHivePath(const std::wstring& nt_path, bool* is_root)
{
    if (is_root)
    {
        *is_root = false;
    }
    EnsureHiveListLoaded();
    if (hive_list_.empty() || nt_path.empty())
    {
        return L"";
    }
    std::wstring nt_lower = ToLower(nt_path);
    size_t best_len = 0;
    std::wstring best_path;
    for (const auto& entry : hive_list_)
    {
        const std::wstring& hive_key = entry.first;
        if (nt_lower.size() < hive_key.size())
        {
            continue;
        }
        if (nt_lower.compare(0, hive_key.size(), hive_key) != 0)
        {
            continue;
        }
        if (nt_lower.size() > hive_key.size() && nt_lower[hive_key.size()] != L'\\')
        {
            continue;
        }
        if (hive_key.size() > best_len)
        {
            best_len = hive_key.size();
            best_path = entry.second;
        }
    }
    if (best_len > 0 && is_root)
    {
        *is_root = nt_lower.size() == best_len;
    }
    return best_path;
}

int MainWindow::Impl::KeyIconIndex(const RegistryNode& node, bool* is_link, bool* is_hive_root)
{
    if (is_link)
    {
        *is_link = false;
    }
    if (is_hive_root)
    {
        *is_hive_root = false;
    }
    if (node.simulated)
    {
        return kFolderSimIconIndex;
    }
    const KeyInspection inspection = RegistryStore::InspectKey(node, false);
    const bool denied = inspection.denied;
    if (inspection.link)
    {
        if (is_link)
        {
            *is_link = true;
        }
        return kSymlinkIconIndex;
    }
    bool hive_root = false;
    std::wstring hive_path = LookupHivePath(node, &hive_root);
    if (!hive_path.empty() && hive_root && node.subkey.empty())
    {
        if (node.root == HKEY_CURRENT_USER || EqualsInsensitive(node.root_name, L"HKEY_CURRENT_USER"))
        {
            // treat HKCU as an alias
            hive_root = false;
        }
    }
    if (!hive_path.empty() && hive_root)
    {
        if (is_hive_root)
        {
            *is_hive_root = true;
        }
        return denied ? kDatabaseDeniedIconIndex : kDatabaseIconIndex;
    }
    return denied ? kFolderDeniedIconIndex : inspection.is_volatile ? kFolderVolatileIconIndex
                                                                    : kFolderIconIndex;
}

std::wstring MainWindow::Impl::ResolveIconDir(bool use_light) const
{
    if (IsIconSetName(settings_.icon_set, kIconSetCustom))
    {
        std::wstring root = util::JoinPath(util::GetAppDataFolder(), L"icons");
        if (root.empty())
        {
            return L"";
        }
        std::wstring dark_dir = util::JoinPath(root, L"dark");
        std::wstring light_dir = util::JoinPath(root, L"light");
        if (util::IsDirectory(dark_dir) && util::IsDirectory(light_dir))
        {
            return use_light ? light_dir : dark_dir;
        }
        return util::IsDirectory(root) ? root : L"";
    }
    const std::wstring base = AssetsIconsRoot();
    if (!IsIconSetName(settings_.icon_set, kIconSetClassic) || base.empty())
    {
        return L"";
    }
    const std::wstring dir = util::JoinPath(base, kIconSetClassic);
    return util::IsDirectory(dir) ? dir : L"";
}

std::wstring MainWindow::Impl::ResolveIconPath(const wchar_t* filename) const
{
    if (!filename || !*filename || icon_dir_.empty())
    {
        return L"";
    }
    return util::JoinPath(icon_dir_, filename);
}

bool MainWindow::Impl::ShouldUseLightIcons() const
{
    switch (theme_mode_)
    {
    case ThemeMode::kDark:
        return true;
    case ThemeMode::kLight:
        return false;
    case ThemeMode::kSystem:
        return Theme::IsSystemDarkMode();
    case ThemeMode::kCustom:
    default:
        return Theme::UseDarkMode();
    }
}

void MainWindow::Impl::ApplyGridToolbarIcons()
{
    appearance::ReloadListGridIcons();
    appearance::SetListGridChangedCallback(
        [](void* context, bool enabled) {
            static_cast<MainWindow::Impl*>(context)->SetValueGridEnabled(enabled, true);
        },
        this
    );
}

void MainWindow::Impl::LayoutValueGridToolbar()
{
    appearance::LayoutListViews(hwnd_);
}

void MainWindow::Impl::SetValueGridEnabled(bool enabled, bool persist)
{
    settings_.show_value_grid = enabled;
    appearance::SetListGridEnabled(enabled);
    if (persist)
    {
        SaveSettings();
    }
}

ToolbarIcon MainWindow::Impl::MakeToolbarIcon(const wchar_t* filename, int resource_id) const
{
    ToolbarIcon icon;
    icon.resource_id = resource_id + (ShouldUseLightIcons() ? IDI_ICON_LIGHT_OFFSET : 0);
    icon.path = ResolveIconPath(filename);
    return icon;
}

void MainWindow::Impl::ReloadThemeIcons()
{
    UINT dpi = win32::DpiForWindow(hwnd_);
    bool use_light = ShouldUseLightIcons();
    icon_dir_ = ResolveIconDir(use_light);
    auto set_redraw = [](HWND hwnd, bool enable) {
        if (!hwnd)
        {
            return;
        }
        SendMessageW(hwnd, WM_SETREDRAW, enable ? TRUE : FALSE, 0);
    };
    // pause redraw while every image list is replaced
    set_redraw(toolbar_.hwnd(), false);
    set_redraw(browse_.tree().hwnd(), false);
    set_redraw(browse_.values().hwnd(), false);
    set_redraw(search_results_list_, false);
    set_redraw(browse_.go_button(), false);

    toolbar_.LoadIcons(
        {
            MakeToolbarIcon(L"local-registry.ico", IDI_ICON_LOCAL_REGISTRY),
            MakeToolbarIcon(L"remote-registry.ico", IDI_ICON_REMOTE_REGISTRY),
            MakeToolbarIcon(L"offline-registry.ico", IDI_ICON_OFFLINE_REGISTRY),
            MakeToolbarIcon(L"search.ico", IDI_ICON_SEARCH),
            MakeToolbarIcon(L"replace.ico", IDI_ICON_REPLACE),
            MakeToolbarIcon(L"undo.ico", IDI_ICON_UNDO),
            MakeToolbarIcon(L"redo.ico", IDI_ICON_REDO),
            MakeToolbarIcon(L"copy.ico", IDI_ICON_COPY),
            MakeToolbarIcon(L"paste.ico", IDI_ICON_PASTE),
            MakeToolbarIcon(L"delete.ico", IDI_ICON_DELETE),
            MakeToolbarIcon(L"refresh.ico", IDI_ICON_REFRESH),
            MakeToolbarIcon(L"back.ico", IDI_ICON_BACK),
            MakeToolbarIcon(L"forward.ico", IDI_ICON_FORWARD),
            MakeToolbarIcon(L"up.ico", IDI_ICON_UP),
        },
        kToolbarIconSize,
        kToolbarGlyphSize
    );

    BuildImageLists();
    if (browse_.tree().hwnd())
    {
        browse_.tree().SetImageList(tree_images_);
    }
    if (browse_.values().hwnd())
    {
        browse_.values().SetImageList(list_images_);
    }
    if (search_results_list_)
    {
        ListView_SetImageList(search_results_list_, list_images_, LVSIL_SMALL);
    }

    if (address_go_icon_)
    {
        DestroyIcon(address_go_icon_);
        address_go_icon_ = nullptr;
    }
    address_go_icon_ = appearance::LoadIconResource(use_light ? IDI_ICON_LIGHT_GO : IDI_ICON_DARK_GO, kToolbarGlyphSize, dpi);
    ApplyGridToolbarIcons();
    LayoutValueGridToolbar();

    set_redraw(toolbar_.hwnd(), true);
    set_redraw(browse_.tree().hwnd(), true);
    set_redraw(browse_.values().hwnd(), true);
    set_redraw(search_results_list_, true);
    set_redraw(browse_.go_button(), true);
    if (toolbar_.hwnd())
    {
        InvalidateRect(toolbar_.hwnd(), nullptr, TRUE);
    }
    if (browse_.tree().hwnd())
    {
        InvalidateRect(browse_.tree().hwnd(), nullptr, TRUE);
    }
    if (browse_.values().hwnd())
    {
        InvalidateRect(browse_.values().hwnd(), nullptr, TRUE);
    }
    if (search_results_list_)
    {
        InvalidateRect(search_results_list_, nullptr, TRUE);
    }
    if (browse_.go_button())
    {
        InvalidateRect(browse_.go_button(), nullptr, TRUE);
    }
}

void MainWindow::Impl::LayoutControls(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    const int padding = 8;
    UINT dpi = win32::DpiForWindow(hwnd_);
    const int address_height = TextRowHeight(appearance::metrics::kControlHeight);
    const int address_btn_width = std::max(appearance::ScaleForDpi(18, dpi), address_height);
    const int tabs_height = std::max(20, tab_height_);
    const int filter_height = address_height;
    const int filter_min_width = 160;
    const int filter_max_width = 260;
    const int filter_gap = 6;
    int status_height = 0;
    if (status_bar_ && settings_.show_status_bar)
    {
        RECT sb_rect = {};
        GetWindowRect(status_bar_, &sb_rect);
        status_height = sb_rect.bottom - sb_rect.top;
        if (status_height <= 0)
        {
            status_height = 20;
        }
    }
    const bool show_search = IsSearchTabSelected();
    const bool show_value = show_value_ && !show_search;

    int y = kMainVerticalGap;

    const bool dragging_splitter = tree_splitter_.dragging() || history_splitter_.dragging();
    auto place = [&](HWND hwnd, int x, int y_pos, int w, int h) {
        if (!hwnd)
        {
            return;
        }
        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
        if (!dragging_splitter)
        {
            // wait for final redraw
            flags |= SWP_NOREDRAW;
        }
        SetWindowPos(hwnd, nullptr, x, y_pos, w, h, flags);
    };

    if (settings_.show_toolbar)
    {
        SendMessageW(toolbar_.hwnd(), TB_AUTOSIZE, 0, 0);
        SIZE ideal = {};
        SendMessageW(toolbar_.hwnd(), TB_GETMAXSIZE, 0, reinterpret_cast<LPARAM>(&ideal));
        RECT tb_rect = {};
        GetWindowRect(toolbar_.hwnd(), &tb_rect);
        int toolbar_height = tb_rect.bottom - tb_rect.top;
        if (toolbar_height <= 0)
        {
            toolbar_height = ideal.cy;
        }
        const int toolbar_area_width = std::max(0, width - padding * 2);
        place(toolbar_.hwnd(), padding, y, toolbar_area_width, toolbar_height);
        y += toolbar_height;
    }
    if (settings_.show_address_bar)
    {
        int address_width = width - padding * 2 - address_btn_width - 2;
        if (address_width < 120)
        {
            address_width = 120;
        }
        place(browse_.address(), padding, y, address_width, address_height);
        place(browse_.go_button(), padding + address_width, y, address_btn_width, address_height);
        SetEditMargins(browse_.address(), 6, 6);
        SetEditVerticalRect(browse_.address(), ui_font_, 2, 6, 6);
        y += address_height + kMainVerticalGap;
    }

    int tabs_width = width - padding * 2;
    bool show_tabs = settings_.show_tab_control && tab_;
    bool show_filter = show_value && settings_.show_filter_bar && browse_.filter();
    bool show_tab_row = show_tabs || show_filter;
    if (show_tab_row)
    {
        if (show_tabs && show_filter)
        {
            int available = std::max(0, tabs_width);
            int min_needed = kTabMinWidth + filter_min_width + filter_gap;
            if (available >= min_needed)
            {
                int target_width = ClampValue(available / 4, filter_min_width, filter_max_width);
                int filter_width =
                    std::min(target_width, std::max(filter_min_width, available - kTabMinWidth - filter_gap));
                tabs_width = std::max(kTabMinWidth, available - filter_width - filter_gap);
                int filter_y = y + std::max(0, (tabs_height - filter_height) / 2);
                int edit_width = std::max(filter_min_width / 2, filter_width - address_btn_width);
                place(tab_, padding, y, tabs_width, tabs_height);
                place(browse_.filter(), padding + tabs_width + filter_gap, filter_y, edit_width, filter_height);
                place(filter_clear_btn_, padding + tabs_width + filter_gap + edit_width, filter_y, address_btn_width, filter_height);
                SetEditMargins(browse_.filter(), 6, 6);
                SetEditVerticalRect(browse_.filter(), ui_font_, 2, 6, 6);
                ShowWindow(browse_.filter(), SW_SHOW);
                ShowWindow(filter_clear_btn_, SW_SHOW);
            }
            else
            {
                // keep tabs usable when there isnt space for both controls
                show_filter = false;
            }
        }
        if (show_tabs && !show_filter)
        {
            place(tab_, padding, y, tabs_width, tabs_height);
            if (browse_.filter())
            {
                ShowWindow(browse_.filter(), SW_HIDE);
            }
            ShowWindow(filter_clear_btn_, SW_HIDE);
        }
        else if (!show_tabs && show_filter)
        {
            int available = std::max(0, tabs_width);
            int filter_width = ClampValue(available, filter_min_width, filter_max_width);
            int filter_y = y + std::max(0, (tabs_height - filter_height) / 2);
            int filter_x = padding + std::max(0, tabs_width - filter_width);
            int edit_width = std::max(filter_min_width / 2, filter_width - address_btn_width);
            place(browse_.filter(), filter_x, filter_y, edit_width, filter_height);
            place(filter_clear_btn_, filter_x + edit_width, filter_y, address_btn_width, filter_height);
            SetEditMargins(browse_.filter(), 6, 6);
            SetEditVerticalRect(browse_.filter(), ui_font_, 2, 6, 6);
            ShowWindow(browse_.filter(), SW_SHOW);
            ShowWindow(filter_clear_btn_, SW_SHOW);
        }
        y += tabs_height + kMainVerticalGap;
    }
    else
    {
        if (tab_)
        {
            ShowWindow(tab_, SW_HIDE);
        }
        if (browse_.filter())
        {
            ShowWindow(browse_.filter(), SW_HIDE);
        }
        ShowWindow(filter_clear_btn_, SW_HIDE);
    }

    const int status_top = height - status_height;
    if (settings_.show_status_bar && status_bar_)
    {
        place(status_bar_, 0, status_top, width, status_height);
        SendMessageW(status_bar_, WM_SIZE, 0, 0);
    }

    content_rect_ = {0, y, width, status_top};
    LayoutContent(dragging_splitter);

    UpdateStatus();
    if (!dragging_splitter)
    {
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE | RDW_UPDATENOW);
    }
}

} // namespace regkit
