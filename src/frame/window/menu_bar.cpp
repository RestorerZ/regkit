// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/commands/command_detail.h"
#include "frame/window_impl.h"

#include "dialogs/bitfield_definition.h"

#include "frame/tools/research_links.h"
#include "frame/window/shortcut_bindings.h"
#include "win32/shell_integration.h"
#include "win32/shell_paths.h"
#include "win32/text_transform.h"
#include "win32/translation.h"

#include <filesystem>

namespace regkit
{
using namespace command_detail;

namespace
{

struct CacheAvailability
{
    bool any = false;
    bool tabs = false;
    bool history = false;
    bool search_history = false;
    bool tree_state = false;
    bool temporary = false;
};

bool HasCachePattern(const wchar_t* name, const wchar_t* prefix, const wchar_t* suffix)
{
    if (!name || !prefix || !suffix)
    {
        return false;
    }
    return wcslen(name) >= wcslen(prefix) + wcslen(suffix) && util::StartsWithInsensitive(name, prefix) &&
           util::EndsWithInsensitive(name, suffix);
}

CacheAvailability InspectCacheFiles(const std::wstring& folder)
{
    CacheAvailability available;
    if (folder.empty())
    {
        return available;
    }
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(util::JoinPath(folder, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return available;
    }
    do
    {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            continue;
        }
        available.any = true;
        const wchar_t* name = data.cFileName;
        available.tabs = available.tabs || util::EqualsInsensitive(name, L"tabs.ini") ||
                         util::EqualsInsensitive(name, L"session.ini") || HasCachePattern(name, L"search_", L".tsv") ||
                         HasCachePattern(name, L"compare_", L".tsv");
        available.history = available.history || util::EqualsInsensitive(name, L"history.tsv");
        available.search_history = available.search_history || util::EqualsInsensitive(name, L"search_history.txt");
        available.tree_state = available.tree_state || util::EqualsInsensitive(name, L"tree_state.ini");
        available.temporary = available.temporary || HasCachePattern(name, L"export_", L".reg");
    } while (FindNextFileW(find, &data) != 0);
    FindClose(find);
    return available;
}

bool MenuHasDirectCommand(HMENU menu, UINT command)
{
    const int count = menu ? GetMenuItemCount(menu) : 0;
    for (int position = 0; position < count; ++position)
    {
        if (GetMenuItemID(menu, position) == command)
        {
            return true;
        }
    }
    return false;
}

int SubMenuPosition(HMENU menu, UINT command)
{
    const int count = GetMenuItemCount(menu);
    for (int position = 0; position < count; ++position)
    {
        if (MenuHasDirectCommand(GetSubMenu(menu, position), command))
        {
            return position;
        }
    }
    return -1;
}

bool IsMenuInside(HMENU root, HMENU menu)
{
    const int count = root ? GetMenuItemCount(root) : 0;
    for (int position = 0; position < count; ++position)
    {
        HMENU child = GetSubMenu(root, position);
        if (child && (child == menu || IsMenuInside(child, menu)))
        {
            return true;
        }
    }
    return false;
}

} // namespace

std::wstring MainWindow::Impl::CommandShortcutText(int command_id) const
{
    for (const auto& binding : frame::kShortcutBindings)
    {
        if (binding.command == command_id)
        {
            return binding.text;
        }
    }
    return L"";
}

std::wstring MainWindow::Impl::CommandTooltipText(int command_id) const
{
    switch (command_id)
    {
    case cmd::kRegistryLocal:
        return util::Tr(L"Local Registry");
    case cmd::kRegistryNetwork:
        return util::Tr(L"Remote Registry");
    case cmd::kRegistryOffline:
        return util::Tr(L"Offline Registry");
    case cmd::kEditFind:
        return util::Tr(L"Find");
    case cmd::kEditReplace:
        return util::Tr(L"Replace");
    case cmd::kFileSave:
        return util::Tr(L"Save");
    case cmd::kFileExport:
        return util::Tr(L"Export");
    case cmd::kEditUndo:
        return util::Tr(L"Undo");
    case cmd::kEditRedo:
        return util::Tr(L"Redo");
    case cmd::kEditCopy:
        return util::Tr(L"Copy");
    case cmd::kEditPaste:
        return util::Tr(L"Paste");
    case cmd::kEditDelete:
        return util::Tr(L"Delete");
    case cmd::kViewRefresh:
        return util::Tr(L"Refresh");
    case cmd::kNavBack:
        return util::Tr(L"Back");
    case cmd::kNavForward:
        return util::Tr(L"Forward");
    case cmd::kNavUp:
        return util::Tr(L"Up");
    default:
        return L"";
    }
}

bool MainWindow::Impl::EnsureWritable()
{
    if (!settings_.read_only)
    {
        return true;
    }
    ui::ShowWarning(hwnd_, util::Tr(L"Read only mode is enabled."));
    return false;
}

void MainWindow::Impl::RefreshStorageMenuState(HMENU menu)
{
    if (!menu)
    {
        return;
    }
    const int clear_position = MenuHasDirectCommand(menu, cmd::kFileClearCacheAll) ? -1 : SubMenuPosition(menu, cmd::kFileClearCacheAll);
    HMENU clear_menu = clear_position >= 0 ? GetSubMenu(menu, clear_position) : (MenuHasDirectCommand(menu, cmd::kFileClearCacheAll) ? menu : nullptr);
    const bool has_reset = MenuHasDirectCommand(menu, cmd::kOptionsResetSettings);
    if (!clear_menu && !has_reset)
    {
        return;
    }
    if (clear_menu)
    {
        const CacheAvailability cache = InspectCacheFiles(CacheFolderPath());
        EnableMenuItem(clear_menu, cmd::kFileClearCacheAll, MF_BYCOMMAND | (cache.any ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(clear_menu, cmd::kFileClearCacheTabs, MF_BYCOMMAND | (cache.tabs ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(clear_menu, cmd::kFileClearCacheHistory, MF_BYCOMMAND | (cache.history ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(clear_menu, cmd::kFileClearCacheSearchHistory, MF_BYCOMMAND | (cache.search_history ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(clear_menu, cmd::kFileClearCacheTreeState, MF_BYCOMMAND | (cache.tree_state ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(clear_menu, cmd::kFileClearCacheTemporary, MF_BYCOMMAND | (cache.temporary ? MF_ENABLED : MF_GRAYED));
        if (clear_position >= 0)
        {
            EnableMenuItem(menu, clear_position, MF_BYPOSITION | (cache.any ? MF_ENABLED : MF_GRAYED));
        }
    }
    if (has_reset)
    {
        const workspace::Settings settings = CurrentSettings();
        const bool changed = workspace::SerializeSettings(settings) != workspace::SerializeSettings(workspace::DefaultOptions(settings));
        EnableMenuItem(menu, cmd::kOptionsResetSettings, MF_BYCOMMAND | (changed ? MF_ENABLED : MF_GRAYED));
    }
}

void MainWindow::Impl::FillBitfieldMenu(HMENU menu)
{
    if (GetMenuItemCount(menu) != 1 || GetMenuItemID(menu, 0) != cmd::kToolsBitfieldDefinitions)
    {
        return;
    }
    const std::vector<editors::bitfield::DefinitionFile>& files = editors::bitfield::BundledFiles();
    if (!files.empty())
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    for (size_t i = 0; i < files.size() && i <= static_cast<size_t>(cmd::kToolsBitfieldFileMax - cmd::kToolsBitfieldFileBase); ++i)
    {
        std::wstring label = files[i].name.empty() ? util::FileBaseName(files[i].path) : files[i].name;
        label.append(L"   (").append(std::to_wstring(files[i].definitions.size())).append(L")");
        AppendMenuW(menu, MF_STRING, cmd::kToolsBitfieldFileBase + i, label.c_str());
    }
}

void MainWindow::Impl::BuildMenus()
{
    if (deferred_startup_complete_ && !favorites_loaded_)
    {
        RefreshFavoritesCache();
    }
    if (deferred_startup_complete_ && !bundled_defaults_loaded_)
    {
        RefreshBundledDefaultsCache();
    }
    menu_items_.clear();
    HMENU menu = CreateMenu();
    auto append_menu = [&](HMENU target, UINT flags, int command, const wchar_t* text) {
        std::wstring shortcut = CommandShortcutText(command);
        if (!shortcut.empty())
        {
            std::wstring combined = std::wstring(text) + L"\t" + shortcut;
            AppendMenuW(target, flags, command, combined.c_str());
            return;
        }
        AppendMenuW(target, flags, command, text);
    };
    auto append_popup = [&](HMENU target, HMENU popup, const wchar_t* text) {
        AppendMenuW(target, MF_POPUP, reinterpret_cast<UINT_PTR>(popup), text);
    };
    auto separator = [](HMENU target) { AppendMenuW(target, MF_SEPARATOR, 0, nullptr); };

    HMENU file_menu = CreatePopupMenu();
    append_menu(file_menu, MF_STRING, cmd::kFileOpenRegFile, util::Tr(L"Open .reg File..."));
    append_menu(file_menu, MF_STRING, cmd::kFileSave, util::Tr(L"Save"));
    separator(file_menu);
    append_menu(file_menu, MF_STRING, cmd::kFileImport, util::Tr(L"Import..."));
    append_menu(file_menu, MF_STRING, cmd::kFileExport, util::Tr(L"Export..."));
    separator(file_menu);
    append_menu(file_menu, MF_STRING, cmd::kFileImportComments, util::Tr(L"Import Comments..."));
    append_menu(file_menu, MF_STRING, cmd::kFileExportComments, util::Tr(L"Export Comments..."));
    separator(file_menu);
    append_menu(file_menu, MF_STRING, cmd::kRegistryLocal, util::Tr(L"Local Registry"));
    if (win32::HasAlternateView())
    {
        append_menu(file_menu, MF_STRING, cmd::kRegistryLocal32, util::Tr(L"Local Registry (32-bit)"));
    }
    append_menu(file_menu, MF_STRING, cmd::kRegistryNetwork, util::Tr(L"Remote Registry..."));
    append_menu(file_menu, MF_STRING, cmd::kRegistryOffline, util::Tr(L"Offline Registry..."));
    append_menu(file_menu, MF_STRING, cmd::kFileSaveOfflineHive, util::Tr(L"Save Offline Hive..."));
    separator(file_menu);
    append_menu(file_menu, MF_STRING, cmd::kFileLoadHive, util::Tr(L"Load Hive..."));
    append_menu(file_menu, MF_STRING, cmd::kFileUnloadHive, util::Tr(L"Unload Hive..."));
    separator(file_menu);
    append_menu(file_menu, MF_STRING, cmd::kFileExit, util::Tr(L"Exit"));
    append_popup(menu, file_menu, util::Tr(L"&File"));

    HMENU edit_menu = CreatePopupMenu();
    append_menu(edit_menu, MF_STRING, cmd::kEditUndo, util::Tr(L"Undo"));
    append_menu(edit_menu, MF_STRING, cmd::kEditRedo, util::Tr(L"Redo"));
    separator(edit_menu);
    append_popup(edit_menu, BuildNewMenu(), util::Tr(L"New"));
    separator(edit_menu);
    append_menu(edit_menu, MF_STRING, cmd::kEditModify, util::Tr(L"Modify..."));
    append_menu(edit_menu, MF_STRING, cmd::kEditModifyBinary, util::Tr(L"Modify Binary Data..."));
    append_menu(edit_menu, MF_STRING, cmd::kEditChangeType, util::Tr(L"Change Data Type..."));
    append_menu(edit_menu, MF_STRING, cmd::kEditBits, util::Tr(L"Edit Bits..."));
    append_menu(edit_menu, MF_STRING, cmd::kEditDecodeValue, util::Tr(L"Decode Value..."));
    append_menu(edit_menu, MF_STRING, cmd::kEditModifyComment, util::Tr(L"Modify Comment..."));
    append_menu(edit_menu, MF_STRING, cmd::kEditResetDefault, util::Tr(L"Reset to Default"));
    separator(edit_menu);
    append_menu(edit_menu, MF_STRING, cmd::kEditCopy, util::Tr(L"Copy"));
    HMENU copy_other_menu = CreatePopupMenu();
    append_menu(copy_other_menu, MF_STRING, cmd::kEditCopyKey, util::Tr(L"Key Name"));
    append_menu(copy_other_menu, MF_STRING, cmd::kEditCopyKeyPath, util::Tr(L"Key Path"));
    append_popup(copy_other_menu, BuildCopyKeyPathMenu(), util::Tr(L"Key Path As"));
    separator(copy_other_menu);
    append_menu(copy_other_menu, MF_STRING, cmd::kEditCopyValueName, util::Tr(L"Value Name"));
    append_menu(copy_other_menu, MF_STRING, cmd::kEditCopyValueData, util::Tr(L"Value Data"));
    append_popup(edit_menu, copy_other_menu, util::Tr(L"Copy Other"));
    append_menu(edit_menu, MF_STRING, cmd::kEditPaste, util::Tr(L"Paste"));
    append_menu(edit_menu, MF_STRING, cmd::kEditRename, util::Tr(L"Rename"));
    append_menu(edit_menu, MF_STRING, cmd::kEditDelete, util::Tr(L"Delete"));
    separator(edit_menu);
    append_menu(edit_menu, MF_STRING, cmd::kViewSelectAll, util::Tr(L"Select All"));
    append_menu(edit_menu, MF_STRING, cmd::kEditInvertSelection, util::Tr(L"Invert Selection"));
    separator(edit_menu);
    append_menu(edit_menu, MF_STRING, cmd::kEditFind, util::Tr(L"Find..."));
    append_menu(edit_menu, MF_STRING, cmd::kEditReplace, util::Tr(L"Replace..."));
    append_menu(edit_menu, MF_STRING, cmd::kEditGoTo, util::Tr(L"Go To..."));
    if (win32::HasAlternateView())
    {
        append_menu(edit_menu, MF_STRING, cmd::kRegistryOtherView, util::Tr(L"Go to Other View"));
    }
    append_menu(edit_menu, MF_STRING, cmd::kEditGoToVirtualStore, util::Tr(L"Go to Virtual Store Copy"));
    append_menu(edit_menu, MF_STRING, cmd::kEditGoToGlobalKey, util::Tr(L"Go to Global Key"));
    separator(edit_menu);
    append_menu(edit_menu, MF_STRING, cmd::kEditPermissions, util::Tr(L"Permissions..."));
    append_menu(edit_menu, MF_STRING, cmd::kEditKeyInfo, util::Tr(L"Key Information..."));
    append_popup(menu, edit_menu, util::Tr(L"&Edit"));

    HMENU view_menu = CreatePopupMenu();
    append_menu(view_menu, MF_STRING, cmd::kViewRefresh, util::Tr(L"Refresh"));
    append_menu(view_menu, MF_STRING, cmd::kViewAutoRefresh, util::Tr(L"Auto Refresh"));
    separator(view_menu);
    AppendMenuW(view_menu, MF_STRING, cmd::kViewToolbar, util::Tr(L"Toolbar"));
    AppendMenuW(view_menu, MF_STRING, cmd::kViewAddressBar, util::Tr(L"Address Bar"));
    AppendMenuW(view_menu, MF_STRING, cmd::kViewFilterBar, util::Tr(L"Filter Bar"));
    AppendMenuW(view_menu, MF_STRING, cmd::kViewTabControl, util::Tr(L"Tabs"));
    AppendMenuW(view_menu, MF_STRING, cmd::kViewStatusBar, util::Tr(L"Status Bar"));
    separator(view_menu);
    AppendMenuW(view_menu, MF_STRING, cmd::kViewKeyTree, util::Tr(L"Key Tree"));
    append_menu(view_menu, MF_STRING, cmd::kViewHistory, util::Tr(L"History"));
    separator(view_menu);
    AppendMenuW(view_menu, MF_STRING, cmd::kViewKeysInList, util::Tr(L"Keys in List"));
    AppendMenuW(view_menu, MF_STRING, cmd::kViewGridLines, util::Tr(L"Grid Lines"));
    AppendMenuW(view_menu, MF_STRING, cmd::kViewSimulatedKeys, util::Tr(L"Simulated Keys"));
    AppendMenuW(view_menu, MF_STRING, cmd::kViewExtraHives, util::Tr(L"Show Extra Root Keys"));
    append_popup(menu, view_menu, util::Tr(L"&View"));

    HMENU options_menu = CreatePopupMenu();
    HMENU theme_menu = CreatePopupMenu();
    AppendMenuW(theme_menu, MF_STRING, cmd::kOptionsThemeSystem, util::Tr(L"System"));
    AppendMenuW(theme_menu, MF_STRING, cmd::kOptionsThemeLight, util::Tr(L"Light"));
    AppendMenuW(theme_menu, MF_STRING, cmd::kOptionsThemeDark, util::Tr(L"Dark"));
    AppendMenuW(theme_menu, MF_STRING, cmd::kOptionsThemeCustom, util::Tr(L"Custom"));
    separator(theme_menu);
    AppendMenuW(theme_menu, MF_STRING, cmd::kOptionsThemePresets, util::Tr(L"Theme Presets..."));
    append_popup(options_menu, theme_menu, util::Tr(L"Theme"));
    HMENU icon_menu = CreatePopupMenu();
    AppendMenuW(icon_menu, MF_STRING, cmd::kOptionsIconSetClassic, util::Tr(L"Classic"));
    AppendMenuW(icon_menu, MF_STRING, cmd::kOptionsIconSetPhosphor, util::Tr(L"Phosphor"));
    AppendMenuW(icon_menu, MF_STRING, cmd::kOptionsIconSetCustom, util::Tr(L"Custom"));
    append_popup(options_menu, icon_menu, util::Tr(L"Icons"));
    HMENU language_menu = CreatePopupMenu();
    language_packs_ = util::InstalledLanguages();
    language_packs_.insert(language_packs_.begin(), {L"en", L"English"});
    AppendMenuW(language_menu, MF_STRING, cmd::kOptionsLanguageAuto, util::Tr(L"Automatic"));
    separator(language_menu);
    for (size_t i = 0; i < language_packs_.size() && cmd::kOptionsLanguageBase + static_cast<int>(i) <= cmd::kOptionsLanguageMax; ++i)
    {
        AppendMenuW(language_menu, MF_STRING, cmd::kOptionsLanguageBase + static_cast<int>(i), language_packs_[i].name.c_str());
    }
    append_popup(options_menu, language_menu, util::Tr(L"Language"));
    AppendMenuW(options_menu, MF_STRING, cmd::kViewFont, util::Tr(L"Font..."));
    separator(options_menu);
    AppendMenuW(options_menu, MF_STRING, cmd::kOptionsReadOnly, util::Tr(L"Read Only Mode"));
    AppendMenuW(options_menu, MF_STRING | (util::IsProcessPrivileged() ? 0 : MF_GRAYED), cmd::kOptionsBackupRestore, util::Tr(L"Use Backup/Restore Privileges"));
    separator(options_menu);
    const bool is_elevated = util::IsProcessElevated();
    const bool is_system = util::IsProcessSystem();
    const bool is_ti = util::IsProcessTrustedInstaller();
    const bool is_high = is_system || is_ti;
    HMENU run_as_menu = CreatePopupMenu();
    append_menu(run_as_menu, MF_STRING, cmd::kFileRestart, util::Tr(L"Restart"));
    AppendMenuW(run_as_menu, MF_STRING | (((is_high || is_elevated) && util::IsUacEnabled()) ? 0 : MF_GRAYED), cmd::kOptionsRestartUser, util::Tr(L"Restart as User"));
    AppendMenuW(run_as_menu, MF_STRING | ((is_elevated && !is_high) ? MF_GRAYED : 0), cmd::kOptionsRestartAdmin, util::Tr(L"Restart as Admin"));
    AppendMenuW(run_as_menu, MF_STRING | (is_system ? MF_GRAYED : 0), cmd::kOptionsRestartSystem, util::Tr(L"Restart as SYSTEM"));
    AppendMenuW(run_as_menu, MF_STRING | (is_ti ? MF_GRAYED : 0), cmd::kOptionsRestartTrustedInstaller, util::Tr(L"Restart as TrustedInstaller"));
    separator(run_as_menu);
    AppendMenuW(run_as_menu, MF_STRING, cmd::kOptionsAlwaysRunAdmin, util::Tr(L"Always Run as Admin"));
    AppendMenuW(run_as_menu, MF_STRING, cmd::kOptionsAlwaysRunSystem, util::Tr(L"Always Run as SYSTEM"));
    AppendMenuW(run_as_menu, MF_STRING, cmd::kOptionsAlwaysRunTrustedInstaller, util::Tr(L"Always Run as TrustedInstaller"));
    separator(run_as_menu);
    AppendMenuW(run_as_menu, MF_STRING, cmd::kOptionsHkcuFollowsUser, util::Tr(L"HKCU Follows Signed-In User"));
    append_popup(options_menu, run_as_menu, util::Tr(L"Run As"));
    separator(options_menu);
    AppendMenuW(options_menu, MF_STRING | ((is_elevated || is_high) ? 0 : MF_GRAYED), cmd::kOptionsReplaceRegEdit, util::Tr(L"Replace RegEdit"));
    AppendMenuW(options_menu, MF_STRING | (is_high ? MF_GRAYED : 0), cmd::kOptionsEditContextMenu, util::Tr(L"Add \"Edit\" Context Menu"));
    AppendMenuW(options_menu, MF_STRING, cmd::kOptionsSingleInstance, util::Tr(L"Single Instance"));
    AppendMenuW(options_menu, MF_STRING, cmd::kOptionsAutoComplete, util::Tr(L"Autocomplete Key Paths"));
    HMENU save_tabs_menu = CreatePopupMenu();
    AppendMenuW(save_tabs_menu, MF_STRING, cmd::kOptionsSaveTabs, util::Tr(L"All Tabs"));
    separator(save_tabs_menu);
    AppendMenuW(save_tabs_menu, MF_STRING, cmd::kOptionsSaveTabsLocal, util::Tr(L"Local Registry Tabs"));
    AppendMenuW(save_tabs_menu, MF_STRING, cmd::kOptionsSaveTabsOffline, util::Tr(L"Offline Hive Tabs"));
    AppendMenuW(save_tabs_menu, MF_STRING, cmd::kOptionsSaveTabsRemote, util::Tr(L"Remote Registry Tabs"));
    separator(save_tabs_menu);
    AppendMenuW(save_tabs_menu, MF_STRING, cmd::kOptionsSaveTabsSearch, util::Tr(L"Find Results"));
    AppendMenuW(save_tabs_menu, MF_STRING, cmd::kOptionsSaveTabsCompare, util::Tr(L"Comparison Results"));
    AppendMenuW(save_tabs_menu, MF_STRING, cmd::kOptionsSaveTabsRegFile, util::Tr(L".reg File Tabs"));
    separator(options_menu);
    append_popup(options_menu, save_tabs_menu, util::Tr(L"Save Tabs"));
    AppendMenuW(options_menu, MF_STRING, cmd::kViewSaveTreeState, util::Tr(L"Save Previous Tree State"));
    append_menu(options_menu, MF_STRING, cmd::kFileClearHistoryOnExit, util::Tr(L"Clear History on Exit"));
    append_menu(options_menu, MF_STRING, cmd::kFileClearTabsOnExit, util::Tr(L"Clear Tabs on Exit"));
    separator(options_menu);
    HMENU clear_cache_menu = CreatePopupMenu();
    AppendMenuW(clear_cache_menu, MF_STRING, cmd::kFileClearCacheAll, util::Tr(L"Clear All"));
    separator(clear_cache_menu);
    AppendMenuW(clear_cache_menu, MF_STRING, cmd::kFileClearCacheTabs, util::Tr(L"Tab Sessions"));
    AppendMenuW(clear_cache_menu, MF_STRING, cmd::kFileClearCacheHistory, util::Tr(L"Change History"));
    AppendMenuW(clear_cache_menu, MF_STRING, cmd::kFileClearCacheSearchHistory, util::Tr(L"Search History"));
    AppendMenuW(clear_cache_menu, MF_STRING, cmd::kFileClearCacheTreeState, util::Tr(L"Tree State"));
    AppendMenuW(clear_cache_menu, MF_STRING, cmd::kFileClearCacheTemporary, util::Tr(L"Temporary Files"));
    append_popup(options_menu, clear_cache_menu, util::Tr(L"Clear Caches"));
    AppendMenuW(options_menu, MF_STRING, cmd::kOptionsResetSettings, util::Tr(L"Reset Settings..."));
    HMENU favorites_menu = CreatePopupMenu();
    AppendMenuW(favorites_menu, MF_STRING, cmd::kFavoritesAdd, util::Tr(L"Add to Favorites..."));
    AppendMenuW(favorites_menu, MF_STRING, cmd::kFavoritesRemove, util::Tr(L"Remove Favorite"));
    AppendMenuW(favorites_menu, MF_STRING, cmd::kFavoritesEdit, util::Tr(L"Edit Favorites..."));
    separator(favorites_menu);
    append_menu(favorites_menu, MF_STRING, cmd::kFavoritesImport, util::Tr(L"Import Favorites..."));
    append_menu(favorites_menu, MF_STRING, cmd::kFavoritesImportRegEdit, util::Tr(L"Import RegEdit Favorites"));
    append_menu(favorites_menu, MF_STRING, cmd::kFavoritesExport, util::Tr(L"Export Favorites..."));
    if (!favorites_cache_.empty())
    {
        separator(favorites_menu);
        int limit =
            std::min(static_cast<int>(favorites_cache_.size()), cmd::kFavoritesItemMax - cmd::kFavoritesItemBase + 1);
        for (int i = 0; i < limit; ++i)
        {
            AppendMenuW(favorites_menu, MF_STRING, cmd::kFavoritesItemBase + i, favorites_cache_[static_cast<size_t>(i)].c_str());
        }
    }
    regedit_favorites_menu_ = favorites_menu;
    regedit_favorites_static_count_ = GetMenuItemCount(favorites_menu);
    regedit_favorites_.clear();
    append_popup(menu, favorites_menu, util::Tr(L"F&avorites"));
    append_popup(menu, options_menu, util::Tr(L"&Options"));

    HMENU tools_menu = CreatePopupMenu();
    append_menu(tools_menu, MF_STRING, cmd::kOptionsCompareRegistries, util::Tr(L"Compare Registries..."));
    append_menu(tools_menu, MF_STRING, cmd::kToolsConvertFile, util::Tr(L"Convert File..."));
    append_menu(tools_menu, MF_STRING, cmd::kToolsKeyHandles, util::Tr(L"Key Handles..."));
    HMENU bitfield_menu = CreatePopupMenu();
    AppendMenuW(bitfield_menu, MF_STRING, cmd::kToolsBitfieldDefinitions, util::Tr(L"New Definition File..."));
    append_popup(tools_menu, bitfield_menu, util::Tr(L"Bit Definitions"));
    append_popup(menu, tools_menu, util::Tr(L"&Tools"));

    HMENU window_menu = CreatePopupMenu();
    append_menu(window_menu, MF_STRING, cmd::kWindowNew, util::Tr(L"New Window"));
    separator(window_menu);
    append_menu(window_menu, MF_STRING, cmd::kTabClose, util::Tr(L"Close Tab"));
    AppendMenuW(window_menu, MF_STRING, cmd::kWindowClose, util::Tr(L"Close Window"));
    separator(window_menu);
    AppendMenuW(window_menu, MF_STRING, cmd::kWindowAlwaysOnTop, util::Tr(L"Always on Top"));
    append_popup(menu, window_menu, util::Tr(L"&Window"));

    HMENU trace_menu = CreatePopupMenu();
    append_menu(trace_menu, MF_STRING, cmd::kTraceLoad23H2, util::Tr(L"23H2"));
    append_menu(trace_menu, MF_STRING, cmd::kTraceLoad24H2, util::Tr(L"24H2"));
    append_menu(trace_menu, MF_STRING, cmd::kTraceLoad25H2, util::Tr(L"25H2"));
    bool has_recent_trace = false;
    int recent_limit = std::min(static_cast<int>(recent_trace_paths_.items().size()), cmd::kTraceRecentMax - cmd::kTraceRecentBase + 1);
    for (int i = 0; i < recent_limit; ++i)
    {
        const std::wstring& path = recent_trace_paths_.items()[static_cast<size_t>(i)];
        if (path.empty())
        {
            continue;
        }
        if (!has_recent_trace)
        {
            separator(trace_menu);
        }
        has_recent_trace = true;
        std::wstring name = util::FileName(path);
        if (name.empty())
        {
            name = util::Tr(L"Trace");
        }
        append_menu(trace_menu, MF_STRING, cmd::kTraceRecentBase + i, name.c_str());
    }
    separator(trace_menu);
    append_menu(trace_menu, MF_STRING, cmd::kTraceLoadCustom, util::Tr(L"Open Trace File..."));
    separator(trace_menu);
    append_menu(trace_menu, MF_STRING, cmd::kTraceEditActive, util::Tr(L"Edit Active Traces..."));
    append_menu(trace_menu, MF_STRING, cmd::kTraceClear, util::Tr(L"Clear Active Traces"));
    separator(trace_menu);
    append_menu(trace_menu, MF_STRING | (has_recent_trace ? 0 : MF_GRAYED), cmd::kTraceEditRecent, util::Tr(L"Edit Recent Traces..."));
    append_menu(trace_menu, MF_STRING | (has_recent_trace ? 0 : MF_GRAYED), cmd::kTraceClearRecent, util::Tr(L"Clear Recent Traces"));
    separator(trace_menu);
    append_menu(trace_menu, MF_STRING, cmd::kTraceGuide, util::Tr(L"Guide"));

    HMENU default_menu = CreatePopupMenu();
    HMENU bundled_menu = nullptr;
    std::wstring bundled_group;
    for (size_t i = 0; i < bundled_defaults_.size(); ++i)
    {
        const auto& entry = bundled_defaults_[i];
        if (!bundled_menu || entry.group != bundled_group)
        {
            bundled_group = entry.group;
            bundled_menu = CreatePopupMenu();
            append_popup(default_menu, bundled_menu, bundled_group.c_str());
        }
        append_menu(bundled_menu, MF_STRING, cmd::kDefaultBundledBase + static_cast<int>(i), entry.label.c_str());
    }
    bool has_recent_default = false;
    int default_recent_limit = std::min(static_cast<int>(recent_default_paths_.items().size()), cmd::kDefaultRecentMax - cmd::kDefaultRecentBase + 1);
    for (int i = 0; i < default_recent_limit; ++i)
    {
        const std::wstring& path = recent_default_paths_.items()[static_cast<size_t>(i)];
        if (path.empty())
        {
            continue;
        }
        if (!has_recent_default && !bundled_defaults_.empty())
        {
            separator(default_menu);
        }
        has_recent_default = true;
        std::wstring name = util::FileBaseName(path);
        const std::wstring build = defaults::ShortLabel(std::wstring(), path);
        if (!build.empty())
        {
            name = build + L" - " + name;
        }
        if (name.empty())
        {
            name = util::Tr(L"Default");
        }
        append_menu(default_menu, MF_STRING, cmd::kDefaultRecentBase + i, name.c_str());
    }
    separator(default_menu);
    append_menu(default_menu, MF_STRING, cmd::kDefaultLoadCustom, util::Tr(L"Open Default File..."));
    separator(default_menu);
    append_menu(default_menu, MF_STRING, cmd::kDefaultEditActive, util::Tr(L"Edit Active Defaults..."));
    append_menu(default_menu, MF_STRING, cmd::kDefaultClear, util::Tr(L"Clear Active Defaults"));
    separator(default_menu);
    append_menu(default_menu, MF_STRING | (has_recent_default ? 0 : MF_GRAYED), cmd::kDefaultEditRecent, util::Tr(L"Edit Recent Defaults..."));
    append_menu(default_menu, MF_STRING | (has_recent_default ? 0 : MF_GRAYED), cmd::kDefaultClearRecent, util::Tr(L"Clear Recent Defaults"));
    separator(default_menu);
    append_menu(default_menu, MF_STRING, cmd::kDefaultResetEnable, util::Tr(L"Enable Context Menu (risky)"));

    HMENU help_menu = CreatePopupMenu();
    append_menu(help_menu, MF_STRING, cmd::kHelpContents, util::Tr(L"Documentation"));
    AppendMenuW(help_menu, MF_STRING, cmd::kHelpDiscord, L"Discord");
    separator(help_menu);
    AppendMenuW(help_menu, MF_STRING, cmd::kHelpCheckUpdates, util::Tr(L"Check for Updates"));
    AppendMenuW(help_menu, MF_STRING, cmd::kHelpAutoCheckUpdates, util::Tr(L"Check for Updates Automatically"));
    separator(help_menu);
    AppendMenuW(help_menu, MF_STRING, cmd::kHelpAbout, util::Tr(L"About RegKit"));

    HMENU research_menu = CreatePopupMenu();
    int research_command = cmd::kResearchItemBase;
    for (const auto& link : frame::ResearchLinks())
    {
        AppendMenuW(research_menu, MF_STRING, research_command++, link.name);
        if (link.separator_after)
        {
            separator(research_menu);
        }
    }

    append_popup(menu, trace_menu, util::Tr(L"T&race"));
    append_popup(menu, default_menu, util::Tr(L"Defa&ult"));
    append_popup(menu, research_menu, util::Tr(L"Re&search"));
    append_popup(menu, help_menu, util::Tr(L"&Help"));

    PrepareMenusForOwnerDraw(menu);

    HMENU old_menu = GetMenu(hwnd_);
    SetMenu(hwnd_, menu);
    DrawMenuBar(hwnd_);
    if (old_menu)
    {
        DestroyMenu(old_menu);
    }
}

void MainWindow::Impl::UpdateMenuState(HMENU menu)
{
    auto check = [menu](int id, bool on) { CheckMenuItem(menu, static_cast<UINT>(id), MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    auto enable = [menu](int id, bool on) { EnableMenuItem(menu, static_cast<UINT>(id), MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    RefreshStorageMenuState(menu);
    FillBitfieldMenu(menu);
    RefreshResetDefaultMenu(menu);
    const RegistryNode* node = browse_.current_node();
    check(cmd::kViewGridLines, settings_.show_value_grid);
    enable(cmd::kEditPermissions, node != nullptr);
    enable(cmd::kEditKeyInfo, node != nullptr);
    enable(cmd::kEditModifyComment, node != nullptr);
    const HWND values = browse_.values().hwnd();
    const int selected_index = values && ListView_GetSelectedCount(values) == 1 ? ListView_GetNextItem(values, -1, LVNI_SELECTED) : -1;
    const ListRow* selected_row = selected_index >= 0 ? browse_.values().RowAt(selected_index) : nullptr;
    const bool can_open_value = selected_row && selected_row->kind == rowkind::kValue && !selected_row->simulated;
    const bool can_edit_data = can_open_value && !selected_row->missing;
    enable(cmd::kEditModify, can_open_value);
    enable(cmd::kEditModifyBinary, can_open_value);
    enable(cmd::kEditCopyValueName, can_open_value);
    enable(cmd::kEditChangeType, can_edit_data);
    enable(cmd::kEditDecodeValue, can_edit_data);
    enable(cmd::kEditBits, can_edit_data);
    enable(cmd::kEditCopyValueData, can_edit_data);
    const bool hives_allowed = !settings_.read_only && session_->mode != RegistryMode::kRemote;
    enable(cmd::kFileLoadHive, hives_allowed);
    enable(cmd::kFileUnloadHive, hives_allowed && node && IsMountedHive(node->root, node->subkey));
    if (GetMenuState(menu, cmd::kOptionsHiveFileDir, MF_BYCOMMAND) != static_cast<UINT>(-1))
    {
        enable(cmd::kOptionsHiveFileDir, !ResolveSelectedHiveFilePath().empty());
    }
    if (!IsMenuInside(GetMenu(hwnd_), menu))
    {
        return;
    }

    const bool can_modify = !settings_.read_only;
    const RegistryMode mode = session_->mode;
    for (int id : {cmd::kFileImport, cmd::kEditUndo, cmd::kEditRedo, cmd::kEditPaste, cmd::kEditReplace})
    {
        enable(id, can_modify);
    }
    const bool value_focused = can_open_value && GetFocus() == values;
    enable(cmd::kEditRename, can_modify && !(value_focused && (selected_row->extra.empty() || selected_row->missing)));
    enable(cmd::kEditDelete, can_modify && !(value_focused && selected_row->missing));
    const int tab_index = tab_ ? TabCtrl_GetCurSel(tab_) : -1;
    const TabEntry* tab = tab_index >= 0 && static_cast<size_t>(tab_index) < tabs_.size() ? &tabs_[static_cast<size_t>(tab_index)] : nullptr;
    const bool dirty = tab && (tab->kind == TabEntry::Kind::kRegFile ? tab->reg_file_dirty : tab->kind == TabEntry::Kind::kRegistry && tab->session && tab->session->offline_dirty);
    enable(cmd::kFileSave, can_modify && dirty);
    check(cmd::kRegistryLocal, mode == RegistryMode::kLocal && !session_->view);
    check(cmd::kRegistryLocal32, mode == RegistryMode::kLocal && session_->view);
    enable(cmd::kRegistryOtherView, mode == RegistryMode::kLocal && node != nullptr);
    if (GetMenuState(menu, cmd::kEditGoToVirtualStore, MF_BYCOMMAND) != static_cast<UINT>(-1))
    {
        enable(cmd::kEditGoToVirtualStore, !VirtualStoreTarget().empty());
        enable(cmd::kEditGoToGlobalKey, !GlobalKeyTarget().empty());
    }
    check(cmd::kRegistryNetwork, mode == RegistryMode::kRemote);
    check(cmd::kRegistryOffline, mode == RegistryMode::kOffline);
    enable(cmd::kFileSaveOfflineHive, mode == RegistryMode::kOffline && !session_->offline_mount.empty());
    enable(cmd::kNewVolatileKey, mode != RegistryMode::kOffline);
    const int new_position = SubMenuPosition(menu, cmd::kNewKey);
    if (new_position >= 0)
    {
        EnableMenuItem(menu, static_cast<UINT>(new_position), MF_BYPOSITION | (can_modify ? MF_ENABLED : MF_GRAYED));
    }

    check(cmd::kViewAutoRefresh, settings_.auto_refresh);
    check(cmd::kViewToolbar, settings_.show_toolbar);
    check(cmd::kViewAddressBar, settings_.show_address_bar);
    check(cmd::kViewFilterBar, settings_.show_filter_bar);
    check(cmd::kViewTabControl, settings_.show_tab_control);
    check(cmd::kViewStatusBar, settings_.show_status_bar);
    check(cmd::kViewKeyTree, settings_.show_tree);
    check(cmd::kViewHistory, settings_.show_history);
    check(cmd::kViewKeysInList, settings_.show_keys_in_list);
    check(cmd::kViewSimulatedKeys, settings_.show_simulated_keys);
    enable(cmd::kViewSimulatedKeys, HasActiveTraces());
    check(cmd::kViewExtraHives, settings_.show_extra_hives);
    enable(cmd::kViewExtraHives, mode == RegistryMode::kLocal);

    check(cmd::kOptionsThemeSystem, theme_mode_ == ThemeMode::kSystem);
    check(cmd::kOptionsThemeLight, theme_mode_ == ThemeMode::kLight);
    check(cmd::kOptionsThemeDark, theme_mode_ == ThemeMode::kDark);
    check(cmd::kOptionsThemeCustom, theme_mode_ == ThemeMode::kCustom);
    check(cmd::kOptionsIconSetClassic, util::EqualsInsensitive(settings_.icon_set, kIconSetClassic));
    check(cmd::kOptionsIconSetPhosphor, util::EqualsInsensitive(settings_.icon_set, kIconSetPhosphor));
    check(cmd::kOptionsIconSetCustom, util::EqualsInsensitive(settings_.icon_set, kIconSetCustom));
    check(cmd::kOptionsLanguageAuto, settings_.language.empty());
    for (size_t i = 0; i < language_packs_.size(); ++i)
    {
        check(cmd::kOptionsLanguageBase + static_cast<int>(i), settings_.language == language_packs_[i].code);
    }
    check(cmd::kOptionsReadOnly, settings_.read_only);
    check(cmd::kOptionsBackupRestore, backup_privileges_ != nullptr);
    check(cmd::kOptionsAlwaysRunAdmin, settings_.always_run_as_admin);
    check(cmd::kOptionsAlwaysRunSystem, settings_.always_run_as_system);
    check(cmd::kOptionsAlwaysRunTrustedInstaller, settings_.always_run_as_trustedinstaller);
    check(cmd::kOptionsHkcuFollowsUser, settings_.hkcu_follows_shell_user);
    if (GetMenuState(menu, cmd::kOptionsReplaceRegEdit, MF_BYCOMMAND) != static_cast<UINT>(-1))
    {
        const std::wstring exe_path = util::GetModulePath();
        check(cmd::kOptionsReplaceRegEdit, win32::IsRegEditReplacementRegistered(exe_path));
        check(cmd::kOptionsEditContextMenu, win32::IsEditMenuRegistered(exe_path));
    }
    check(cmd::kOptionsSingleInstance, settings_.single_instance);
    check(cmd::kOptionsAutoComplete, settings_.autocomplete);
    const int kinds = settings_.save_tab_kinds;
    check(cmd::kOptionsSaveTabs, (kinds & workspace::kSaveTabsAll) == workspace::kSaveTabsAll);
    check(cmd::kOptionsSaveTabsLocal, (kinds & workspace::kSaveTabsLocal) != 0);
    check(cmd::kOptionsSaveTabsOffline, (kinds & workspace::kSaveTabsOffline) != 0);
    check(cmd::kOptionsSaveTabsRemote, (kinds & workspace::kSaveTabsRemote) != 0);
    check(cmd::kOptionsSaveTabsSearch, (kinds & workspace::kSaveTabsSearch) != 0);
    check(cmd::kOptionsSaveTabsCompare, (kinds & workspace::kSaveTabsCompare) != 0);
    check(cmd::kOptionsSaveTabsRegFile, (kinds & workspace::kSaveTabsRegFile) != 0);
    const int save_tabs_position = SubMenuPosition(menu, cmd::kOptionsSaveTabs);
    if (save_tabs_position >= 0)
    {
        CheckMenuItem(menu, static_cast<UINT>(save_tabs_position), MF_BYPOSITION | (kinds != 0 ? MF_CHECKED : MF_UNCHECKED));
    }
    check(cmd::kViewSaveTreeState, settings_.save_tree_state);
    check(cmd::kFileClearHistoryOnExit, settings_.clear_history_on_exit);
    check(cmd::kFileClearTabsOnExit, settings_.clear_tabs_on_exit);
    enable(cmd::kWindowNew, !settings_.single_instance);
    check(cmd::kWindowAlwaysOnTop, settings_.always_on_top);
    check(cmd::kHelpAutoCheckUpdates, settings_.auto_check_updates);

    auto trace_active = [&](auto match) { return std::any_of(active_traces_.begin(), active_traces_.end(), match); };
    check(cmd::kTraceLoad23H2, trace_active([](const auto& trace) { return util::EqualsInsensitive(trace.label, L"23H2"); }));
    check(cmd::kTraceLoad24H2, trace_active([](const auto& trace) { return util::EqualsInsensitive(trace.label, L"24H2"); }));
    check(cmd::kTraceLoad25H2, trace_active([](const auto& trace) { return util::EqualsInsensitive(trace.label, L"25H2"); }));
    for (size_t i = 0; i < recent_trace_paths_.items().size() && cmd::kTraceRecentBase + static_cast<int>(i) <= cmd::kTraceRecentMax; ++i)
    {
        const std::wstring& path = recent_trace_paths_.items()[i];
        check(cmd::kTraceRecentBase + static_cast<int>(i), trace_active([&](const auto& trace) { return EqualsInsensitive(trace.source_path, path); }));
    }
    enable(cmd::kTraceClear, !active_traces_.empty());

    auto default_active = [&](const std::wstring& path) {
        return std::any_of(active_defaults_.begin(), active_defaults_.end(), [&](const auto& defaults) { return EqualsInsensitive(defaults.source_path, path); });
    };
    for (size_t i = 0; i < bundled_defaults_.size(); ++i)
    {
        check(cmd::kDefaultBundledBase + static_cast<int>(i), default_active(bundled_defaults_[i].path));
    }
    for (size_t i = 0; i < recent_default_paths_.items().size() && cmd::kDefaultRecentBase + static_cast<int>(i) <= cmd::kDefaultRecentMax; ++i)
    {
        check(cmd::kDefaultRecentBase + static_cast<int>(i), default_active(recent_default_paths_.items()[i]));
    }
    enable(cmd::kDefaultClear, !active_defaults_.empty());
    check(cmd::kDefaultResetEnable, settings_.default_reset_enabled);
}

void MainWindow::Impl::RefreshFavoritesCache()
{
    favorites_cache_.clear();
    FavoritesStore::Load(&favorites_cache_);
    favorites_loaded_ = true;
}

void MainWindow::Impl::RefreshRegEditFavoritesMenu()
{
    if (!regedit_favorites_menu_)
    {
        return;
    }
    while (GetMenuItemCount(regedit_favorites_menu_) > regedit_favorites_static_count_)
    {
        DeleteMenu(regedit_favorites_menu_, regedit_favorites_static_count_, MF_BYPOSITION);
    }
    regedit_favorites_.clear();
    FavoritesStore::LoadRegEdit(&regedit_favorites_);
    const size_t limit = std::min(regedit_favorites_.size(), static_cast<size_t>(cmd::kRegEditFavoriteMax - cmd::kRegEditFavoriteBase + 1));
    if (limit == 0)
    {
        return;
    }
    AppendMenuW(regedit_favorites_menu_, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < limit; ++i)
    {
        AppendMenuW(regedit_favorites_menu_, MF_STRING, cmd::kRegEditFavoriteBase + static_cast<UINT>(i), regedit_favorites_[i].name.c_str());
    }
    regedit_favorites_.resize(limit);
}

void MainWindow::Impl::RefreshBundledDefaultsCache()
{
    bundled_defaults_.clear();
    bundled_defaults_loaded_ = true;

    std::wstring module_dir = util::GetModuleDirectory();
    if (module_dir.empty())
    {
        return;
    }
    std::wstring assets = util::JoinPath(module_dir, L"assets");
    std::wstring defaults_dir = util::JoinPath(assets, L"defaults");
    std::error_code error;
    const std::filesystem::path root(defaults_dir);
    for (const auto& folder :
         std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, error))
    {
        if (!folder.is_directory(error))
        {
            error.clear();
            continue;
        }
        for (const auto& file : std::filesystem::directory_iterator(
                 folder.path(),
                 std::filesystem::directory_options::skip_permission_denied,
                 error
             ))
        {
            if (!file.is_regular_file(error) || !util::EqualsInsensitive(file.path().extension().c_str(), L".reg"))
            {
                error.clear();
                continue;
            }
            BundledDefault entry;
            entry.group = folder.path().filename().wstring();
            entry.label = file.path().stem().wstring();
            entry.path = file.path().wstring();
            bundled_defaults_.push_back(std::move(entry));
        }
        error.clear();
    }

    std::sort(bundled_defaults_.begin(), bundled_defaults_.end(), [](const BundledDefault& left, const BundledDefault& right) {
        const int group = util::CompareInsensitive(left.group, right.group);
        return group != 0 ? group < 0 : util::CompareInsensitive(left.label, right.label) < 0;
    });
    size_t bundled_limit =
        std::min(bundled_defaults_.size(), static_cast<size_t>(cmd::kDefaultBundledMax - cmd::kDefaultBundledBase + 1));
    if (bundled_defaults_.size() > bundled_limit)
    {
        bundled_defaults_.resize(bundled_limit);
    }
}

bool MainWindow::Impl::HandleMenuCommand(int command_id)
{
    return HandleDynamicCommand(command_id) || HandleFileCommand(command_id) || HandleViewCommand(command_id) ||
           HandleTraceDefaultCommand(command_id) || HandleWorkspaceAppearanceCommand(command_id) ||
           HandleNavigateClipboardCommand(command_id) || HandleMutationCommand(command_id) || HandleToolsCommand(command_id);
}

std::vector<MainWindow::Impl::DefaultValueChoice> MainWindow::Impl::SelectedValueDefaultChoices() const
{
    if (!settings_.default_reset_enabled || settings_.read_only || active_defaults_.empty())
    {
        return {};
    }
    std::vector<ListRow> rows = SelectedListRows(browse_.values());
    if (rows.size() != 1 || rows.front().kind != rowkind::kValue || rows.front().simulated)
    {
        return {};
    }
    return CollectDefaultChoices(rows.front().extra);
}

HMENU MainWindow::Impl::BuildResetDefaultMenu(const std::vector<DefaultValueChoice>& choices) const
{
    HMENU menu = CreatePopupMenu();
    const int limit = cmd::kResetDefaultMax - cmd::kResetDefaultBase + 1;
    for (size_t i = 0; i < choices.size() && static_cast<int>(i) < limit; ++i)
    {
        std::wstring text = choices[i].label.empty() ? std::wstring(util::Tr(L"Default")) : choices[i].label;
        if (!choices[i].present)
        {
            text.append(L" ").append(util::Tr(L"(Missing)"));
        }
        AppendMenuW(menu, MF_STRING, cmd::kResetDefaultBase + static_cast<int>(i), text.c_str());
    }
    return menu;
}

void MainWindow::Impl::RefreshResetDefaultMenu(HMENU menu)
{
    const int count = GetMenuItemCount(menu);
    for (int position = 0; position < count; ++position)
    {
        MENUITEMINFOW item = {};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_ID;
        if (!GetMenuItemInfoW(menu, position, TRUE, &item) || item.wID != static_cast<UINT>(cmd::kEditResetDefault))
        {
            continue;
        }
        const std::vector<DefaultValueChoice> choices = SelectedValueDefaultChoices();
        item.fMask = MIIM_STATE | MIIM_SUBMENU;
        item.hSubMenu = choices.size() < 2 ? nullptr : BuildResetDefaultMenu(choices);
        item.fState = choices.empty() ? MFS_GRAYED : MFS_ENABLED;
        SetMenuItemInfoW(menu, position, TRUE, &item);
        return;
    }
}

} // namespace regkit
