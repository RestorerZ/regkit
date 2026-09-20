// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "registry/registry_backends.h"

#include "registry/key_algorithms.h"
#include "win32/handle_owner.h"
#include "win32/shell_paths.h"
#include "win32/system_error.h"

#include <algorithm>
#include <iterator>
#include <vector>

#include <winternl.h>

namespace regkit::registry_backend::offline {
namespace {

using ORHKEY = void*;
using OROpenHiveFn = DWORD(WINAPI*)(PCWSTR, ORHKEY*);
using ORCloseHiveFn = DWORD(WINAPI*)(ORHKEY);
using ORSaveHiveFn = DWORD(WINAPI*)(ORHKEY, PCWSTR, DWORD, DWORD);
using OROpenKeyFn = DWORD(WINAPI*)(ORHKEY, PCWSTR, ORHKEY*);
using ORCloseKeyFn = DWORD(WINAPI*)(ORHKEY);
using ORCreateKeyFn = DWORD(WINAPI*)(
    ORHKEY,
    PCWSTR,
    PWSTR,
    DWORD,
    PSECURITY_DESCRIPTOR,
    ORHKEY*,
    DWORD*
);
using ORDeleteKeyFn = DWORD(WINAPI*)(ORHKEY, PCWSTR);
using ORQueryInfoKeyFn = DWORD(WINAPI*)(
    ORHKEY,
    PWSTR,
    DWORD*,
    DWORD*,
    DWORD*,
    DWORD*,
    DWORD*,
    DWORD*,
    DWORD*,
    DWORD*,
    FILETIME*
);
using OREnumKeyFn =
    DWORD(WINAPI*)(ORHKEY, DWORD, PWSTR, DWORD*, PWSTR, DWORD*, FILETIME*);
using ORGetValueFn =
    DWORD(WINAPI*)(ORHKEY, PCWSTR, PCWSTR, DWORD*, void*, DWORD*);
using ORSetValueFn =
    DWORD(WINAPI*)(ORHKEY, PCWSTR, DWORD, const BYTE*, DWORD);
using ORDeleteValueFn = DWORD(WINAPI*)(ORHKEY, PCWSTR);
using OREnumValueFn =
    DWORD(WINAPI*)(ORHKEY, DWORD, PWSTR, DWORD*, DWORD*, BYTE*, DWORD*);
using ORRenameKeyFn = DWORD(WINAPI*)(ORHKEY, PCWSTR);
using ORGetKeySecurityFn =
    DWORD(WINAPI*)(ORHKEY, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, DWORD*);
using ORSetKeySecurityFn =
    DWORD(WINAPI*)(ORHKEY, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR);

template <typename Function>
Function LoadFunction(
    HMODULE module,
    const char* name
) {
  return reinterpret_cast<Function>(GetProcAddress(module, name));
}

class OffregApi {
public:
  OffregApi() {
    const std::wstring directory = util::GetModuleDirectory();
    const std::wstring path =
        directory.empty() ? std::wstring() : util::JoinPath(directory, L"offreg.dll");
    const DWORD attributes = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      load_error_ = path.empty() ? ERROR_MOD_NOT_FOUND : attributes == INVALID_FILE_ATTRIBUTES ? GetLastError()
                                                                                               : ERROR_ACCESS_DENIED;
      return;
    }
    module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module_ && GetLastError() == ERROR_INVALID_PARAMETER) {
      module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!module_) {
      load_error_ = GetLastError();
      return;
    }
    open_hive = LoadFunction<OROpenHiveFn>(module_, "OROpenHive");
    close_hive = LoadFunction<ORCloseHiveFn>(module_, "ORCloseHive");
    save_hive = LoadFunction<ORSaveHiveFn>(module_, "ORSaveHive");
    open_key = LoadFunction<OROpenKeyFn>(module_, "OROpenKey");
    close_key = LoadFunction<ORCloseKeyFn>(module_, "ORCloseKey");
    create_key = LoadFunction<ORCreateKeyFn>(module_, "ORCreateKey");
    delete_key = LoadFunction<ORDeleteKeyFn>(module_, "ORDeleteKey");
    query_info = LoadFunction<ORQueryInfoKeyFn>(module_, "ORQueryInfoKey");
    enum_key = LoadFunction<OREnumKeyFn>(module_, "OREnumKey");
    get_value = LoadFunction<ORGetValueFn>(module_, "ORGetValue");
    set_value = LoadFunction<ORSetValueFn>(module_, "ORSetValue");
    delete_value = LoadFunction<ORDeleteValueFn>(module_, "ORDeleteValue");
    enum_value = LoadFunction<OREnumValueFn>(module_, "OREnumValue");
    rename_key = LoadFunction<ORRenameKeyFn>(module_, "ORRenameKey");
    get_key_security =
        LoadFunction<ORGetKeySecurityFn>(module_, "ORGetKeySecurity");
    set_key_security =
        LoadFunction<ORSetKeySecurityFn>(module_, "ORSetKeySecurity");
    if (!valid()) {
      load_error_ = ERROR_PROC_NOT_FOUND;
      FreeLibrary(module_);
      module_ = nullptr;
    }
  }

  ~OffregApi() {
    if (module_) {
      FreeLibrary(module_);
    }
  }

  OffregApi(const OffregApi&) = delete;
  OffregApi& operator=(const OffregApi&) = delete;

  bool valid() const noexcept {
    return module_ && open_hive && close_hive && save_hive && open_key &&
           close_key && create_key && delete_key && query_info && enum_key &&
           get_value && set_value && delete_value && enum_value && rename_key;
  }

  OROpenHiveFn open_hive = nullptr;
  ORCloseHiveFn close_hive = nullptr;
  ORSaveHiveFn save_hive = nullptr;
  OROpenKeyFn open_key = nullptr;
  ORCloseKeyFn close_key = nullptr;
  ORCreateKeyFn create_key = nullptr;
  ORDeleteKeyFn delete_key = nullptr;
  ORQueryInfoKeyFn query_info = nullptr;
  OREnumKeyFn enum_key = nullptr;
  ORGetValueFn get_value = nullptr;
  ORSetValueFn set_value = nullptr;
  ORDeleteValueFn delete_value = nullptr;
  OREnumValueFn enum_value = nullptr;
  ORRenameKeyFn rename_key = nullptr;
  ORGetKeySecurityFn get_key_security = nullptr;
  ORSetKeySecurityFn set_key_security = nullptr;

  DWORD load_error() const noexcept {
    return load_error_;
  }

private:
  HMODULE module_ = nullptr;
  DWORD load_error_ = ERROR_SUCCESS;
};

OffregApi& OffregInstance() {
  static OffregApi api;
  return api;
}

OffregApi* Api() {
  OffregApi& api = OffregInstance();
  return api.valid() ? &api : nullptr;
}

std::wstring OffregLoadFailure() {
  std::wstring message = L"offreg.dll couldn't be loaded.";
  const std::wstring detail =
      util::FormatWin32Error(OffregInstance().load_error());
  if (!detail.empty()) {
    message += L"\n";
    message += detail;
  }
  return message;
}

struct CloseOfflineKey {
  void operator()(ORHKEY key) const noexcept {
    OffregInstance().close_key(key);
  }
};

class OfflineKey {
public:
  OfflineKey(
      const RegistryNode& node
  )
      : api_(Api()) {
    ORHKEY root = reinterpret_cast<ORHKEY>(node.root);
    if (!api_ || !root) {
      return;
    }
    if (node.subkey.empty()) {
      // hive roots stay owned by the caller while opened subkeys are owned here
      key_ = root;
    } else if (api_->open_key(root, node.subkey.c_str(), owner_.put()) == ERROR_SUCCESS) {
      key_ = owner_.get();
    }
  }
  OfflineKey(
      OffregApi* api,
      ORHKEY parent,
      const std::wstring& name
  )
      : api_(api) {
    if (api_->open_key(parent, name.c_str(), owner_.put()) == ERROR_SUCCESS) {
      key_ = owner_.get();
    }
  }

  explicit operator bool() const noexcept {
    return key_ != nullptr;
  }
  ORHKEY get() const noexcept {
    return key_;
  }
  OffregApi& api() const noexcept {
    return *api_;
  }

  LONG QueryInfo(DWORD* subkeys, DWORD* max_subkey_length, DWORD* values, DWORD* max_value_name_length, DWORD* max_value_data_length, FILETIME* last_write) const {
    return static_cast<LONG>(api_->query_info(key_, nullptr, nullptr, subkeys, max_subkey_length, nullptr, values, max_value_name_length, max_value_data_length, nullptr, last_write));
  }
  LONG EnumKey(DWORD index, wchar_t* name, DWORD* length) const {
    return static_cast<LONG>(api_->enum_key(key_, index, name, length, nullptr, nullptr, nullptr));
  }
  LONG EnumValue(DWORD index, wchar_t* name, DWORD* name_length, DWORD* type, BYTE* data, DWORD* data_length) const {
    return static_cast<LONG>(api_->enum_value(key_, index, name, name_length, type, data, data_length));
  }
  LONG GetValue(const wchar_t* name, DWORD* type, BYTE* data, DWORD* size) const {
    return static_cast<LONG>(api_->get_value(key_, nullptr, name, type, data, size));
  }
  LONG SetValue(const wchar_t* name, DWORD type, const BYTE* data, DWORD size) const {
    return static_cast<LONG>(api_->set_value(key_, name, type, data, size));
  }
  LONG DeleteValue(const wchar_t* name) const {
    return static_cast<LONG>(api_->delete_value(key_, name));
  }
  LONG GetSecurity(SECURITY_INFORMATION information, PSECURITY_DESCRIPTOR descriptor, DWORD* size) const {
    return api_->get_key_security ? static_cast<LONG>(api_->get_key_security(key_, information, descriptor, size)) : ERROR_CALL_NOT_IMPLEMENTED;
  }
  LONG SetSecurity(SECURITY_INFORMATION information, PSECURITY_DESCRIPTOR descriptor) const {
    return api_->set_key_security ? static_cast<LONG>(api_->set_key_security(key_, information, descriptor)) : ERROR_CALL_NOT_IMPLEMENTED;
  }

private:
  OffregApi* api_ = nullptr;
  ORHKEY key_ = nullptr;
  util::UniqueResource<ORHKEY, CloseOfflineKey> owner_;
};

std::vector<HKEY> g_roots;

bool DeleteSubtree(
    const OfflineKey& parent,
    const std::wstring& name
) {
  // close child handle before deleting its now empty key
  {
    const OfflineKey child(&parent.api(), parent.get(), name);
    if (!child) {
      return false;
    }
    for (const std::wstring& child_name : SubKeyNames(child, false)) {
      if (!DeleteSubtree(child, child_name)) {
        return false;
      }
    }
  }
  return parent.api().delete_key(parent.get(), name.c_str()) == ERROR_SUCCESS;
}

template <typename Action>
bool WithApi(
    std::wstring* error,
    Action&& action
) {
  if (error) {
    error->clear();
  }
  OffregApi* api = Api();
  if (!api) {
    // report missing/incomplete offreg support
    if (error) {
      *error = OffregLoadFailure();
    }
    return false;
  }
  const DWORD result = action(*api);
  if (result != ERROR_SUCCESS && error) {
    *error = util::FormatWin32Error(result);
  }
  return result == ERROR_SUCCESS;
}

void OsVersion(
    DWORD* major,
    DWORD* minor
) {
  using RtlGetVersionFn = NTSTATUS(WINAPI*)(PRTL_OSVERSIONINFOW);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto get_version = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
  RTL_OSVERSIONINFOW version = {};
  version.dwOSVersionInfoSize = sizeof(version);
  if (get_version && get_version(&version) == 0) {
    *major = version.dwMajorVersion;
    *minor = version.dwMinorVersion;
  }
}

} // namespace

bool OpenHive(
    const std::wstring& path,
    HKEY* root,
    std::wstring* error
) {
  *root = nullptr;
  return WithApi(error, [&](OffregApi& api) {
    ORHKEY hive = nullptr;
    DWORD result = api.open_hive(path.c_str(), &hive);
    // reject a success result that didnt return a usable root
    if (result == ERROR_SUCCESS && !hive) {
      result = ERROR_INVALID_HANDLE;
    }
    *root = reinterpret_cast<HKEY>(hive);
    return result;
  });
}

bool SaveHive(
    HKEY root,
    const std::wstring& path,
    std::wstring* error
) {
  return root && WithApi(error, [&](OffregApi& api) {
           DWORD major = 10;
           DWORD minor = 0;
           OsVersion(&major, &minor);
           return api.save_hive(reinterpret_cast<ORHKEY>(root), path.c_str(), major, minor);
         });
}

bool CloseHive(
    HKEY root,
    std::wstring* error
) {
  return !root || WithApi(error, [&](OffregApi& api) { return api.close_hive(reinterpret_cast<ORHKEY>(root)); });
}

void SetRoots(
    const std::vector<HKEY>& roots
) {
  g_roots.clear();
  std::copy_if(roots.begin(), roots.end(), std::back_inserter(g_roots), [](HKEY root) { return root != nullptr; });
}

void AddRoot(
    HKEY root
) {
  if (root && !Owns(root)) {
    g_roots.push_back(root);
  }
}

void RemoveRoot(
    HKEY root
) {
  g_roots.erase(std::remove(g_roots.begin(), g_roots.end(), root), g_roots.end());
}

bool Owns(
    HKEY root
) {
  return root && std::find(g_roots.begin(), g_roots.end(), root) != g_roots.end();
}

bool HasSubKeys(
    const RegistryNode& node
) {
  const OfflineKey key(node);
  return key && registry_backend::HasSubKeys(key);
}

bool QueryKeyInfo(
    const RegistryNode& node,
    KeyInfo* info
) {
  const OfflineKey key(node);
  return key && registry_backend::QueryKeyInfo(key, info);
}

bool QuerySymbolicLinkTarget(
    const RegistryNode& node,
    std::wstring* target
) {
  target->clear();
  const OfflineKey key(node);
  return key && ReadLinkTarget(key, target) && !target->empty();
}

std::vector<std::wstring> EnumSubKeyNames(
    const RegistryNode& node,
    bool sorted
) {
  const OfflineKey key(node);
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
    bool
) {
  const OfflineKey key(node);
  return key && EnumerateKey(key, include_values, include_data, include_subkeys, out_info, value_callback, subkey_callback, max_data_size, scratch);
}

bool QueryValue(
    const RegistryNode& node,
    const std::wstring& value_name,
    ValueEntry* out
) {
  const OfflineKey key(node);
  return key && registry_backend::QueryValue(key, value_name, out);
}

bool CreateKey(
    const RegistryNode& node,
    const std::wstring& name
) {
  const OfflineKey parent(node);
  if (!parent) {
    return false;
  }
  util::UniqueResource<ORHKEY, CloseOfflineKey> created;
  DWORD disposition = 0;
  // dont report an existing key as newly created
  return parent.api().create_key(parent.get(), name.c_str(), nullptr, 0, nullptr, created.put(), &disposition) == ERROR_SUCCESS &&
         disposition == REG_CREATED_NEW_KEY;
}

bool ReadKeySecurity(
    const RegistryNode& node,
    std::vector<BYTE>* descriptor
) {
  descriptor->clear();
  const OfflineKey key(node);
  return key && ReadSecurity(key, descriptor);
}

bool WriteKeySecurity(
    const RegistryNode& node,
    const std::vector<BYTE>& descriptor
) {
  const OfflineKey key(node);
  return key && WriteSecurity(key, descriptor);
}

bool DeleteKey(
    const RegistryNode& node
) {
  RegistryNode parent_node;
  std::wstring name;
  if (!SplitNode(node, &parent_node, &name)) {
    return false;
  }
  const OfflineKey parent(parent_node);
  return parent && DeleteSubtree(parent, name);
}

bool RenameKey(
    const RegistryNode& node,
    const std::wstring& new_name
) {
  const OfflineKey key(node);
  return key && key.api().rename_key(key.get(), new_name.c_str()) == ERROR_SUCCESS;
}

bool DeleteValue(
    const RegistryNode& node,
    const std::wstring& value_name
) {
  const OfflineKey key(node);
  return key && key.DeleteValue(ValueNameArg(value_name)) == ERROR_SUCCESS;
}

bool SetValue(
    const RegistryNode& node,
    const std::wstring& value_name,
    DWORD type,
    const std::vector<BYTE>& data
) {
  const OfflineKey key(node);
  return key && key.SetValue(ValueNameArg(value_name), type, data.empty() ? nullptr : data.data(), static_cast<DWORD>(data.size())) == ERROR_SUCCESS;
}

bool RenameValue(
    const RegistryNode& node,
    const std::wstring& old_name,
    const std::wstring& new_name,
    bool* both_names_left
) {
  const OfflineKey key(node);
  return key && registry_backend::RenameValue(key, old_name, new_name, both_names_left);
}

} // namespace regkit::registry_backend::offline
