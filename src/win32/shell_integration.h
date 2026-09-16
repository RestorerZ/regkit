// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <string>

namespace regkit::win32 {

bool IsRegFileEditMenuRegistered(const std::wstring& exe_path);
LONG SetRegFileEditMenu(const std::wstring& exe_path, bool enable, LONG* cleanup_error = nullptr);
LONG RemoveRegFileEditMenuIfOwned(const std::wstring& exe_path);
bool IsRegEditReplacementRegistered(const std::wstring& exe_path);
LONG SetRegEditReplacement(
    const std::wstring& exe_path,
    bool enable,
    bool* conflict = nullptr,
    bool overwrite_existing = false
);

} // namespace regkit::win32
