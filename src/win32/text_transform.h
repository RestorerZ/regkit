// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace util {

std::wstring WindowText(HWND window);
std::wstring ToLower(std::wstring_view text);
std::wstring TrimWhitespace(const std::wstring& text);
std::wstring ExpandEnvironmentStringsDynamic(const std::wstring& text);
std::wstring ToHex(const BYTE* data, size_t size, size_t max_bytes);

} // namespace util
