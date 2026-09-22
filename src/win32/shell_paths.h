// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace util
{

std::wstring GetModuleDirectory();
std::wstring GetModulePath();
std::wstring JoinPath(const std::wstring& left, const std::wstring& right);
std::wstring GetAppDataFolder();
std::wstring GetCacheFolder();
bool HasFileExtension(std::wstring_view path, std::wstring_view extension);
std::wstring EnsureFileExtension(std::wstring path, std::wstring_view extension);

} // namespace util
