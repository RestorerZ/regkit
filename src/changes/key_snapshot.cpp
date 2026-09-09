// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "changes/key_snapshot.h"

#include "registry/registry_path.h"

namespace regkit::changes {
namespace {

RegistryNode ChildNode(
    const RegistryNode& parent,
    const std::wstring& name
) {
  RegistryNode child = parent;
  child.subkey = parent.subkey.empty() ? name
                                       : parent.subkey + L"\\" + name;
  child.children_loaded = false;
  return child;
}

} // namespace

KeySnapshot CaptureKey(
    const RegistryNode& node
) {
  KeySnapshot snapshot;
  snapshot.name = registry_path::Leaf(node.subkey);
  if (!RegistryStore::ReadKeySecurity(node, &snapshot.security) &&
      !RegistryStore::IsVirtualRoot(node.root)) {
    snapshot.complete = false;
  }
  if (RegistryStore::ReadKeyLink(node, &snapshot.link_target)) {
    return snapshot;
  }
  RegistryStore::KeyEnumResult result;
  bool reserved = false;
  std::vector<std::wstring> children;
  snapshot.complete = RegistryStore::EnumKeyStreaming(
                          node,
                          true,
                          true,
                          true,
                          &result,
                          [&](const ValueInfo& info, const BYTE* data, DWORD size) {
                            if (!reserved) {
                              if (result.info_valid) {
                                snapshot.values.reserve(result.info.value_count);
                              }
                              reserved = true;
                            }
                            ValueEntry value;
                            value.name = info.name;
                            value.type = info.type;
                            if (data && size > 0) {
                              value.data.assign(data, data + size);
                            }
                            snapshot.values.push_back(std::move(value));
                            return true;
                          },
                          [&](const std::wstring& name) {
                            children.push_back(name);
                            return true;
                          }
                      ) &&
                      snapshot.complete;

  snapshot.children.reserve(children.size());
  for (const std::wstring& name : children) {
    snapshot.children.push_back(CaptureKey(ChildNode(node, name)));
    if (!snapshot.children.back().complete) {
      snapshot.complete = false;
    }
  }
  if (result.info_valid && result.info.subkey_count != children.size()) {
    snapshot.complete = false;
  }
  return snapshot;
}

bool RestoreKey(
    const RegistryNode& parent,
    const KeySnapshot& snapshot
) {
  if (snapshot.name.empty()) {
    return false;
  }
  if (!snapshot.link_target.empty()) {
    if (!RegistryStore::CreateKeyLink(parent, snapshot.name, snapshot.link_target)) {
      return false;
    }
    if (!snapshot.security.empty()) {
      RegistryStore::WriteKeySecurity(ChildNode(parent, snapshot.name), snapshot.security);
    }
    return true;
  }
  if (!RegistryStore::CreateKey(parent, snapshot.name)) {
    return false;
  }
  const RegistryNode node = ChildNode(parent, snapshot.name);
  if (!snapshot.security.empty()) {
    RegistryStore::WriteKeySecurity(node, snapshot.security);
  }
  for (const ValueEntry& value : snapshot.values) {
    if (!RegistryStore::SetValue(node, value.name, value.type, value.data)) {
      return false;
    }
  }
  for (const KeySnapshot& child : snapshot.children) {
    if (!RestoreKey(node, child)) {
      return false;
    }
  }
  return true;
}

} // namespace regkit::changes
