// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/commands/command_detail.h"
#include "frame/window_impl.h"
#include "win32/translation.h"

namespace regkit
{
using namespace command_detail;

bool MainWindow::Impl::HandleFavoritesCommand(int command_id)
{
    switch (command_id)
    {
    case cmd::kFavoritesAdd:
        {
            if (browse_.current_node())
            {
                if (FavoritesStore::Add(registry_path::Build(*browse_.current_node())))
                {
                    RefreshFavoritesCache();
                    BuildMenus();
                }
            }
            return true;
        }
    case cmd::kFavoritesRemove:
        {
            if (browse_.current_node())
            {
                if (FavoritesStore::Remove(registry_path::Build(*browse_.current_node())))
                {
                    RefreshFavoritesCache();
                    BuildMenus();
                }
            }
            return true;
        }
    case cmd::kFavoritesEdit:
        {
            if (!favorites_loaded_)
            {
                RefreshFavoritesCache();
            }
            std::wstring content = JoinLines(favorites_cache_);
            editors::TextRequest request;
            request.title = util::Tr(L"Edit Favorites");
            request.label = util::Tr(L"Each line should include one key.");
            request.text = content;
            request.multiline = true;
            editors::TextResult result;
            if (editors::EditText(hwnd_, request, &result))
            {
                content = std::move(result.text);
                std::vector<std::wstring> updated = SplitLines(content);
                if (!FavoritesStore::Save(updated))
                {
                    ui::ShowError(hwnd_, util::Tr(L"Failed to save favorites."));
                    return true;
                }
                favorites_cache_ = std::move(updated);
                favorites_loaded_ = true;
                BuildMenus();
            }
            return true;
        }
    case cmd::kFavoritesImport:
        {
            std::wstring path;
            if (!ui::PromptOpenFile(hwnd_, L"Favorites Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0\0", &path))
            {
                return true;
            }
            if (!FavoritesStore::ImportFromFile(path))
            {
                ui::ShowError(hwnd_, util::Tr(L"Failed to import favorites."));
            }
            else
            {
                RefreshFavoritesCache();
                AppendHistoryEntry(L"Import favorites " + util::FileName(path), L"", path);
            }
            BuildMenus();
            return true;
        }
    case cmd::kFavoritesImportRegEdit:
        {
            size_t imported = 0;
            std::wstring error;
            if (!FavoritesStore::ImportFromRegEdit(&imported, &error))
            {
                ui::ShowError(hwnd_, error.empty() ? util::Tr(L"Failed to import RegEdit favorites.") : error);
                return true;
            }
            if (imported > 0)
            {
                RefreshFavoritesCache();
                BuildMenus();
            }
            AppendHistoryEntry(L"Import RegEdit favorites", L"", std::to_wstring(imported) + L" favorites");
            return true;
        }
    case cmd::kFavoritesExport:
        {
            std::wstring path;
            win32::OpenAfter open_after = win32::OpenAfter::kNone;
            if (!ui::PromptSaveFile(hwnd_, L"Favorites Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0\0", &path, &open_after))
            {
                return true;
            }
            if (!FavoritesStore::ExportToFile(path))
            {
                ui::ShowError(hwnd_, util::Tr(L"Failed to export favorites."));
            }
            else
            {
                AppendHistoryEntry(L"Export favorites " + util::FileName(path), L"", path);
                OpenSavedFile(path, open_after);
            }
            return true;
        }
    default:
        return false;
    }
}

RegistryNode MainWindow::Impl::SelectedKeyNode() const
{
    int index = -1;
    const ListRow* row = SelectedValueRow(browse_.values(), &index);
    return row && row->kind == rowkind::kKey && !row->extra.empty() ? ChildNode(*browse_.current_node(), row->extra) : *browse_.current_node();
}

bool MainWindow::Impl::HandleNavigateClipboardCommand(int command_id)
{
    switch (command_id)
    {
    case cmd::kEditCopyKey:
    case cmd::kEditCopyValueName:
    case cmd::kEditCopyValueData:
    case cmd::kEditCopyKeyPath:
    case cmd::kEditCopyKeyPathAbbrev:
    case cmd::kEditCopyKeyPathRegEdit:
    case cmd::kEditCopyKeyPathRegFile:
    case cmd::kEditCopyKeyPathPowerShell:
    case cmd::kEditCopyKeyPathPowerShellProvider:
    case cmd::kEditCopyKeyPathEscaped:
    case cmd::kEditCopyKeyPathNative:
    case cmd::kEditCopyKeyPathNativeResolved:
    case cmd::kEditCopy:
        return HandleClipboardCommand(command_id);
    case cmd::kEditGoTo:
    case cmd::kEditPermissions:
    case cmd::kEditKeyInfo:
    case cmd::kEditOpenSourceHive:
    case cmd::kEditFind:
        return HandleEditToolsCommand(command_id);
    case cmd::kEditPaste:
    case cmd::kEditReplace:
    case cmd::kEditUndo:
    case cmd::kEditRedo:
        return HandleChangeHistoryCommand(command_id);
    case cmd::kRegistryLocal:
    case cmd::kRegistryNetwork:
    case cmd::kRegistryOffline:
    case cmd::kNavBack:
    case cmd::kNavForward:
    case cmd::kNavUp:
        return HandleRegistryNavigationCommand(command_id);
    default:
        return false;
    }
}

bool MainWindow::Impl::HandleClipboardCommand(int command_id)
{
    switch (command_id)
    {
    case cmd::kEditCopyKey:
        {
            std::wstring name;
            int index = -1;
            const ListRow* row = SelectedValueRow(browse_.values(), &index);
            if (row && row->kind == rowkind::kKey)
            {
                name = row->name;
            }
            else if (browse_.current_node())
            {
                name = registry_path::DisplayName(LeafName(*browse_.current_node()));
            }
            if (!name.empty())
            {
                ui::CopyTextToClipboard(hwnd_, name);
            }
            return true;
        }
    case cmd::kEditCopyValueName:
        {
            std::vector<ListRow> selected_rows = SelectedListRows(browse_.values());
            if (selected_rows.size() != 1 || selected_rows.front().kind != rowkind::kValue)
            {
                return true;
            }
            const ListRow* row = &selected_rows.front();
            std::wstring name = row->extra.empty() ? L"(Default)" : row->extra;
            ui::CopyTextToClipboard(hwnd_, name);
            return true;
        }
    case cmd::kEditCopyValueData:
        {
            if (!browse_.current_node())
            {
                return true;
            }
            const ListRow* row = SelectedValueRow(browse_.values(), nullptr);
            if (!row || row->kind != rowkind::kValue)
            {
                return true;
            }
            if (row->simulated)
            {
                return true;
            }
            RegistryValue entry;
            if (!GetValueEntry(*browse_.current_node(), row->extra, &entry))
            {
                ui::ShowError(hwnd_, util::Tr(L"Failed to read value."));
                return true;
            }
            std::wstring data =
                value_format::DisplayData(entry.type, entry.data.data(), static_cast<DWORD>(entry.data.size()));
            ui::CopyTextToClipboard(hwnd_, data);
            return true;
        }
    case cmd::kEditCopyKeyPath:
    case cmd::kEditCopyKeyPathAbbrev:
    case cmd::kEditCopyKeyPathRegEdit:
    case cmd::kEditCopyKeyPathRegFile:
    case cmd::kEditCopyKeyPathPowerShell:
    case cmd::kEditCopyKeyPathPowerShellProvider:
    case cmd::kEditCopyKeyPathEscaped:
    case cmd::kEditCopyKeyPathNative:
    case cmd::kEditCopyKeyPathNativeResolved:
        {
            if (command_id == cmd::kEditCopyKeyPathNative || command_id == cmd::kEditCopyKeyPathNativeResolved)
            {
                if (!browse_.current_node())
                {
                    return true;
                }
                const RegistryNode node = SelectedKeyNode();
                // hkcr is a merged view, so only the opened key knows which hive it lives in
                std::wstring native = command_id == cmd::kEditCopyKeyPathNative ? registry_path::BuildNative(node) : std::wstring();
                KeyDetails details;
                if (native.empty() && RegistryStore::QueryKeyDetails(node, &details))
                {
                    native = std::move(details.native.native_name);
                }
                if (native.empty())
                {
                    native = registry_path::BuildNative(node);
                }
                if (!native.empty())
                {
                    ui::CopyTextToClipboard(hwnd_, registry_path::DisplayName(native));
                }
                return true;
            }
            auto build_path = [&]() -> std::wstring {
                std::wstring path;
                int index = -1;
                const ListRow* row = SelectedValueRow(browse_.values(), &index);
                if (row && row->kind == rowkind::kKey && browse_.current_node())
                {
                    path = registry_path::Build(*browse_.current_node());
                    if (!row->extra.empty())
                    {
                        path += L"\\" + row->name;
                    }
                }
                else if (browse_.current_node())
                {
                    path = registry_path::Build(*browse_.current_node());
                }
                return path;
            };
            std::wstring path = build_path();
            if (path.empty())
            {
                return true;
            }
            RegistryPathFormat format = RegistryPathFormat::kFull;
            switch (command_id)
            {
            case cmd::kEditCopyKeyPathAbbrev:
                format = RegistryPathFormat::kAbbrev;
                break;
            case cmd::kEditCopyKeyPathRegEdit:
                format = RegistryPathFormat::kRegEdit;
                break;
            case cmd::kEditCopyKeyPathRegFile:
                format = RegistryPathFormat::kRegFile;
                break;
            case cmd::kEditCopyKeyPathPowerShell:
                format = RegistryPathFormat::kPowerShellDrive;
                break;
            case cmd::kEditCopyKeyPathPowerShellProvider:
                format = RegistryPathFormat::kPowerShellProvider;
                break;
            case cmd::kEditCopyKeyPathEscaped:
                format = RegistryPathFormat::kEscaped;
                break;
            default:
                format = RegistryPathFormat::kFull;
                break;
            }
            ui::CopyTextToClipboard(hwnd_, FormatRegistryPath(path, format));
            return true;
        }
    case cmd::kEditCopy:
        {
            HWND focus = GetFocus();
            if (focus == browse_.tree().hwnd() && browse_.current_node() && !browse_.current_node()->subkey.empty())
            {
                const RegistryNode& node = *browse_.current_node();
                RegistryNode parent = node;
                parent.subkey = registry_path::Parent(node.subkey);
                clipboard_.kind = ClipboardItem::Kind::kKey;
                clipboard_.source_parent = parent;
                clipboard_.name = LeafName(node);
                clipboard_.key_snapshot = changes::CaptureKey(node, false);
                ui::CopyTextToClipboard(hwnd_, registry_path::Build(node));
                return true;
            }
            if (focus == browse_.values().hwnd() || focus == search_results_list_ || focus == history_list_)
            {
                HWND list = focus;
                int selected = ListView_GetSelectedCount(list);
                if (selected > 0)
                {
                    std::wstring text = BuildSelectedListViewText(list);
                    if (!text.empty())
                    {
                        ui::CopyTextToClipboard(hwnd_, text);
                    }
                    if (list == browse_.values().hwnd() && selected == 1 && browse_.current_node())
                    {
                        int index = -1;
                        const ListRow* row = SelectedValueRow(browse_.values(), &index);
                        if (row && row->kind == rowkind::kValue)
                        {
                            RegistryValue entry;
                            if (GetValueEntry(*browse_.current_node(), row->extra, &entry))
                            {
                                clipboard_.kind = ClipboardItem::Kind::kValue;
                                clipboard_.source_parent = *browse_.current_node();
                                clipboard_.name = entry.name;
                                clipboard_.value = entry;
                            }
                        }
                        else if (row && row->kind == rowkind::kKey)
                        {
                            RegistryNode child = ChildNode(*browse_.current_node(), row->extra);
                            clipboard_.kind = ClipboardItem::Kind::kKey;
                            clipboard_.source_parent = *browse_.current_node();
                            clipboard_.name = row->extra;
                            clipboard_.key_snapshot = changes::CaptureKey(child, false);
                        }
                    }
                    else if (list == browse_.values().hwnd())
                    {
                        clipboard_.kind = ClipboardItem::Kind::kNone;
                    }
                    return true;
                }
            }
            if (!browse_.current_node())
            {
                return true;
            }
            int index = -1;
            const ListRow* row = SelectedValueRow(browse_.values(), &index);
            if (row && row->kind == rowkind::kValue)
            {
                RegistryValue entry;
                if (GetValueEntry(*browse_.current_node(), row->extra, &entry))
                {
                    clipboard_.kind = ClipboardItem::Kind::kValue;
                    clipboard_.source_parent = *browse_.current_node();
                    clipboard_.name = entry.name;
                    clipboard_.value = entry;
                    ui::CopyTextToClipboard(hwnd_, row->name);
                }
                else
                {
                    ui::ShowError(hwnd_, util::Tr(L"Failed to read value."));
                }
                return true;
            }
            if (row && row->kind == rowkind::kKey)
            {
                RegistryNode child = ChildNode(*browse_.current_node(), row->extra);
                clipboard_.kind = ClipboardItem::Kind::kKey;
                clipboard_.source_parent = *browse_.current_node();
                clipboard_.name = row->extra;
                clipboard_.key_snapshot = changes::CaptureKey(child, false);
                ui::CopyTextToClipboard(hwnd_, registry_path::Build(child));
                return true;
            }
            clipboard_.kind = ClipboardItem::Kind::kNone;
            ui::CopyTextToClipboard(hwnd_, registry_path::Build(*browse_.current_node()));
            return true;
        }
    default:
        return false;
    }
}

bool MainWindow::Impl::HandleEditToolsCommand(int command_id)
{
    switch (command_id)
    {
    case cmd::kEditGoTo:
        if (browse_.address())
        {
            SetFocus(browse_.address());
            SendMessageW(browse_.address(), EM_SETSEL, 0, -1);
        }
        return true;
    case cmd::kEditPermissions:
    case cmd::kEditKeyInfo:
    case cmd::kEditOpenSourceHive:
        if (command_id == cmd::kEditPermissions && !EnsureWritable())
        {
            return true;
        }
        if (browse_.current_node())
        {
            const RegistryNode node = SelectedKeyNode();
            if (command_id == cmd::kEditPermissions)
            {
                ShowPermissionsDialog(node);
            }
            else if (command_id == cmd::kEditKeyInfo)
            {
                ShowKeyInfoDialog(node);
            }
            else
            {
                KeyDetails details;
                const std::wstring source = RegistryStore::QueryKeyDetails(node, &details) ? registry_path::ClassesSourcePath(details.native.native_name, util::GetCurrentUserSidString()) : std::wstring();
                if (source.empty() || !SelectTreePath(source))
                {
                    ui::ShowError(hwnd_, util::Tr(L"The source key couldn't be opened.") + std::wstring(L"\n") + registry_path::Build(node));
                }
            }
        }
        return true;
    case cmd::kEditFind:
        {
            SearchDialogResult options = last_search_;
            SearchSources sources;
            sources.traces = HasActiveTraces();
            sources.defaults = !active_defaults_.empty();
            sources.registry_root = registry_root_.get() != nullptr;
            sources.offline = !session_->offline_roots.empty();
            sources.reg_files = std::any_of(tabs_.begin(), tabs_.end(), [](const TabEntry& tab) {
                return tab.kind == TabEntry::Kind::kRegFile && !tab.reg_file_roots.empty();
            });
            sources.remote = session_->remote_hklm != nullptr;
            sources.extra_hives = settings_.show_extra_hives;
            if (ShowSearchDialog(hwnd_, &options, sources))
            {
                last_search_ = options;
                StartSearch(options);
            }
            return true;
        }
    default:
        return false;
    }
}

bool MainWindow::Impl::HandleChangeHistoryCommand(int command_id)
{
    switch (command_id)
    {
    case cmd::kEditPaste:
        {
            if (!EnsureWritable())
            {
                return true;
            }
            if (!browse_.current_node() || clipboard_.kind == ClipboardItem::Kind::kNone)
            {
                return true;
            }
            if (clipboard_.kind == ClipboardItem::Kind::kValue)
            {
                bool same_parent = SameNode(*browse_.current_node(), clipboard_.source_parent);
                std::wstring base_name = clipboard_.name;
                if (same_parent)
                {
                    if (base_name.empty())
                    {
                        base_name = L"Default - Copy";
                    }
                    else
                    {
                        base_name += L" - Copy";
                    }
                }
                std::wstring unique = MakeUniqueValueName(*browse_.current_node(), base_name);
                RegistryValue new_value = clipboard_.value;
                new_value.name = unique;
                if (!RegistryStore::SetValue(*browse_.current_node(), unique, new_value.type, new_value.data))
                {
                    ui::ShowError(hwnd_, util::Tr(L"Failed to paste value."));
                }
                else
                {
                    std::wstring data_text = value_format::Data(new_value.type, new_value.data.data(), static_cast<DWORD>(new_value.data.size()));
                    AppendValueHistoryEntry(L"Create value " + unique, L"", data_text, *browse_.current_node(), unique, HistoryEntry::RevertKind::kDeleteValue);
                    MarkOfflineDirty();
                    changes::UndoOperation op;
                    op.type = changes::UndoOperation::Type::kCreateValue;
                    op.node = *browse_.current_node();
                    op.name = unique;
                    op.new_value = new_value;
                    PushUndo(std::move(op));
                    UpdateValueListForNode(browse_.current_node());
                }
                return true;
            }
            if (clipboard_.kind == ClipboardItem::Kind::kKey)
            {
                bool same_parent = SameNode(*browse_.current_node(), clipboard_.source_parent);
                std::wstring base_name = clipboard_.name;
                if (same_parent && !base_name.empty())
                {
                    base_name += L" - Copy";
                }
                std::wstring unique = MakeUniqueKeyName(*browse_.current_node(), base_name);
                changes::KeySnapshot snapshot = clipboard_.key_snapshot;
                snapshot.name = unique;
                if (!changes::RestoreKey(*browse_.current_node(), snapshot))
                {
                    RefreshTreeSelection();
                    UpdateValueListForNode(browse_.current_node());
                    ui::ShowError(hwnd_, util::Tr(L"Failed to paste key."));
                }
                else
                {
                    AppendHistoryEntry(L"Create key " + unique, L"", L"");
                    MarkOfflineDirty();
                    changes::UndoOperation op;
                    op.type = changes::UndoOperation::Type::kCreateKey;
                    op.node = *browse_.current_node();
                    op.name = unique;
                    op.key_snapshot = snapshot;
                    PushUndo(std::move(op));
                    RefreshTreeSelection();
                    UpdateValueListForNode(browse_.current_node());
                }
                return true;
            }
            return true;
        }
    case cmd::kEditReplace:
        {
            if (!EnsureWritable())
            {
                return true;
            }
            ReplaceDialogResult options = last_replace_;
            if (options.start_key.empty() && browse_.current_node())
            {
                options.start_key = registry_path::Build(*browse_.current_node());
            }
            if (ShowReplaceDialog(hwnd_, &options))
            {
                last_replace_ = options;
                StartReplace(options);
            }
            return true;
        }
    case cmd::kEditUndo:
        {
            if (!EnsureWritable())
            {
                return true;
            }
            auto operation = session_->undo.TakeUndo();
            if (!operation)
            {
                return true;
            }
            switch (ApplyUndoOperation(*operation, false))
            {
            case ReplayResult::kSuccess:
                session_->undo.CompleteUndo(std::move(*operation));
                break;
            case ReplayResult::kUnchanged:
                session_->undo.CompleteRedo(std::move(*operation));
                ui::ShowError(hwnd_, util::Tr(L"The change couldn't be undone."));
                break;
            case ReplayResult::kPartial:
                break;
            }
            UpdateUndoButtons();
            return true;
        }
    case cmd::kEditRedo:
        {
            if (!EnsureWritable())
            {
                return true;
            }
            auto operation = session_->undo.TakeRedo();
            if (!operation)
            {
                return true;
            }
            switch (ApplyUndoOperation(*operation, true))
            {
            case ReplayResult::kSuccess:
                session_->undo.CompleteRedo(std::move(*operation));
                break;
            case ReplayResult::kUnchanged:
                session_->undo.CompleteUndo(std::move(*operation));
                ui::ShowError(hwnd_, util::Tr(L"The change couldn't be redone."));
                break;
            case ReplayResult::kPartial:
                break;
            }
            UpdateUndoButtons();
            return true;
        }
    default:
        return false;
    }
}

bool MainWindow::Impl::HandleRegistryNavigationCommand(int command_id)
{
    switch (command_id)
    {
    case cmd::kRegistryLocal:
        OpenLocalRegistryTab();
        return true;
    case cmd::kRegistryNetwork:
        if (SwitchToRemoteRegistry())
        {
            BuildMenus();
        }
        return true;
    case cmd::kRegistryOffline:
        if (SwitchToOfflineRegistry())
        {
            BuildMenus();
        }
        return true;
    case cmd::kNavBack:
        NavigateBack();
        return true;
    case cmd::kNavForward:
        NavigateForward();
        return true;
    case cmd::kNavUp:
        NavigateUp();
        return true;
    default:
        return false;
    }
}

} // namespace regkit
