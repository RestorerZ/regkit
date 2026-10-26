// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <string>

namespace regkit::util
{

std::wstring FormatWin32Error(DWORD code);

} // namespace regkit::util
