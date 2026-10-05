// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"
#include "win32/translation.h"

namespace regkit
{

using namespace window_detail;

void MainWindow::Impl::UpdateUndoButtons()
{
    if (toolbar_.hwnd())
    {
        SendMessageW(toolbar_.hwnd(), TB_SETSTATE, cmd::kEditUndo, !settings_.read_only && session_->undo.CanUndo() ? TBSTATE_ENABLED : 0);
        SendMessageW(toolbar_.hwnd(), TB_SETSTATE, cmd::kEditRedo, !settings_.read_only && session_->undo.CanRedo() ? TBSTATE_ENABLED : 0);
    }
}

void MainWindow::Impl::PushUndo(changes::UndoOperation operation)
{
    if (is_replaying_)
    {
        return;
    }
    session_->undo.Push(std::move(operation));
    UpdateUndoButtons();
}

void MainWindow::Impl::ClearRedo()
{
    session_->undo.ClearRedo();
    UpdateUndoButtons();
}

MainWindow::Impl::ReplayResult MainWindow::Impl::ApplyUndoOperation(const changes::UndoOperation& operation, bool redo)
{
    if (!browse_.current_node())
    {
        return ReplayResult::kUnchanged;
    }
    bool ok = false;
    bool rename_left_both_names = false;
    std::optional<std::wstring> restored_value;
    is_replaying_ = true;
    switch (operation.type)
    {
    case changes::UndoOperation::Type::kCreateKey:
        {
            if (redo)
            {
                if (!operation.key_snapshot.name.empty())
                {
                    ok = changes::RestoreKey(operation.node, operation.key_snapshot);
                }
                else
                {
                    ok = RegistryStore::CreateKey(operation.node, operation.name);
                }
                if (ok)
                {
                    RefreshTreeSelection();
                    SelectChildKey(operation.node, operation.name);
                }
            }
            else
            {
                RegistryNode child = ChildNode(operation.node, operation.name);
                ok = RegistryStore::DeleteKey(child);
                if (ok)
                {
                    RefreshTreeSelection();
                }
            }
            break;
        }
    case changes::UndoOperation::Type::kDeleteKey:
        {
            if (redo)
            {
                RegistryNode child = ChildNode(operation.node, operation.name);
                ok = RegistryStore::DeleteKey(child);
                if (ok)
                {
                    RefreshTreeSelection();
                }
            }
            else
            {
                ok = changes::RestoreKey(operation.node, operation.key_snapshot);
                if (ok)
                {
                    RefreshTreeSelection();
                    SelectChildKey(operation.node, operation.key_snapshot.name);
                }
            }
            break;
        }
    case changes::UndoOperation::Type::kRenameKey:
        {
            std::wstring from = redo ? operation.name : operation.new_name;
            std::wstring to = redo ? operation.new_name : operation.name;
            RegistryNode child = ChildNode(operation.node, from);
            ok = RegistryStore::RenameKey(child, to);
            if (ok)
            {
                RefreshTreeSelection();
                std::wstring path = registry_path::Build(operation.node);
                if (!path.empty())
                {
                    path.append(L"\\");
                    path.append(to);
                    SelectTreePath(path);
                }
            }
            break;
        }
    case changes::UndoOperation::Type::kCreateValue:
        {
            if (redo)
            {
                ok = RegistryStore::SetValue(operation.node, operation.new_value.name, operation.new_value.type, operation.new_value.data);
                restored_value = operation.new_value.name;
            }
            else
            {
                ok = RegistryStore::DeleteValue(operation.node, operation.name);
            }
            break;
        }
    case changes::UndoOperation::Type::kDeleteValue:
        {
            if (redo)
            {
                ok = RegistryStore::DeleteValue(operation.node, operation.old_value.name);
            }
            else
            {
                ok = RegistryStore::SetValue(operation.node, operation.old_value.name, operation.old_value.type, operation.old_value.data);
                restored_value = operation.old_value.name;
            }
            break;
        }
    case changes::UndoOperation::Type::kModifyValue:
        {
            const RegistryValue& value = redo ? operation.new_value : operation.old_value;
            ok = RegistryStore::SetValue(operation.node, value.name, value.type, value.data);
            break;
        }
    case changes::UndoOperation::Type::kReplaceKey:
        {
            ok = changes::ReplaceKey(operation.node, redo ? operation.new_key_snapshot : operation.key_snapshot);
            if (ok)
            {
                RefreshTreeSelection();
            }
            break;
        }
    case changes::UndoOperation::Type::kRenameValue:
        {
            std::wstring from = redo ? operation.name : operation.new_name;
            std::wstring to = redo ? operation.new_name : operation.name;
            ok = RegistryStore::RenameValue(operation.node, from, to, &rename_left_both_names);
            break;
        }
    default:
        break;
    }
    is_replaying_ = false;

    if (ok || rename_left_both_names)
    {
        MarkOfflineDirty();
    }
    if ((ok || rename_left_both_names) && browse_.current_node())
    {
        UpdateValueListForNode(browse_.current_node());
        if (ok && restored_value)
        {
            SelectValueAfterRefresh(*restored_value);
        }
    }
    if (rename_left_both_names)
    {
        ui::ShowError(hwnd_, util::Tr(L"The value was copied to the new name but the old name "
                                      L"couldn't be removed. Both names now exist."));
    }
    UpdateUndoButtons();
    if (rename_left_both_names)
    {
        return ReplayResult::kPartial;
    }
    return ok ? ReplayResult::kSuccess : ReplayResult::kUnchanged;
}

bool MainWindow::Impl::SameNode(const RegistryNode& left, const RegistryNode& right) const
{
    if (left.root != right.root)
    {
        return false;
    }
    if (!EqualsInsensitive(left.subkey, right.subkey))
    {
        return false;
    }
    return EqualsInsensitive(left.root_name, right.root_name);
}

std::wstring MainWindow::Impl::MakeUniqueValueName(const RegistryNode& node, const std::wstring& base) const
{
    std::unordered_set<std::wstring> value_names;
    RegistryStore::KeyEnumResult enum_result;
    bool names_reserved = false;
    RegistryStore::EnumKeyStreaming(node, true, false, false, &enum_result, [&](const ValueInfo& value, const BYTE*, DWORD) {
        if (!names_reserved)
        {
            if (enum_result.info_valid)
            {
                value_names.reserve(enum_result.info.value_count);
            }
            names_reserved = true;
        }
        value_names.insert(ToLower(value.name));
        return true;
    },
                                    {});
    auto exists = [&](const std::wstring& candidate) -> bool { return value_names.contains(ToLower(candidate)); };

    std::wstring base_name = base;
    if (base_name.empty())
    {
        if (!exists(base_name))
        {
            return base_name;
        }
        base_name = L"Default";
    }
    if (!exists(base_name))
    {
        return base_name;
    }
    for (int i = 2; i < 10000; ++i)
    {
        std::wstring next = base_name + L" (" + std::to_wstring(i) + L")";
        if (!exists(next))
        {
            return next;
        }
    }
    return base_name;
}

std::wstring MainWindow::Impl::MakeUniqueKeyName(const RegistryNode& node, const std::wstring& base) const
{
    auto keys = RegistryStore::EnumSubKeyNames(node, false);
    auto exists = [&](const std::wstring& candidate) -> bool {
        for (const auto& key : keys)
        {
            if (EqualsInsensitive(key, candidate))
            {
                return true;
            }
        }
        return false;
    };

    std::wstring base_name = base;
    if (base_name.empty())
    {
        base_name = L"New Key";
    }
    if (!exists(base_name))
    {
        return base_name;
    }
    for (int i = 2; i < 10000; ++i)
    {
        std::wstring next = base_name + L" (" + std::to_wstring(i) + L")";
        if (!exists(next))
        {
            return next;
        }
    }
    return base_name;
}

bool MainWindow::Impl::ResolvePathToNode(const std::wstring& path, RegistryNode* node) const
{
    if (!node || path.empty())
    {
        return false;
    }
    for (const auto& root_entry : browse_.roots())
    {
        if (!StartsWithInsensitive(path, root_entry.path_name))
        {
            continue;
        }
        std::wstring rest = registry_path::RawName(std::wstring_view(path).substr(root_entry.path_name.size()));
        if (!rest.empty() && (rest.front() == L'\\' || rest.front() == L'/'))
        {
            rest.erase(rest.begin());
        }
        if (root_entry.subkey_prefix.empty())
        {
            node->root = root_entry.root;
            node->root_name = root_entry.path_name;
            node->subkey = rest;
            return true;
        }
        std::wstring prefix = root_entry.subkey_prefix;
        if (!rest.empty())
        {
            if (!StartsWithInsensitive(rest, prefix))
            {
                rest = prefix + L"\\" + rest;
            }
        }
        else
        {
            rest = prefix;
        }
        node->root = root_entry.root;
        node->root_name = root_entry.path_name;
        node->subkey = rest;
        return true;
    }
    return false;
}

} // namespace regkit
