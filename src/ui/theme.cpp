// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/theme.h"

namespace regkit
{

void EnsureSubclass(HWND hwnd, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR data)
{
    if (hwnd && !GetWindowSubclass(hwnd, proc, id, nullptr))
    {
        SetWindowSubclass(hwnd, proc, id, data);
    }
}

} // namespace regkit
