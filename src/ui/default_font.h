// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

namespace regkit::ui
{

// the font every dialog uses, the system font unless a custom one is set
void SetCustomFont(const LOGFONTW* font);
LOGFONTW SystemUIFontLogFont();
LOGFONTW DefaultUIFontLogFont(UINT dpi);
HFONT DefaultUIFont(UINT dpi);

} // namespace regkit::ui
