// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "registry/security_dialog.h"

#include "registry/registry_path.h"
#include "win32/process_rights.h"
#include "win32/registry_native.h"
#include "win32/registry_view.h"

#include <string>

#include <accctrl.h>
#include <aclapi.h>
#include <aclui.h>

namespace regkit {

namespace {

class RegistrySecurityInformation : public ISecurityInformation {
public:
  RegistrySecurityInformation(
      HKEY key,
      std::wstring object_name,
      bool read_only
  )
      : key_(key), object_name_(std::move(object_name)), read_only_(read_only) {
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(
      REFIID riid,
      void** ppv
  ) override {
    if (!ppv) {
      return E_POINTER;
    }
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_ISecurityInformation) {
      *ppv = static_cast<ISecurityInformation*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const LONG remaining = InterlockedDecrement(&references_);
    return static_cast<ULONG>(remaining < 0 ? 0 : remaining);
  }

  HRESULT STDMETHODCALLTYPE GetObjectInformation(
      PSI_OBJECT_INFO info
  ) override {
    if (!info) {
      return E_POINTER;
    }
    DWORD flags = SI_ADVANCED | SI_EDIT_OWNER | SI_EDIT_PERMS | SI_CONTAINER;
    if (read_only_) {
      flags |= SI_READONLY | SI_OWNER_READONLY;
    }
    info->dwFlags = flags;
    info->hInstance = nullptr;
    info->pszServerName = nullptr;
    info->pszObjectName = const_cast<wchar_t*>(object_name_.c_str());
    info->pszPageTitle = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetSecurity(
      SECURITY_INFORMATION security_info,
      PSECURITY_DESCRIPTOR* out_sd,
      BOOL
  ) override {
    if (!out_sd) {
      return E_POINTER;
    }
    *out_sd = nullptr;
    DWORD result = GetSecurityInfo(key_, SE_REGISTRY_KEY, security_info, nullptr, nullptr, nullptr, nullptr, out_sd);
    return HRESULT_FROM_WIN32(result);
  }

  HRESULT STDMETHODCALLTYPE SetSecurity(
      SECURITY_INFORMATION security_info,
      PSECURITY_DESCRIPTOR sd
  ) override {
    if (!sd) {
      return E_POINTER;
    }
    const LSTATUS status = RegSetKeySecurity(key_, security_info, sd);
    return status == ERROR_SUCCESS ? S_OK
                                   : HRESULT_FROM_WIN32(status);
  }

  HRESULT STDMETHODCALLTYPE GetAccessRights(
      const GUID*,
      DWORD,
      PSI_ACCESS* access,
      ULONG* count,
      ULONG* default_access
  ) override {
    static SI_ACCESS rights[] = {
        {&GUID_NULL, KEY_CREATE_SUB_KEY, const_cast<wchar_t*>(L"Create"), SI_ACCESS_SPECIFIC},
        {&GUID_NULL, KEY_ENUMERATE_SUB_KEYS, const_cast<wchar_t*>(L"Enumerate"), SI_ACCESS_SPECIFIC},
        {&GUID_NULL, KEY_SET_VALUE, const_cast<wchar_t*>(L"Set Value"), SI_ACCESS_SPECIFIC},
        {&GUID_NULL, KEY_QUERY_VALUE, const_cast<wchar_t*>(L"Query Value"), SI_ACCESS_SPECIFIC},
        {&GUID_NULL, KEY_WRITE, const_cast<wchar_t*>(L"Write"), SI_ACCESS_GENERAL},
        {&GUID_NULL, KEY_READ, const_cast<wchar_t*>(L"Read"), SI_ACCESS_GENERAL},
    };
    if (access) {
      *access = rights;
    }
    if (count) {
      *count = static_cast<ULONG>(_countof(rights));
    }
    if (default_access) {
      *default_access = 0;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE MapGeneric(
      const GUID*,
      UCHAR*,
      ACCESS_MASK* mask
  ) override {
    if (!mask) {
      return E_POINTER;
    }
    GENERIC_MAPPING mapping = {};
    mapping.GenericRead = KEY_READ;
    mapping.GenericWrite = KEY_WRITE;
    mapping.GenericExecute = KEY_EXECUTE;
    mapping.GenericAll = KEY_ALL_ACCESS;
    MapGenericMask(mask, &mapping);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetInheritTypes(
      PSI_INHERIT_TYPE* types,
      ULONG* count
  ) override {
    static SI_INHERIT_TYPE inherit_types[] = {
        {&GUID_NULL, 0, const_cast<wchar_t*>(L"This key only")},
        {&GUID_NULL, CONTAINER_INHERIT_ACE, const_cast<wchar_t*>(L"This key and subkeys")},
    };
    if (types) {
      *types = inherit_types;
    }
    if (count) {
      *count = static_cast<ULONG>(_countof(inherit_types));
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE PropertySheetPageCallback(
      HWND,
      UINT,
      SI_PAGE_TYPE
  ) override {
    return S_OK;
  }

private:
  HKEY key_ = nullptr;
  std::wstring object_name_;
  bool read_only_ = false;
  LONG references_ = 1;
};

} // namespace

bool ShowRegistryPermissions(
    HWND owner,
    const RegistryNode& node
) {
  std::wstring path = registry_path::Build(node);
  if (path.empty()) {
    return false;
  }

  const util::PrivilegeScope privilege({SE_TAKE_OWNERSHIP_NAME});
  bool read_only = false;
  util::UniqueHKey key;
  LONG result = util::OpenRegistryPath(node.root, node.subkey, READ_CONTROL | WRITE_DAC | WRITE_OWNER | win32::kDefaultRegistryView, false, &key);
  if (result == ERROR_ACCESS_DENIED) {
    read_only = true;
    result = util::OpenRegistryPath(node.root, node.subkey, READ_CONTROL | win32::kDefaultRegistryView, false, &key);
  }
  if (result == ERROR_ACCESS_DENIED) {
    result = util::OpenRegistryPath(node.root, node.subkey, MAXIMUM_ALLOWED | win32::kDefaultRegistryView, false, &key);
  }

  bool ok = false;
  if (result == ERROR_SUCCESS && key.get()) {
    RegistrySecurityInformation info(key.get(), path, read_only);
    ok = SUCCEEDED(EditSecurity(owner, &info));
  }
  return ok;
}

} // namespace regkit
