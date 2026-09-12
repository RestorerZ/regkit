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

using NtOpenKey = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
using NtOpenKeyEx = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG);
using NtDeleteKey = NTSTATUS(NTAPI*)(HANDLE);

NtOpenKey ResolveNtOpenKey() {
  HMODULE module = GetModuleHandleW(L"ntdll.dll");
  return module
             ? reinterpret_cast<NtOpenKey>(GetProcAddress(module, "NtOpenKey"))
             : nullptr;
}

NtOpenKeyEx ResolveNtOpenKeyEx() {
  HMODULE module = GetModuleHandleW(L"ntdll.dll");
  return module ? reinterpret_cast<NtOpenKeyEx>(
                      GetProcAddress(module, "NtOpenKeyEx")
                  )
                : nullptr;
}

NtDeleteKey ResolveNtDeleteKey() {
  HMODULE module = GetModuleHandleW(L"ntdll.dll");
  return module ? reinterpret_cast<NtDeleteKey>(
                      GetProcAddress(module, "NtDeleteKey")
                  )
                : nullptr;
}

} // namespace

UniqueHKey OpenNativeRegistryKey(
    const std::wstring& path,
    REGSAM access,
    bool open_link
) {
  static const NtOpenKey open_key = ResolveNtOpenKey();
  static const NtOpenKeyEx open_key_ex = ResolveNtOpenKeyEx();
  if (path.empty() ||
      path.size() * sizeof(wchar_t) > (std::numeric_limits<USHORT>::max)()) {
    return {};
  }
  if (open_link && !open_key_ex) {
    return {};
  }
  if (!open_link && !open_key) {
    return {};
  }
  UNICODE_STRING name = {};
  name.Buffer = const_cast<PWSTR>(path.c_str());
  name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
  name.MaximumLength = name.Length;
  OBJECT_ATTRIBUTES attributes = {};
  const ULONG object_flags =
      OBJ_CASE_INSENSITIVE | (open_link ? OBJ_OPENLINK : 0ul);
  InitializeObjectAttributes(&attributes, &name, object_flags, nullptr, nullptr);
  HANDLE handle = nullptr;
  const NTSTATUS status =
      open_link ? open_key_ex(&handle, access, &attributes, REG_OPTION_OPEN_LINK)
                : open_key(&handle, access, &attributes);
  if (!NT_SUCCESS(status) || !handle) {
    return {};
  }
  return UniqueHKey(reinterpret_cast<HKEY>(handle));
}

UniqueHKey OpenNativeRegistryRoot() {
  return OpenNativeRegistryKey(L"\\REGISTRY", KEY_READ);
}

bool DeleteNativeRegistryKey(
    HKEY key
) {
  static const NtDeleteKey delete_key = ResolveNtDeleteKey();
  return key && delete_key &&
         NT_SUCCESS(delete_key(reinterpret_cast<HANDLE>(key)));
}

} // namespace util
