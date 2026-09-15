// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/handle_owner.h"

#include <string>

namespace util {

UniqueHKey OpenNativeRegistryKey(const std::wstring& path, REGSAM access, bool open_link = false);
UniqueHKey OpenNativeRegistryRoot();
LONG OpenRegistryPath(HKEY root, const std::wstring& subkey, REGSAM access, bool open_link, UniqueHKey* key);
LONG CreateRegistryKey(HKEY parent, const std::wstring& name, REGSAM access, DWORD options, UniqueHKey* key, DWORD* disposition);
LONG RenameRegistryKey(HKEY parent, const std::wstring& old_name, const std::wstring& new_name);
LONG DeleteRegistryTree(HKEY key);
bool DeleteNativeRegistryKey(HKEY key);
bool ReadRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* value_name, std::wstring* value);
LONG WriteRegistryString(HKEY key, const wchar_t* value_name, const std::wstring& value);

} // namespace util
