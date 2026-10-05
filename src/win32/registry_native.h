// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/handle_owner.h"

#include <optional>
#include <string>

namespace regkit::util
{

inline constexpr ULONG kKeyFlagVolatile = 0x1;
inline constexpr ULONG kKeyFlagLink = 0x2;

struct NativeKeyInfo
{
    std::wstring native_name;
    std::optional<ULONG> key_flags;
    std::optional<ULONG> control_flags;
    std::optional<ULONG> virtualization;
    std::optional<ULONG> trust;
    std::optional<ULONG> layer;
};

UniqueHKey OpenNativeRegistryKey(const std::wstring& path, REGSAM access, bool open_link = false, LONG* error = nullptr);
UniqueHKey OpenNativeRegistryRoot();
LONG OpenRegistryPath(HKEY root, const std::wstring& subkey, REGSAM access, bool open_link, UniqueHKey* key);
// while on, opens denied by a dacl are retried with the backup and restore privileges
void SetBackupRestoreMode(bool enable);
bool BackupRestoreMode();
LONG CreateRegistryKey(HKEY parent, const std::wstring& name, REGSAM access, DWORD options, UniqueHKey* key, DWORD* disposition, const std::wstring& class_name = {});
LONG RenameRegistryKey(HKEY parent, const std::wstring& old_name, const std::wstring& new_name);
LONG DeleteRegistryTree(HKEY key);
bool DeleteNativeRegistryKey(HKEY key);
std::optional<ULONG> QueryKeyFlags(HKEY key);
std::wstring QueryKeyName(HKEY key);
NativeKeyInfo QueryNativeKeyInfo(HKEY key);
LONG SetKeyLastWriteTime(HKEY key, const FILETIME& time);
LONG QueryValueCounted(HKEY key, const std::wstring& name, DWORD* type, BYTE* data, DWORD* size);
LONG SetValueCounted(HKEY key, const std::wstring& name, DWORD type, const BYTE* data, DWORD size);
LONG DeleteValueCounted(HKEY key, const std::wstring& name);
LONG ReadRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* value_name, std::wstring* value);
LONG WriteRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* value_name, const std::wstring& value);

} // namespace regkit::util
