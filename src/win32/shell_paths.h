// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace regkit::util
{

std::wstring GetModuleDirectory();
std::wstring GetModulePath();
std::wstring JoinPath(const std::wstring& left, const std::wstring& right);
std::wstring FileName(std::wstring_view path);
std::wstring FileBaseName(std::wstring_view path);
std::wstring TrimTrailingSeparators(std::wstring path);
bool IsFile(const std::wstring& path);
bool IsDirectory(const std::wstring& path);
bool IsMissing(const std::wstring& path);
std::wstring GetAppDataFolder();
std::wstring GetCacheFolder();
bool HasFileExtension(std::wstring_view path, std::wstring_view extension);
std::wstring EnsureFileExtension(std::wstring path, std::wstring_view extension);

} // namespace regkit::util
