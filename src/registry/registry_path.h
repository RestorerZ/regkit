// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace regkit
{

struct RegistryNode;

namespace registry_path
{

enum class Style
{
    kFull,
    kAbbreviated,
    kRegEditAddress,
    kRegFileHeader,
    kPowerShellDrive,
    kPowerShellProvider,
    kEscaped,
};

inline constexpr wchar_t kNullSymbol = 0x2400;

std::wstring DisplayName(std::wstring_view name);
std::wstring RawName(std::wstring_view text);
HKEY RootFromName(std::wstring_view name);
std::wstring RootName(HKEY root);
std::wstring Build(const RegistryNode& node);
std::wstring BuildNative(const RegistryNode& node);

std::wstring Clean(std::wstring_view path);
std::wstring Normalize(std::wstring_view path, std::wstring_view current_user_sid = {});
std::wstring Format(std::wstring_view normalized_path, Style style, std::wstring_view tree_root = L"Computer");
bool ParseRoot(std::wstring_view path, RegistryNode* node);
std::vector<std::wstring> Split(std::wstring_view path);
std::wstring Join(const std::vector<std::wstring>& parts, size_t first_part = 0);
std::wstring JoinPrefix(const std::vector<std::wstring>& parts, size_t part_count);
std::wstring JoinSubkey(std::wstring_view parent, std::wstring_view name);
RegistryNode ChildNode(const RegistryNode& parent, std::wstring_view name);
std::wstring Parent(std::wstring_view path);
std::wstring Leaf(std::wstring_view path);

} // namespace registry_path
} // namespace regkit
