// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <string>
#include <vector>

namespace regkit::key_access
{

struct RunLevel
{
    const wchar_t* name = nullptr;
    ACCESS_MASK granted = 0;
};

// access a principal gets from the descriptor, evaluated with authz
ACCESS_MASK ForSid(PSECURITY_DESCRIPTOR descriptor, PSID sid);
// current, administrator (when not elevated), SYSTEM & TrustedInstaller, the last two are modeled from their groups
std::vector<RunLevel> ForRunLevels(PSECURITY_DESCRIPTOR descriptor);
std::wstring Describe(ACCESS_MASK granted);
bool CanWrite(ACCESS_MASK granted) noexcept;

} // namespace regkit::key_access
