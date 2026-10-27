// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <win32ui/theme/theme_presets.h>

#include <string>
#include <vector>

namespace regkit
{

using win32ui::FindThemePreset;
using win32ui::ThemePreset;

// preset file in data folder, import/export errors are translated
class ThemePresetStore
{
  public:
    static std::wstring Path();
    // false for a missing file, false with the reason for one that cant be read
    static bool Load(std::vector<ThemePreset>* presets, std::wstring* error = nullptr);
    static bool Save(const std::vector<ThemePreset>& presets, std::wstring* error = nullptr);
    static bool ImportFromFile(const std::wstring& path, std::vector<ThemePreset>* presets, std::wstring* error = nullptr);
    static bool ExportToFile(const std::wstring& path, const std::vector<ThemePreset>& presets, std::wstring* error = nullptr);
};

} // namespace regkit
