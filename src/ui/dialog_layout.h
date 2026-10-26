// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <win32ui/dialog/dialog_layout.h>
#include <win32ui/status_bar/status_bar.h>
#include <win32ui/tab_strip/tab_strip.h>

#include <windows.h>

namespace regkit::appearance
{

// RegKit dialog theme, lives for the whole run
const win32ui::DialogTheme* DialogThemeHooks();
// status bar theme, main window adds hooks to a copy
win32ui::StatusBarHooks StatusBarThemeHooks();
// tab strip theme, main window adds closing hooks to a copy
win32ui::TabStripHooks TabStripThemeHooks();
void ApplyDialogTheme(HWND dialog);

} // namespace regkit::appearance
