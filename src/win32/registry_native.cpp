// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "win32/registry_native.h"

#include <winternl.h>

#include <limits>

namespace util {
namespace {

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef OBJ_OPENLINK
#define OBJ_OPENLINK 0x00000100L
#endif

using NtOpenKeyFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
using NtOpenKeyExFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG);
using NtCreateKeyFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, PULONG);
using NtRenameKeyFn = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING);
using NtDeleteKeyFn = NTSTATUS(NTAPI*)(HANDLE);
using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(NTSTATUS);

constexpr REGSAM kViewFlags = KEY_WOW64_32KEY | KEY_WOW64_64KEY;

template <typename Function>
Function Ntdll(
    const char* name
) {
  static const HMODULE module = GetModuleHandleW(L"ntdll.dll");
  return module ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
}

LONG DosError(
    NTSTATUS status
) {
  static const auto convert = Ntdll<RtlNtStatusToDosErrorFn>("RtlNtStatusToDosError");
  return NT_SUCCESS(status) ? ERROR_SUCCESS : static_cast<LONG>(convert ? convert(status) : ERROR_GEN_FAILURE);
}

HANDLE RootHandle(
    HKEY key
) {
  return reinterpret_cast<HANDLE>(reinterpret_cast<ULONG_PTR>(key) & ~static_cast<ULONG_PTR>(3));
}

bool CountedName(
    const std::wstring& text,
    UNICODE_STRING* name
) {
  if (text.size() * sizeof(wchar_t) > (std::numeric_limits<USHORT>::max)()) {
    return false;
  }
  name->Buffer = const_cast<PWSTR>(text.c_str());
  name->Length = static_cast<USHORT>(text.size() * sizeof(wchar_t));
  name->MaximumLength = name->Length;
  return true;
}

LONG OpenNative(
    HKEY parent,
    const std::wstring& path,
    REGSAM access,
    bool open_link,
    UniqueHKey* key
) {
  static const auto open_key = Ntdll<NtOpenKeyFn>("NtOpenKey");
  static const auto open_key_ex = Ntdll<NtOpenKeyExFn>("NtOpenKeyEx");
  UNICODE_STRING name = {};
  if (path.empty() || !CountedName(path, &name)) {
    return ERROR_INVALID_PARAMETER;
  }
  if (open_link ? !open_key_ex : !open_key) {
    return ERROR_CALL_NOT_IMPLEMENTED;
  }
  OBJECT_ATTRIBUTES attributes = {};
  InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | (open_link ? OBJ_OPENLINK : 0ul), parent ? RootHandle(parent) : nullptr, nullptr);
  HANDLE handle = nullptr;
  const NTSTATUS status = open_link
                              ? open_key_ex(&handle, access & ~kViewFlags, &attributes, REG_OPTION_OPEN_LINK)
                              : open_key(&handle, access & ~kViewFlags, &attributes);
  if (NT_SUCCESS(status)) {
    key->reset(reinterpret_cast<HKEY>(handle));
  }
  return DosError(status);
}

} // namespace

UniqueHKey OpenNativeRegistryKey(
    const std::wstring& path,
    REGSAM access,
    bool open_link
) {
  UniqueHKey key;
  OpenNative(nullptr, path, access, open_link, &key);
  return key;
}

UniqueHKey OpenNativeRegistryRoot() {
  return OpenNativeRegistryKey(L"\\REGISTRY", KEY_READ);
}

LONG OpenRegistryPath(
    HKEY root,
    const std::wstring& subkey,
    REGSAM access,
    bool open_link,
    UniqueHKey* key
) {
  key->reset();
  const size_t null_char = subkey.find(L'\0');
  if (null_char == std::wstring::npos) {
    return RegOpenKeyExW(root, subkey.empty() ? nullptr : subkey.c_str(), open_link ? REG_OPTION_OPEN_LINK : 0, access, key->put());
  }
  const size_t split = subkey.rfind(L'\\', null_char);
  const std::wstring prefix = split == std::wstring::npos ? std::wstring() : subkey.substr(0, split);
  UniqueHKey parent;
  const LONG result = RegOpenKeyExW(root, prefix.c_str(), 0, MAXIMUM_ALLOWED | (access & kViewFlags), parent.put());
  if (result != ERROR_SUCCESS) {
    return result;
  }
  return OpenNative(parent.get(), split == std::wstring::npos ? subkey : subkey.substr(split + 1), access, open_link, key);
}

LONG CreateRegistryKey(
    HKEY parent,
    const std::wstring& name,
    REGSAM access,
    DWORD options,
    UniqueHKey* key,
    DWORD* disposition
) {
  key->reset();
  if (name.find(L'\0') == std::wstring::npos) {
    return RegCreateKeyExW(parent, name.c_str(), 0, nullptr, options, access, nullptr, key->put(), disposition);
  }
  static const auto create_key = Ntdll<NtCreateKeyFn>("NtCreateKey");
  UNICODE_STRING counted = {};
  if (!create_key || !CountedName(name, &counted)) {
    return ERROR_INVALID_PARAMETER;
  }
  OBJECT_ATTRIBUTES attributes = {};
  InitializeObjectAttributes(&attributes, &counted, OBJ_CASE_INSENSITIVE | ((options & REG_OPTION_CREATE_LINK) ? OBJ_OPENLINK : 0ul), RootHandle(parent), nullptr);
  HANDLE handle = nullptr;
  ULONG created = 0;
  const NTSTATUS status = create_key(&handle, access & ~kViewFlags, &attributes, 0, nullptr, options, &created);
  if (NT_SUCCESS(status)) {
    key->reset(reinterpret_cast<HKEY>(handle));
    if (disposition) {
      *disposition = created;
    }
  }
  return DosError(status);
}

LONG RenameRegistryKey(
    HKEY parent,
    const std::wstring& old_name,
    const std::wstring& new_name
) {
  if (old_name.find(L'\0') == std::wstring::npos && new_name.find(L'\0') == std::wstring::npos) {
    return RegRenameKey(parent, old_name.c_str(), new_name.c_str());
  }
  static const auto rename_key = Ntdll<NtRenameKeyFn>("NtRenameKey");
  UniqueHKey key;
  LONG result = OpenRegistryPath(parent, old_name, KEY_WRITE, false, &key);
  UNICODE_STRING counted = {};
  if (result == ERROR_SUCCESS && (!rename_key || !CountedName(new_name, &counted))) {
    result = ERROR_INVALID_PARAMETER;
  }
  return result == ERROR_SUCCESS ? DosError(rename_key(reinterpret_cast<HANDLE>(key.get()), &counted)) : result;
}

LONG DeleteRegistryTree(
    HKEY key
) {
  static const auto delete_key = Ntdll<NtDeleteKeyFn>("NtDeleteKey");
  if (!key || !delete_key) {
    return ERROR_INVALID_PARAMETER;
  }
  wchar_t name[256] = {};
  while (true) {
    DWORD length = static_cast<DWORD>(_countof(name));
    const LONG result = RegEnumKeyExW(key, 0, name, &length, nullptr, nullptr, nullptr, nullptr);
    if (result == ERROR_NO_MORE_ITEMS) {
      break;
    }
    UniqueHKey child;
    LONG removed = result == ERROR_SUCCESS
                       ? OpenRegistryPath(key, std::wstring(name, length), DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, true, &child)
                       : result;
    if (removed == ERROR_SUCCESS) {
      removed = DeleteRegistryTree(child.get());
    }
    if (removed != ERROR_SUCCESS) {
      return removed;
    }
  }
  return DosError(delete_key(reinterpret_cast<HANDLE>(key)));
}

bool DeleteNativeRegistryKey(
    HKEY key
) {
  static const auto delete_key = Ntdll<NtDeleteKeyFn>("NtDeleteKey");
  return key && delete_key && NT_SUCCESS(delete_key(reinterpret_cast<HANDLE>(key)));
}

bool ReadRegistryString(
    HKEY root,
    const wchar_t* subkey,
    const wchar_t* value_name,
    std::wstring* value
) {
  if (!value) {
    return false;
  }
  value->clear();
  constexpr DWORD kMaximumBytes = 16u * 1024u * 1024u;
  for (int attempt = 0; attempt < 3; ++attempt) {
    DWORD size = 0;
    LONG result = RegGetValueW(root, subkey, value_name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &size);
    if (result != ERROR_SUCCESS || size < sizeof(wchar_t) ||
        size > kMaximumBytes || size % sizeof(wchar_t) != 0) {
      return false;
    }
    value->resize(size / sizeof(wchar_t));
    result = RegGetValueW(root, subkey, value_name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value->data(), &size);
    if (result == ERROR_MORE_DATA) {
      continue;
    }
    if (result != ERROR_SUCCESS) {
      value->clear();
      return false;
    }
    while (!value->empty() && value->back() == L'\0') {
      value->pop_back();
    }
    return true;
  }
  value->clear();
  return false;
}

LONG WriteRegistryString(
    HKEY key,
    const wchar_t* value_name,
    const std::wstring& value
) {
  if (!key) {
    return ERROR_INVALID_HANDLE;
  }
  if (value.size() >=
      (std::numeric_limits<DWORD>::max)() / sizeof(wchar_t)) {
    return ERROR_INVALID_DATA;
  }
  return RegSetValueExW(
      key,
      value_name,
      0,
      REG_SZ,
      reinterpret_cast<const BYTE*>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))
  );
}

} // namespace util
