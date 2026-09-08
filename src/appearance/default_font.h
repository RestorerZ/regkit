// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

namespace regkit::ui {

LOGFONTW DefaultUIFontLogFont();
LOGFONTW DefaultUIFontLogFont(UINT dpi);
HFONT DefaultUIFont();
HFONT DefaultUIFont(UINT dpi);

} // namespace regkit::ui
