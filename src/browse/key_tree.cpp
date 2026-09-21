// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "browse/key_tree.h"

#include <algorithm>

#include "registry/registry_path.h"
#include "win32/text_transform.h"

namespace regkit {

namespace {
constexpr int kFolderIconIndex = 0;
constexpr int kRootKeysIconIndex = 5;
constexpr int kRegistryIconIndex = 6;
constexpr wchar_t kRootKeysGroupLabel[] = L"Root Keys";
constexpr wchar_t kRealGroupLabel[] = L"REGISTRY";
#ifndef TVS_EX_DOUBLEBUFFER
#define TVS_EX_DOUBLEBUFFER 0x0004
#endif

using util::ToLower;

void SuspendRedraw(
    HWND tree
) {
  SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
}

void ResumeRedraw(
    HWND tree
) {
  SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
  RedrawWindow(tree, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

HTREEITEM InsertFolderItem(
    HWND tree,
    HTREEITEM parent,
    const wchar_t* label,
    int icon = kFolderIconIndex
) {
  TVINSERTSTRUCTW insert = {};
  insert.hParent = parent;
  insert.hInsertAfter = TVI_LAST;
  insert.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM;
  insert.item.pszText = const_cast<wchar_t*>(label);
  insert.item.iImage = icon;
  insert.item.iSelectedImage = icon;
  return TreeView_InsertItem(tree, &insert);
}
} // namespace

void RegistryTree::Create(
    HWND parent,
    HINSTANCE instance,
    int control_id,
    bool show_border,
    bool allow_label_edit
) {
  DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS;
  if (show_border) {
    style |= WS_BORDER;
  }
  if (allow_label_edit) {
    style |= TVS_EDITLABELS;
  }
  hwnd_ = CreateWindowExW(0, WC_TREEVIEWW, L"", style, 0, 0, 100, 100, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)), instance, nullptr);
  if (hwnd_) {
    TreeView_SetExtendedStyle(hwnd_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
  }
}

HWND RegistryTree::hwnd() const {
  return hwnd_;
}

void RegistryTree::SetImageList(
    HIMAGELIST image_list
) {
  if (!hwnd_) {
    return;
  }
  TreeView_SetImageList(hwnd_, image_list, TVSIL_NORMAL);
}

void RegistryTree::SetIconResolver(
    std::function<int(const RegistryNode&)> resolver
) {
  icon_resolver_ = std::move(resolver);
}

void RegistryTree::SetVirtualChildProvider(
    std::function<void(const RegistryNode&, const std::unordered_set<std::wstring>&, std::vector<std::wstring>*)> provider
) {
  virtual_child_provider_ = std::move(provider);
}

void RegistryTree::SetRootLabel(
    const std::wstring& label,
    int icon
) {
  root_icon_ = icon;
  if (label.empty()) {
    root_label_ = L"Computer";
    return;
  }
  root_label_ = label;
}

void RegistryTree::SetRegEditLayout(
    bool enabled
) {
  regedit_layout_ = enabled;
}

void RegistryTree::PopulateRoots(
    const std::vector<RegistryRootEntry>& roots
) {
  TreeView_DeleteAllItems(hwnd_);
  nodes_.clear();
  nodes_.reserve(roots.size() + 3);

  root_item_ = InsertFolderItem(hwnd_, TVI_ROOT, root_label_.c_str(), root_icon_);
  // group root keys unless regedit layout needs a flat tree
  const auto has_group = [&](bool real) {
    return !regedit_layout_ && std::any_of(roots.begin(), roots.end(), [&](const RegistryRootEntry& entry) { return (entry.group == RegistryRootGroup::kReal) == real; });
  };
  standard_group_item_ = has_group(false) ? InsertFolderItem(hwnd_, root_item_, kRootKeysGroupLabel, kRootKeysIconIndex) : nullptr;
  real_group_item_ = has_group(true) ? InsertFolderItem(hwnd_, root_item_, kRealGroupLabel, kRegistryIconIndex) : nullptr;

  for (const auto& root_entry : roots) {
    if (regedit_layout_ && root_entry.group == RegistryRootGroup::kReal) {
      continue;
    }

    auto node = std::make_unique<RegistryNode>();
    node->root = root_entry.root;
    node->subkey = root_entry.subkey_prefix;
    node->root_name = root_entry.path_name;
    RegistryNode* stored = StoreNode(std::move(node));

    int icon_index = kFolderIconIndex;
    if (icon_resolver_) {
      icon_index = icon_resolver_(*stored);
    }

    // reuse REGISTRY group item as real registry root
    if (!regedit_layout_ && root_entry.group == RegistryRootGroup::kReal && real_group_item_ && util::EqualsInsensitive(root_entry.display_name, kRealGroupLabel) && root_entry.subkey_prefix.empty()) {
      TVITEMW item = {};
      item.mask = TVIF_PARAM | TVIF_CHILDREN;
      item.hItem = real_group_item_;
      item.lParam = reinterpret_cast<LPARAM>(stored);
      item.cChildren = I_CHILDRENCALLBACK;
      TreeView_SetItem(hwnd_, &item);
      continue;
    }

    TVINSERTSTRUCTW root_item = {};
    if (regedit_layout_) {
      root_item.hParent = root_item_;
    } else if (root_entry.group == RegistryRootGroup::kReal) {
      root_item.hParent = real_group_item_ ? real_group_item_ : root_item_;
    } else {
      root_item.hParent = standard_group_item_ ? standard_group_item_ : root_item_;
    }
    root_item.hInsertAfter = TVI_LAST;
    root_item.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
    root_item.item.pszText = const_cast<wchar_t*>(root_entry.display_name.c_str());
    root_item.item.lParam = reinterpret_cast<LPARAM>(stored);
    root_item.item.iImage = icon_index;
    root_item.item.iSelectedImage = icon_index;
    root_item.item.cChildren = I_CHILDRENCALLBACK;
    TreeView_InsertItem(hwnd_, &root_item);
  }

  TreeView_Expand(hwnd_, root_item_, TVE_EXPAND);
  if (standard_group_item_) {
    TreeView_Expand(hwnd_, standard_group_item_, TVE_EXPAND);
  }
  if (real_group_item_) {
    TreeView_Expand(hwnd_, real_group_item_, TVE_EXPAND);
  }
}

RegistryNode* RegistryTree::NodeFromItem(
    HTREEITEM item
) {
  if (!item) {
    return nullptr;
  }
  TVITEMW tvi = {};
  tvi.hItem = item;
  tvi.mask = TVIF_PARAM;
  if (!TreeView_GetItem(hwnd_, &tvi)) {
    return nullptr;
  }
  return reinterpret_cast<RegistryNode*>(tvi.lParam);
}

void RegistryTree::OnItemExpanding(
    const NMTREEVIEWW* info
) {
  if (!info || info->action != TVE_EXPAND) {
    return;
  }
  RegistryNode* node = NodeFromItem(info->itemNew.hItem);
  if (!node || node->children_loaded) {
    return;
  }

  // load children on demand when key is expanded
  node->children_loaded = AddChildren(info->itemNew.hItem, node);
}

RegistryNode* RegistryTree::OnSelectionChanged(
    const NMTREEVIEWW* info
) {
  if (!info) {
    return nullptr;
  }
  return NodeFromItem(info->itemNew.hItem);
}

RegistryNode* RegistryTree::StoreNode(
    std::unique_ptr<RegistryNode> node
) {
  // keep node pointers valid as tree items store them in lParam
  RegistryNode* stored = node.get();
  nodes_.emplace(stored, std::move(node));
  return stored;
}

void RegistryTree::CollectSubtree(
    HTREEITEM item,
    std::vector<RegistryNode*>* nodes
) {
  for (HTREEITEM child = TreeView_GetChild(hwnd_, item); child;
       child = TreeView_GetNextSibling(hwnd_, child)) {
    CollectSubtree(child, nodes);
  }
  if (RegistryNode* node = NodeFromItem(item)) {
    nodes->push_back(node);
  }
}

void RegistryTree::DeleteChildren(
    HTREEITEM parent
) {
  if (!hwnd_ || !parent) {
    return;
  }
  HTREEITEM child = TreeView_GetChild(hwnd_, parent);
  if (!child) {
    return;
  }
  if (RegistryNode* node = NodeFromItem(parent)) {
    node->has_children = -1;
    node->icon = -1;
  }
  std::vector<RegistryNode*> released;
  SuspendRedraw(hwnd_);
  while (child) {
    HTREEITEM next = TreeView_GetNextSibling(hwnd_, child);
    // collect node pointers before deleting their tree items
    CollectSubtree(child, &released);
    TreeView_DeleteItem(hwnd_, child);
    child = next;
  }
  ResumeRedraw(hwnd_);
  for (RegistryNode* node : released) {
    nodes_.erase(node);
  }
}

HTREEITEM RegistryTree::InsertChild(
    HTREEITEM parent,
    const std::wstring& name
) {
  RegistryNode* parent_node = NodeFromItem(parent);
  if (!hwnd_ || !parent || !parent_node || !parent_node->children_loaded ||
      name.empty()) {
    return nullptr;
  }
  HTREEITEM after = TVI_FIRST;
  const std::wstring label = registry_path::DisplayName(name);
  wchar_t text[256] = {};
  // find sorted position and reuse a matching item
  for (HTREEITEM sibling = TreeView_GetChild(hwnd_, parent); sibling;
       sibling = TreeView_GetNextSibling(hwnd_, sibling)) {
    TVITEMW item = {};
    item.mask = TVIF_TEXT;
    item.hItem = sibling;
    item.pszText = text;
    item.cchTextMax = static_cast<int>(_countof(text));
    if (!TreeView_GetItem(hwnd_, &item)) {
      continue;
    }
    const int order = util::CompareInsensitive(text, label);
    if (order == 0) {
      return sibling;
    }
    if (order > 0) {
      break;
    }
    after = sibling;
  }

  auto child = std::make_unique<RegistryNode>(registry_path::ChildNode(*parent_node, name));
  child->simulated = false;
  RegistryNode* stored = StoreNode(std::move(child));

  TVINSERTSTRUCTW insert = {};
  insert.hParent = parent;
  insert.hInsertAfter = after;
  insert.item.mask =
      TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
  insert.item.pszText = const_cast<wchar_t*>(label.c_str());
  insert.item.lParam = reinterpret_cast<LPARAM>(stored);
  insert.item.iImage = I_IMAGECALLBACK;
  insert.item.iSelectedImage = I_IMAGECALLBACK;
  insert.item.cChildren = I_CHILDRENCALLBACK;
  HTREEITEM item = TreeView_InsertItem(hwnd_, &insert);
  if (item) {
    parent_node->has_children = 1;
    TVITEMW parent_state = {};
    parent_state.mask = TVIF_CHILDREN;
    parent_state.hItem = parent;
    parent_state.cChildren = 1;
    TreeView_SetItem(hwnd_, &parent_state);
    TreeView_Expand(hwnd_, parent, TVE_EXPAND);
  }
  return item;
}

bool RegistryTree::AddChildren(
    HTREEITEM parent,
    RegistryNode* node
) {
  if (!node) {
    return false;
  }
  std::vector<std::wstring> children;
  const bool enumerated = RegistryStore::EnumKeyStreaming(
      *node,
      false,
      false,
      true,
      nullptr,
      RegistryStore::ValueStreamCallback(),
      [&](const std::wstring& name) {
        children.push_back(name);
        return true;
      },
      MAXDWORD,
      nullptr,
      false
  );
  std::vector<std::wstring> virtual_children;
  // traces can add simulated keys missing from the current key
  if (virtual_child_provider_ && (enumerated || node->simulated)) {
    std::unordered_set<std::wstring> existing_lower;
    existing_lower.reserve(children.size());
    for (const auto& name : children) {
      existing_lower.insert(ToLower(name));
    }
    virtual_child_provider_(*node, existing_lower, &virtual_children);
  }

  struct ChildEntry {
    std::wstring name;
    std::wstring label;
    bool simulated = false;
  };
  std::vector<ChildEntry> entries;
  entries.reserve(children.size() + virtual_children.size());
  for (const auto& name : children) {
    entries.push_back({name, registry_path::DisplayName(name), false});
  }
  for (const auto& name : virtual_children) {
    entries.push_back({name, registry_path::DisplayName(name), true});
  }
  std::sort(entries.begin(), entries.end(), [](const ChildEntry& left, const ChildEntry& right) { return util::CompareInsensitive(left.label, right.label) < 0; });

  if (!entries.empty()) {
    nodes_.reserve(nodes_.size() + entries.size());
  }
  for (const auto& entry : entries) {
    const std::wstring& name = entry.name;
    auto child = std::make_unique<RegistryNode>(registry_path::ChildNode(*node, name));
    child->simulated = entry.simulated;
    RegistryNode* stored = StoreNode(std::move(child));

    TVINSERTSTRUCTW insert = {};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
    insert.item.pszText = const_cast<wchar_t*>(entry.label.c_str());
    insert.item.lParam = reinterpret_cast<LPARAM>(stored);
    insert.item.iImage = I_IMAGECALLBACK;
    insert.item.iSelectedImage = I_IMAGECALLBACK;
    insert.item.cChildren = I_CHILDRENCALLBACK;
    TreeView_InsertItem(hwnd_, &insert);
  }
  node->has_children = entries.empty() ? 0 : 1;
  TVITEMW parent_state = {};
  parent_state.mask = TVIF_CHILDREN;
  parent_state.hItem = parent;
  parent_state.cChildren = entries.empty() ? 0 : 1;
  TreeView_SetItem(hwnd_, &parent_state);
  // only cache the load when the key read succeeded
  return enumerated;
}

void RegistryTree::OnGetDispInfo(
    NMTVDISPINFOW* info
) {
  if (!info) {
    return;
  }
  auto* node = reinterpret_cast<RegistryNode*>(info->item.lParam);
  // resolve child state & icons only when the tree asks for them
  if (info->item.mask & TVIF_CHILDREN) {
    if (!node) {
      info->item.cChildren = 0;
    } else {
      if (node->has_children < 0) {
        node->has_children = HasChildren(*node) ? 1 : 0;
      }
      info->item.cChildren = node->has_children;
    }
  }
  if (info->item.mask & (TVIF_IMAGE | TVIF_SELECTEDIMAGE)) {
    int icon = kFolderIconIndex;
    if (node) {
      if (node->icon < 0) {
        node->icon = icon_resolver_ ? icon_resolver_(*node) : kFolderIconIndex;
      }
      icon = node->icon;
    }
    info->item.iImage = icon;
    info->item.iSelectedImage = icon;
  }
}

bool RegistryTree::HasChildren(
    const RegistryNode& node
) {
  if (RegistryStore::HasSubKeys(node)) {
    return true;
  }
  if (!virtual_child_provider_) {
    return false;
  }
  std::unordered_set<std::wstring> existing_lower;
  std::vector<std::wstring> virtual_children;
  virtual_child_provider_(node, existing_lower, &virtual_children);
  return !virtual_children.empty();
}

} // namespace regkit
