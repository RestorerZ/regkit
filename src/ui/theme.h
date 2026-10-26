// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <win32ui/theme/theme.h>

#include <windows.h>

#include <commctrl.h>

namespace regkit
{

void EnsureSubclass(HWND hwnd, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR data = 0);

} // namespace regkit
