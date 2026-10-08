// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/default_font.h"

#include "ui/font_metrics.h"
#include "win32/registry_native.h"

#include <optional>
#include <string>

namespace regkit::ui
{

namespace
{

std::optional<LOGFONTW> g_custom_font;

LOGFONTW SystemFont(UINT dpi)
{
    static const LOGFONTW base = [] {
        LOGFONTW lf = {};
        HFONT stock = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        if (!stock || GetObjectW(stock, sizeof(lf), &lf) == 0)
        {
            lf.lfWeight = FW_NORMAL;
            lf.lfCharSet = DEFAULT_CHARSET;
        }
        // 9 point Segoe UI, windows can substitute the face
        std::wstring face;
        if (util::ReadRegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes", L"Segoe UI", &face) != ERROR_SUCCESS ||
            face.empty())
        {
            face = L"Segoe UI";
        }
        wcsncpy_s(lf.lfFaceName, face.c_str(), _TRUNCATE);
        return lf;
    }();
    LOGFONTW lf = base;
    lf.lfHeight = appearance::FontHeight(9, dpi);
    return lf;
}

} // namespace

void SetCustomFont(const LOGFONTW* font)
{
    g_custom_font = font ? std::optional<LOGFONTW>(*font) : std::nullopt;
}

LOGFONTW SystemUIFontLogFont()
{
    return SystemFont(static_cast<UINT>(appearance::SystemFontDpi()));
}

LOGFONTW DefaultUIFontLogFont(UINT dpi)
{
    if (!g_custom_font)
    {
        return SystemFont(dpi);
    }
    LOGFONTW lf = *g_custom_font;
    lf.lfHeight = appearance::FontHeight(appearance::FontPointSize(lf, 9), dpi);
    return lf;
}

HFONT DefaultUIFont(UINT dpi)
{
    LOGFONTW lf = DefaultUIFontLogFont(dpi);
    return CreateFontIndirectW(&lf);
}

} // namespace regkit::ui
