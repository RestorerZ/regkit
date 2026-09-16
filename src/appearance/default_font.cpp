// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "appearance/default_font.h"

#include "appearance/font_metrics.h"
#include "win32/registry_native.h"
#include "win32/shell_paths.h"
#include "workspace/settings.h"

#include <string>

namespace regkit::ui {
LOGFONTW DefaultUIFontLogFont(
    UINT dpi
) {
  LOGFONTW lf = {};
  HFONT stock = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  if (!stock || GetObjectW(stock, sizeof(lf), &lf) == 0) {
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
  }
  // 9 point Segoe UI, windows/regit can override it
  lf.lfHeight = appearance::FontHeight(9, dpi);
  std::wstring face;
  if (util::ReadRegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes", L"Segoe UI", &face) != ERROR_SUCCESS || face.empty()) {
    face = L"Segoe UI";
  }
  workspace::Settings settings;
  const std::wstring folder = util::GetAppDataFolder();
  if (!folder.empty() && workspace::LoadSettings(util::JoinPath(folder, L"settings.ini"), &settings) && settings.use_custom_font) {
    if (!settings.font_face.empty()) {
      face = settings.font_face;
    }
    if (settings.font_size > 0) {
      lf.lfHeight = appearance::FontHeight(settings.font_size, dpi);
    }
    lf.lfWeight = settings.font_weight;
    lf.lfItalic = settings.font_italic ? TRUE : FALSE;
  }
  wcsncpy_s(lf.lfFaceName, face.c_str(), _TRUNCATE);
  return lf;
}

LOGFONTW DefaultUIFontLogFont() {
  return DefaultUIFontLogFont(static_cast<UINT>(appearance::SystemFontDpi()));
}

HFONT DefaultUIFont(
    UINT dpi
) {
  LOGFONTW lf = DefaultUIFontLogFont(dpi);
  return CreateFontIndirectW(&lf);
}

HFONT DefaultUIFont() {
  return DefaultUIFont(static_cast<UINT>(appearance::SystemFontDpi()));
}

} // namespace regkit::ui
