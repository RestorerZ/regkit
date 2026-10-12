// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

namespace regkit::win32
{

template <typename Fn>
Fn ImportProc(HMODULE module, const char* name) noexcept
{
    return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}

template <typename Fn>
Fn ImportProc(const wchar_t* module, const char* name) noexcept
{
    return ImportProc<Fn>(GetModuleHandleW(module), name);
}

inline const RTL_OSVERSIONINFOW& OsVersion() noexcept
{
    static const RTL_OSVERSIONINFOW version = [] {
        RTL_OSVERSIONINFOW info = {};
        info.dwOSVersionInfoSize = sizeof(info);
        const auto get_version = ImportProc<LONG(WINAPI*)(RTL_OSVERSIONINFOW*)>(L"ntdll.dll", "RtlGetVersion");
        if (!get_version || get_version(&info) != 0)
        {
            info = {};
        }
        return info;
    }();
    return version;
}

} // namespace regkit::win32
