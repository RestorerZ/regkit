// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"
#include "win32/text_transform.h"

namespace regkit
{
using namespace window_detail;

void MainWindow::Impl::RefreshTreePath(const std::wstring& path)
{
    RefreshTreeItem(FindTreeItem(path));
}

namespace
{

bool IsAncestorItem(HWND tree, HTREEITEM candidate, HTREEITEM item)
{
    for (HTREEITEM parent = TreeView_GetParent(tree, item); parent; parent = TreeView_GetParent(tree, parent))
    {
        if (parent == candidate)
        {
            return true;
        }
    }
    return false;
}

std::wstring KernelName(const RegistryNode& node)
{
    KeyDetails details;
    return RegistryStore::QueryKeyDetails(node, &details) ? details.native.native_name : std::wstring();
}

std::wstring NativePath(const RegistryNode& node)
{
    return node.view ? std::wstring() : registry_path::BuildNative(node);
}

} // namespace

void MainWindow::Impl::RefreshMatchingTreeNodes(HTREEITEM selected)
{
    HWND tree = browse_.tree().hwnd();
    if (tree && !selected)
    {
        selected = TreeView_GetSelection(tree);
    }
    const RegistryNode* target = selected ? browse_.tree().NodeFromItem(selected) : nullptr;
    if (!target)
    {
        return;
    }
    const std::wstring native = NativePath(*target);
    const std::wstring kernel = KernelName(*target);
    if (native.empty() && kernel.empty())
    {
        return;
    }
    auto label_of = [&](HTREEITEM item, wchar_t* buffer, int size) -> bool {
        TVITEMW info = {};
        info.hItem = item;
        info.mask = TVIF_TEXT;
        info.pszText = buffer;
        info.cchTextMax = size;
        return TreeView_GetItem(tree, &info) != FALSE && buffer[0] != 0;
    };
    wchar_t wanted[256] = {};
    if (!label_of(selected, wanted, static_cast<int>(_countof(wanted))))
    {
        return;
    }

    std::vector<std::pair<int, HTREEITEM>> matches;
    std::vector<std::pair<int, HTREEITEM>> pending;
    if (HTREEITEM root = TreeView_GetRoot(tree))
    {
        pending.emplace_back(0, root);
    }
    while (!pending.empty())
    {
        const int depth = pending.back().first;
        HTREEITEM item = pending.back().second;
        pending.pop_back();
        for (HTREEITEM child = TreeView_GetChild(tree, item); child; child = TreeView_GetNextSibling(tree, child))
        {
            pending.emplace_back(depth + 1, child);
        }
        const RegistryNode* node = browse_.tree().NodeFromItem(item);
        if (item == selected || !node || !node->children_loaded || IsAncestorItem(tree, item, selected))
        {
            continue;
        }
        const std::wstring path = NativePath(*node);
        wchar_t text[256] = {};
        if ((!path.empty() && (util::EqualsInsensitive(path, native) || util::EqualsInsensitive(path, kernel))) ||
            (!kernel.empty() && label_of(item, text, static_cast<int>(_countof(text))) && util::EqualsInsensitive(text, wanted) && util::EqualsInsensitive(KernelName(*node), kernel)))
        {
            matches.emplace_back(depth, item);
        }
    }

    if (matches.empty())
    {
        return;
    }
    std::sort(matches.begin(), matches.end(), [](const std::pair<int, HTREEITEM>& left, const std::pair<int, HTREEITEM>& right) {
        return left.first > right.first;
    });
    HTREEITEM first_visible = TreeView_GetFirstVisible(tree);
    browse_.tree().SuspendRedraw();
    for (const auto& match : matches)
    {
        RefreshTreeItem(match.second);
    }
    if (first_visible)
    {
        TreeView_SelectSetFirstVisible(tree, first_visible);
    }
    browse_.tree().ResumeRedraw();
}

void MainWindow::Impl::RefreshTreeSelection()
{
    if (!browse_.tree().hwnd())
    {
        return;
    }
    HTREEITEM selected = TreeView_GetSelection(browse_.tree().hwnd());
    RefreshTreeItem(selected);
    RefreshMatchingTreeNodes(selected);
}

void MainWindow::Impl::RefreshTreeItem(HTREEITEM item)
{
    if (!browse_.tree().hwnd() || !item)
    {
        return;
    }
    browse_.tree().SuspendRedraw();
    browse_.tree().Resync(item);
    browse_.tree().ResumeRedraw();
    MarkTreeStateDirty();
}

void MainWindow::Impl::RefreshWholeTree()
{
    if (browse_.tree().hwnd())
    {
        RefreshTreeItem(TreeView_GetRoot(browse_.tree().hwnd()));
    }
}

void MainWindow::Impl::UpdateSimulatedChain(HTREEITEM item)
{
    if (!browse_.tree().hwnd() || !item)
    {
        return;
    }
    while (item)
    {
        RegistryNode* node = browse_.tree().NodeFromItem(item);
        if (node && node->simulated)
        {
            KeyInfo info = {};
            if (RegistryStore::QueryKeyInfo(*node, &info))
            {
                node->simulated = false;
                int icon = KeyIconIndex(*node, nullptr, nullptr);
                TVITEMW tvi = {};
                tvi.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE;
                tvi.hItem = item;
                tvi.iImage = icon;
                tvi.iSelectedImage = icon;
                TreeView_SetItem(browse_.tree().hwnd(), &tvi);
            }
        }
        item = TreeView_GetParent(browse_.tree().hwnd(), item);
    }
}

void MainWindow::Impl::CaptureTreeState(std::wstring* selected_path, std::vector<std::wstring>* expanded_paths) const
{
    if (selected_path)
    {
        selected_path->clear();
    }
    if (expanded_paths)
    {
        expanded_paths->clear();
    }
    if (!browse_.tree().hwnd())
    {
        return;
    }
    if (selected_path)
    {
        RegistryNode* node = browse_.current_node();
        if (!node)
        {
            HTREEITEM selected = TreeView_GetSelection(browse_.tree().hwnd());
            if (selected)
            {
                TVITEMW tvi = {};
                tvi.hItem = selected;
                tvi.mask = TVIF_PARAM;
                if (TreeView_GetItem(browse_.tree().hwnd(), &tvi))
                {
                    node = reinterpret_cast<RegistryNode*>(tvi.lParam);
                }
            }
        }
        if (node)
        {
            *selected_path = registry_path::Build(*node);
        }
    }
    if (!expanded_paths)
    {
        return;
    }
    HTREEITEM root = TreeView_GetRoot(browse_.tree().hwnd());
    if (!root)
    {
        return;
    }
    std::function<void(HTREEITEM, bool)> walk = [&](HTREEITEM item, bool ancestors_expanded) {
        while (item)
        {
            TVITEMW tvi = {};
            tvi.hItem = item;
            tvi.mask = TVIF_STATE | TVIF_PARAM;
            tvi.stateMask = TVIS_EXPANDED;
            if (TreeView_GetItem(browse_.tree().hwnd(), &tvi))
            {
                bool expanded = (tvi.state & TVIS_EXPANDED) != 0;
                if (ancestors_expanded && expanded)
                {
                    RegistryNode* node = reinterpret_cast<RegistryNode*>(tvi.lParam);
                    if (node)
                    {
                        expanded_paths->push_back(registry_path::Build(*node));
                    }
                }
            }
            HTREEITEM child = TreeView_GetChild(browse_.tree().hwnd(), item);
            if (child)
            {
                bool expanded = (tvi.state & TVIS_EXPANDED) != 0;
                if (ancestors_expanded && expanded)
                {
                    walk(child, true);
                }
            }
            item = TreeView_GetNextSibling(browse_.tree().hwnd(), item);
        }
    };
    walk(root, true);
}

void MainWindow::Impl::RestoreTreeState()
{
    if (tree_state_restored_)
    {
        return;
    }
    if (!settings_.save_tree_state)
    {
        return;
    }
    tree_state_restored_ = true;
    if (!browse_.tree().hwnd())
    {
        return;
    }
    workspace::TreeState state = saved_tree_state_;
    state.Normalize();
    ExpandTreePaths(state.expanded_paths);
    if (!state.selected_path.empty())
    {
        SelectTreePath(state.selected_path);
    }
}

void MainWindow::Impl::ApplySavedWindowPlacement()
{
    if (!window_placement_loaded_ || !hwnd_)
    {
        return;
    }
    if (settings_.window_width <= 0 || settings_.window_height <= 0)
    {
        return;
    }
    const int min_width = 640;
    const int min_height = 480;
    const int width = std::max(settings_.window_width, min_width);
    const int height = std::max(settings_.window_height, min_height);

    RECT work = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
    {
        work = {};
    }
    RECT target = {settings_.window_x + work.left, settings_.window_y + work.top, settings_.window_x + work.left + width, settings_.window_y + work.top + height};
    win32::ClampToWorkArea(&target);
    SetWindowPos(hwnd_, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

HTREEITEM MainWindow::Impl::FindTreeItem(const std::wstring& path)
{
    if (!browse_.tree().hwnd())
    {
        return nullptr;
    }
    std::vector<std::wstring> parts = BuildVisibleTreePathParts(path);
    if (parts.empty())
    {
        return nullptr;
    }
    HTREEITEM current = TreeView_GetRoot(browse_.tree().hwnd());
    for (const auto& part : parts)
    {
        TreeView_Expand(browse_.tree().hwnd(), current, TVE_EXPAND);
        HTREEITEM child = FindChildByText(browse_.tree().hwnd(), current, part);
        if (!child)
        {
            return nullptr;
        }
        current = child;
    }
    return current;
}

void MainWindow::Impl::ExpandTreePaths(const std::vector<std::wstring>& paths)
{
    HWND tree = browse_.tree().hwnd();
    if (!tree || paths.empty())
    {
        return;
    }
    std::unordered_set<std::wstring> wanted;
    wanted.reserve(paths.size());
    for (const auto& path : paths)
    {
        std::wstring lower = ToLower(path);
        while (!lower.empty())
        {
            if (!wanted.insert(lower).second)
            {
                break;
            }
            const size_t separator = lower.rfind(L'\\');
            lower = separator == std::wstring::npos ? std::wstring() : lower.substr(0, separator);
        }
    }
    if (wanted.empty())
    {
        return;
    }
    std::vector<HTREEITEM> pending;
    if (HTREEITEM root = TreeView_GetRoot(tree))
    {
        pending.push_back(root);
    }
    while (!pending.empty())
    {
        HTREEITEM item = pending.back();
        pending.pop_back();
        RegistryNode* node = browse_.tree().NodeFromItem(item);
        if (node && wanted.find(ToLower(registry_path::Build(*node))) == wanted.end())
        {
            continue;
        }
        TreeView_Expand(tree, item, TVE_EXPAND);
        for (HTREEITEM child = TreeView_GetChild(tree, item); child; child = TreeView_GetNextSibling(tree, child))
        {
            pending.push_back(child);
        }
    }
}

void MainWindow::Impl::MarkTreeStateDirty()
{
    if (!settings_.save_tree_state || !hwnd_ || !browse_.tree().hwnd() || !IsWindow(browse_.tree().hwnd()))
    {
        return;
    }
    SetTimer(hwnd_, kTreeStateTimerId, 400, nullptr);
}

void MainWindow::Impl::CaptureTreeStateNow()
{
    if (!settings_.save_tree_state || !browse_.tree().hwnd() || !IsWindow(browse_.tree().hwnd()))
    {
        return;
    }
    std::wstring selected;
    std::vector<std::wstring> expanded;
    CaptureTreeState(&selected, &expanded);
    workspace::TreeState state;
    state.selected_path = std::move(selected);
    state.expanded_paths = std::move(expanded);
    tree_state_saver_.Submit(std::move(state));
}

void MainWindow::Impl::SaveTreeStateFile(const std::wstring& selected, const std::vector<std::wstring>& expanded) const
{
    workspace::TreeState state;
    state.selected_path = selected;
    state.expanded_paths = expanded;
    workspace::SaveTreeState(TreeStatePath(), state);
}

} // namespace regkit
