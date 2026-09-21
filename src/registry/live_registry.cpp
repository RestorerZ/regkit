// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "registry/registry_backends.h"

#include "registry/key_algorithms.h"
#include "win32/registry_native.h"
#include "win32/registry_view.h"

namespace regkit::registry_backend::live {
namespace {

class LiveKey : public RegistryKeyHandle {
public:
  LiveKey(
      const RegistryNode& node,
      REGSAM access,
      bool open_link = false
  ) {
    if (node.root) {
      util::OpenRegistryPath(node.root, node.subkey, access | win32::kDefaultRegistryView, open_link, &key_);
    }
  }
  using RegistryKeyHandle::RegistryKeyHandle;
};
LiveKey OpenChild(
    const RegistryNode& node,
    REGSAM parent_access,
    REGSAM child_access,
    bool open_link,
    std::wstring* name = nullptr
) {
  RegistryNode parent_node;
  std::wstring leaf;
  util::UniqueHKey child;
  if (SplitNode(node, &parent_node, &leaf)) {
    LiveKey parent(parent_node, parent_access);
    if (parent) {
      util::OpenRegistryPath(parent.get(), leaf, child_access, open_link, &child);
    }
  }
  if (name) {
    *name = std::move(leaf);
  }
  return LiveKey(std::move(child));
}

} // namespace

bool HasSubKeys(
    const RegistryNode& node
) {
  LiveKey key(node, KEY_QUERY_VALUE);
  return key && registry_backend::HasSubKeys(key);
}

bool QueryKeyInfo(
    const RegistryNode& node,
    KeyInfo* info
) {
  LiveKey key(node, KEY_READ);
  return key && registry_backend::QueryKeyInfo(key, info);
}

bool QuerySymbolicLinkTarget(
    const RegistryNode& node,
    std::wstring* target,
    bool* denied
) {
  target->clear();
  LONG error = ERROR_SUCCESS;
  LiveKey key(util::OpenNativeRegistryKey(registry_path::BuildNative(node), KEY_QUERY_VALUE, true, &error));
  if (denied) {
    *denied = error == ERROR_ACCESS_DENIED;
  }
  return key && ReadLinkTarget(key, target) && !target->empty();
}

std::vector<std::wstring> EnumSubKeyNames(
    const RegistryNode& node,
    bool sorted
) {
  LiveKey key(node, KEY_READ);
  return key ? SubKeyNames(key, sorted) : std::vector<std::wstring>();
}

bool EnumKeyStreaming(
    const RegistryNode& node,
    bool include_values,
    bool include_data,
    bool include_subkeys,
    RegistryStore::KeyEnumResult* out_info,
    const RegistryStore::ValueStreamCallback& value_callback,
    const RegistryStore::SubkeyStreamCallback& subkey_callback,
    DWORD max_data_size,
    EnumerationScratch* scratch,
    bool,
    bool open_link
) {
  LiveKey key(node, KEY_READ, open_link);
  return key && EnumerateKey(key, include_values, include_data, include_subkeys, out_info, value_callback, subkey_callback, max_data_size, scratch);
}

bool QueryValue(
    const RegistryNode& node,
    const std::wstring& value_name,
    ValueEntry* out
) {
  LiveKey key(node, KEY_QUERY_VALUE);
  return key && registry_backend::QueryValue(key, value_name, out);
}

bool CreateKey(
    const RegistryNode& node,
    const std::wstring& name
) {
  LiveKey parent(node, KEY_WRITE);
  util::UniqueHKey created;
  DWORD disposition = 0;
  return parent &&
         util::CreateRegistryKey(parent.get(), name, KEY_READ | KEY_WRITE, REG_OPTION_NON_VOLATILE, &created, &disposition) == ERROR_SUCCESS &&
         disposition == REG_CREATED_NEW_KEY;
}

bool CreateRegistryLink(
    const RegistryNode& node,
    const std::wstring& name,
    const std::wstring& nt_target,
    DWORD* error
) {
  LONG result = ERROR_ACCESS_DENIED;
  LiveKey parent(node, KEY_WRITE);
  util::UniqueHKey created;
  DWORD disposition = 0;
  if (parent) {
    result = util::CreateRegistryKey(parent.get(), name, KEY_SET_VALUE | KEY_CREATE_LINK | DELETE, REG_OPTION_NON_VOLATILE | REG_OPTION_CREATE_LINK, &created, &disposition);
  }
  if (result == ERROR_SUCCESS) {
    result = RegSetValueExW(created.get(), L"SymbolicLinkValue", 0, REG_LINK, reinterpret_cast<const BYTE*>(nt_target.c_str()), static_cast<DWORD>(nt_target.size() * sizeof(wchar_t)));
    if (result != ERROR_SUCCESS) {
      util::DeleteNativeRegistryKey(created.get());
    }
  }
  if (error) {
    *error = static_cast<DWORD>(result);
  }
  return result == ERROR_SUCCESS;
}

bool ReadKeyLink(
    const RegistryNode& node,
    std::wstring* target
) {
  std::wstring value;
  const LiveKey link = OpenChild(node, KEY_READ, KEY_QUERY_VALUE, true);
  if (!link || !ReadLinkTarget(link, &value)) {
    return false;
  }
  if (target) {
    *target = std::move(value);
  }
  return true;
}

bool ReadKeySecurity(
    const RegistryNode& node,
    std::vector<BYTE>* descriptor
) {
  descriptor->clear();
  LiveKey key(node, READ_CONTROL, true);
  return key && ReadSecurity(key, descriptor);
}

bool WriteKeySecurity(
    const RegistryNode& node,
    const std::vector<BYTE>& descriptor
) {
  LiveKey key(node, WRITE_DAC | WRITE_OWNER, true);
  return key && WriteSecurity(key, descriptor);
}

bool DeleteKey(
    const RegistryNode& node
) {
  const LiveKey target = OpenChild(node, KEY_ENUMERATE_SUB_KEYS, DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, true);
  return target && util::DeleteRegistryTree(target.get()) == ERROR_SUCCESS;
}

bool RenameKey(
    const RegistryNode& node,
    const std::wstring& new_name
) {
  RegistryNode parent_node;
  std::wstring old_name;
  if (!SplitNode(node, &parent_node, &old_name)) {
    return false;
  }
  LiveKey parent(parent_node, KEY_WRITE);
  return parent && util::RenameRegistryKey(parent.get(), old_name, new_name) == ERROR_SUCCESS;
}

bool DeleteValue(
    const RegistryNode& node,
    const std::wstring& value_name
) {
  LiveKey key(node, KEY_SET_VALUE);
  return key && key.DeleteValue(value_name.c_str()) == ERROR_SUCCESS;
}

bool SetValue(
    const RegistryNode& node,
    const std::wstring& value_name,
    DWORD type,
    const std::vector<BYTE>& data
) {
  LiveKey key(node, KEY_SET_VALUE);
  return key && key.SetValue(value_name.c_str(), type, data.empty() ? nullptr : data.data(), static_cast<DWORD>(data.size())) == ERROR_SUCCESS;
}

bool RenameValue(
    const RegistryNode& node,
    const std::wstring& old_name,
    const std::wstring& new_name,
    bool* both_names_left
) {
  LiveKey key(node, KEY_QUERY_VALUE | KEY_SET_VALUE);
  return key && registry_backend::RenameValue(key, old_name, new_name, both_names_left);
}

} // namespace regkit::registry_backend::live
