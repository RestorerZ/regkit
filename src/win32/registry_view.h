// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

namespace regkit::win32
{

inline constexpr REGSAM kDefaultRegistryView = KEY_WOW64_64KEY;
inline constexpr REGSAM kAlternateRegistryView = KEY_WOW64_32KEY;

// 64-bit windows keeps a separate registry view for 32-bit programs
inline bool HasAlternateView()
{
    SYSTEM_INFO info = {};
    GetNativeSystemInfo(&info);
    return info.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_INTEL;
}

inline const wchar_t* RegExeViewSwitch(REGSAM view)
{
    if (!HasAlternateView())
    {
        return L"";
    }
    return view == kAlternateRegistryView ? L"/reg:32" : L"/reg:64";
}

} // namespace regkit::win32
