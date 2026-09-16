// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "appearance/presets.h"

#include <algorithm>

#include "win32/file_text.h"
#include "win32/shell_paths.h"
#include "win32/text_transform.h"

namespace regkit {

namespace {

constexpr wchar_t kPresetSection[] = L"[preset]";

struct BuiltInPreset {
  const wchar_t* name;
  bool is_dark;
  ThemeColors colors;
};

constexpr BuiltInPreset kBuiltInPresets[] = {
    {L"Default Dark", true, {RGB(20, 20, 20), RGB(20, 20, 20), RGB(34, 34, 34), RGB(14, 14, 14), RGB(34, 34, 34), RGB(66, 66, 66), RGB(200, 200, 200), RGB(170, 170, 170), RGB(90, 162, 255), RGB(20, 20, 20), RGB(200, 200, 200), RGB(44, 44, 44), RGB(90, 162, 255)}},
    {L"Default Light", false, {RGB(245, 245, 245), RGB(255, 255, 255), RGB(242, 242, 242), RGB(235, 235, 235), RGB(242, 242, 242), RGB(204, 204, 204), RGB(32, 32, 32), RGB(96, 96, 96), RGB(0, 120, 215), RGB(255, 255, 255), RGB(32, 32, 32), RGB(236, 236, 236), RGB(0, 120, 215)}},
    {L"Ayu Dark", true, {RGB(15, 20, 25), RGB(21, 26, 33), RGB(27, 34, 43), RGB(9, 14, 19), RGB(27, 34, 43), RGB(37, 51, 64), RGB(230, 225, 207), RGB(166, 179, 191), RGB(255, 180, 84), RGB(21, 26, 33), RGB(230, 225, 207), RGB(27, 34, 43), RGB(255, 180, 84)}},
    {L"Ayu Light", false, {RGB(250, 250, 250), RGB(243, 243, 243), RGB(232, 232, 232), RGB(240, 240, 240), RGB(232, 232, 232), RGB(214, 214, 214), RGB(92, 103, 115), RGB(138, 145, 153), RGB(242, 151, 24), RGB(243, 243, 243), RGB(92, 103, 115), RGB(232, 232, 232), RGB(242, 151, 24)}},
    {L"Catppuccin Frappe", true, {RGB(48, 52, 70), RGB(65, 69, 89), RGB(81, 87, 109), RGB(42, 46, 64), RGB(81, 87, 109), RGB(98, 104, 128), RGB(198, 208, 245), RGB(181, 191, 226), RGB(140, 170, 238), RGB(65, 69, 89), RGB(198, 208, 245), RGB(81, 87, 109), RGB(140, 170, 238)}},
    {L"Catppuccin Latte", false, {RGB(239, 241, 245), RGB(230, 233, 239), RGB(220, 224, 232), RGB(229, 231, 235), RGB(220, 224, 232), RGB(204, 208, 218), RGB(76, 79, 105), RGB(108, 111, 133), RGB(30, 102, 245), RGB(230, 233, 239), RGB(76, 79, 105), RGB(220, 224, 232), RGB(30, 102, 245)}},
    {L"Catppuccin Macchiato", true, {RGB(36, 39, 58), RGB(48, 52, 70), RGB(54, 58, 79), RGB(30, 33, 52), RGB(54, 58, 79), RGB(73, 77, 100), RGB(202, 211, 245), RGB(165, 173, 203), RGB(138, 173, 244), RGB(48, 52, 70), RGB(202, 211, 245), RGB(54, 58, 79), RGB(138, 173, 244)}},
    {L"Catppuccin Mocha", true, {RGB(30, 30, 46), RGB(42, 43, 60), RGB(49, 50, 68), RGB(24, 24, 40), RGB(49, 50, 68), RGB(69, 71, 90), RGB(205, 214, 244), RGB(166, 173, 200), RGB(137, 180, 250), RGB(42, 43, 60), RGB(205, 214, 244), RGB(49, 50, 68), RGB(137, 180, 250)}},
    {L"Dracula", true, {RGB(40, 42, 54), RGB(52, 55, 70), RGB(59, 63, 82), RGB(34, 36, 48), RGB(59, 63, 82), RGB(68, 71, 90), RGB(248, 248, 242), RGB(191, 191, 191), RGB(189, 147, 249), RGB(52, 55, 70), RGB(248, 248, 242), RGB(59, 63, 82), RGB(189, 147, 249)}},
    {L"Everforest Dark", true, {RGB(43, 51, 57), RGB(52, 63, 68), RGB(60, 71, 77), RGB(37, 45, 51), RGB(60, 71, 77), RGB(61, 72, 77), RGB(211, 198, 170), RGB(157, 169, 160), RGB(167, 192, 128), RGB(52, 63, 68), RGB(211, 198, 170), RGB(60, 71, 77), RGB(167, 192, 128)}},
    {L"Everforest Light", false, {RGB(243, 234, 211), RGB(232, 223, 198), RGB(223, 212, 181), RGB(233, 224, 201), RGB(223, 212, 181), RGB(211, 198, 170), RGB(92, 106, 114), RGB(127, 140, 141), RGB(141, 161, 1), RGB(232, 223, 198), RGB(92, 106, 114), RGB(223, 212, 181), RGB(141, 161, 1)}},
    {L"Gruvbox Dark", true, {RGB(40, 40, 40), RGB(50, 48, 47), RGB(60, 56, 54), RGB(34, 34, 34), RGB(60, 56, 54), RGB(80, 73, 69), RGB(235, 219, 178), RGB(189, 174, 147), RGB(250, 189, 47), RGB(50, 48, 47), RGB(235, 219, 178), RGB(60, 56, 54), RGB(250, 189, 47)}},
    {L"Gruvbox Light", false, {RGB(251, 241, 199), RGB(242, 229, 188), RGB(235, 219, 178), RGB(241, 231, 189), RGB(235, 219, 178), RGB(213, 196, 161), RGB(60, 56, 54), RGB(124, 111, 100), RGB(215, 153, 33), RGB(242, 229, 188), RGB(60, 56, 54), RGB(235, 219, 178), RGB(215, 153, 33)}},
    {L"Horizon", true, {RGB(28, 30, 38), RGB(35, 37, 48), RGB(45, 47, 58), RGB(22, 24, 32), RGB(45, 47, 58), RGB(46, 48, 62), RGB(224, 224, 224), RGB(157, 160, 162), RGB(233, 86, 120), RGB(35, 37, 48), RGB(224, 224, 224), RGB(45, 47, 58), RGB(233, 86, 120)}},
    {L"Kanagawa Dragon", true, {RGB(24, 22, 22), RGB(31, 31, 31), RGB(38, 38, 38), RGB(18, 16, 16), RGB(38, 38, 38), RGB(45, 42, 46), RGB(197, 201, 197), RGB(166, 166, 156), RGB(127, 180, 202), RGB(31, 31, 31), RGB(197, 201, 197), RGB(38, 38, 38), RGB(127, 180, 202)}},
    {L"Kanagawa Lotus", false, {RGB(242, 236, 188), RGB(231, 221, 176), RGB(223, 212, 164), RGB(232, 226, 178), RGB(223, 212, 164), RGB(200, 192, 160), RGB(77, 74, 65), RGB(116, 108, 93), RGB(196, 109, 137), RGB(231, 221, 176), RGB(77, 74, 65), RGB(223, 212, 164), RGB(196, 109, 137)}},
    {L"Kanagawa Wave", true, {RGB(31, 31, 40), RGB(42, 42, 55), RGB(54, 54, 70), RGB(25, 25, 34), RGB(54, 54, 70), RGB(59, 59, 79), RGB(220, 215, 186), RGB(166, 166, 156), RGB(126, 156, 216), RGB(42, 42, 55), RGB(220, 215, 186), RGB(54, 54, 70), RGB(126, 156, 216)}},
    {L"Material", true, {RGB(38, 50, 56), RGB(47, 59, 67), RGB(54, 69, 79), RGB(32, 44, 50), RGB(54, 69, 79), RGB(55, 71, 79), RGB(207, 216, 220), RGB(176, 190, 197), RGB(128, 203, 196), RGB(47, 59, 67), RGB(207, 216, 220), RGB(54, 69, 79), RGB(128, 203, 196)}},
    {L"Monokai", true, {RGB(39, 40, 34), RGB(45, 46, 39), RGB(58, 59, 51), RGB(33, 34, 28), RGB(58, 59, 51), RGB(62, 61, 50), RGB(248, 248, 242), RGB(197, 197, 190), RGB(166, 226, 46), RGB(45, 46, 39), RGB(248, 248, 242), RGB(58, 59, 51), RGB(166, 226, 46)}},
    {L"Night Owl", true, {RGB(1, 22, 39), RGB(11, 37, 58), RGB(17, 50, 77), RGB(0, 16, 33), RGB(17, 50, 77), RGB(18, 48, 71), RGB(214, 222, 235), RGB(159, 179, 200), RGB(130, 170, 255), RGB(11, 37, 58), RGB(214, 222, 235), RGB(17, 50, 77), RGB(130, 170, 255)}},
    {L"Nord", true, {RGB(46, 52, 64), RGB(59, 66, 82), RGB(67, 76, 94), RGB(40, 46, 58), RGB(67, 76, 94), RGB(76, 86, 106), RGB(229, 233, 240), RGB(167, 177, 194), RGB(136, 192, 208), RGB(59, 66, 82), RGB(229, 233, 240), RGB(67, 76, 94), RGB(136, 192, 208)}},
    {L"One Dark", true, {RGB(40, 44, 52), RGB(47, 52, 63), RGB(59, 64, 74), RGB(34, 38, 46), RGB(59, 64, 74), RGB(62, 68, 81), RGB(171, 178, 191), RGB(139, 147, 165), RGB(97, 175, 239), RGB(47, 52, 63), RGB(171, 178, 191), RGB(59, 64, 74), RGB(97, 175, 239)}},
    {L"One Light", false, {RGB(250, 250, 250), RGB(242, 242, 242), RGB(231, 231, 231), RGB(240, 240, 240), RGB(231, 231, 231), RGB(208, 208, 208), RGB(56, 58, 66), RGB(107, 111, 119), RGB(64, 120, 242), RGB(242, 242, 242), RGB(56, 58, 66), RGB(231, 231, 231), RGB(64, 120, 242)}},
    {L"Rose Pine", true, {RGB(25, 23, 36), RGB(31, 29, 46), RGB(38, 35, 58), RGB(19, 17, 30), RGB(38, 35, 58), RGB(64, 61, 82), RGB(224, 222, 244), RGB(156, 154, 179), RGB(235, 111, 146), RGB(31, 29, 46), RGB(224, 222, 244), RGB(38, 35, 58), RGB(235, 111, 146)}},
    {L"Rose Pine Moon", true, {RGB(35, 33, 54), RGB(42, 39, 63), RGB(49, 48, 74), RGB(29, 27, 48), RGB(49, 48, 74), RGB(68, 65, 90), RGB(224, 222, 244), RGB(179, 176, 214), RGB(234, 154, 151), RGB(42, 39, 63), RGB(224, 222, 244), RGB(49, 48, 74), RGB(234, 154, 151)}},
    {L"Solarized Dark", true, {RGB(0, 43, 54), RGB(7, 54, 66), RGB(10, 60, 71), RGB(0, 37, 48), RGB(10, 60, 71), RGB(15, 59, 70), RGB(147, 161, 161), RGB(131, 148, 150), RGB(181, 137, 0), RGB(7, 54, 66), RGB(147, 161, 161), RGB(10, 60, 71), RGB(181, 137, 0)}},
    {L"Solarized Light", false, {RGB(253, 246, 227), RGB(238, 232, 213), RGB(228, 221, 200), RGB(243, 236, 217), RGB(228, 221, 200), RGB(214, 207, 181), RGB(88, 110, 117), RGB(101, 123, 131), RGB(181, 137, 0), RGB(238, 232, 213), RGB(88, 110, 117), RGB(228, 221, 200), RGB(181, 137, 0)}},
    {L"Tokyo Night", true, {RGB(26, 27, 38), RGB(36, 40, 59), RGB(47, 51, 77), RGB(20, 21, 32), RGB(47, 51, 77), RGB(65, 72, 104), RGB(192, 202, 245), RGB(169, 177, 214), RGB(122, 162, 247), RGB(36, 40, 59), RGB(192, 202, 245), RGB(47, 51, 77), RGB(122, 162, 247)}},
};

struct ColorKey {
  const wchar_t* key;
  COLORREF ThemeColors::* member;
};

constexpr ColorKey kColorKeys[] = {
    {L"background", &ThemeColors::background},
    {L"panel", &ThemeColors::panel},
    {L"surface", &ThemeColors::surface},
    {L"field", &ThemeColors::field},
    {L"header", &ThemeColors::header},
    {L"border", &ThemeColors::border},
    {L"text", &ThemeColors::text},
    {L"muted_text", &ThemeColors::muted_text},
    {L"accent", &ThemeColors::accent},
    {L"selection", &ThemeColors::selection},
    {L"selection_text", &ThemeColors::selection_text},
    {L"hover", &ThemeColors::hover},
    {L"focus", &ThemeColors::focus},
};

std::wstring PresetsPath() {
  const std::wstring folder = util::GetAppDataFolder();
  return folder.empty() ? std::wstring() : util::JoinPath(folder, L"theme_presets.rktheme");
}

bool ApplyField(
    ThemePreset* preset,
    std::wstring_view key,
    const std::wstring& value
) {
  if (util::EqualsInsensitive(key, L"name")) {
    preset->name = value;
    return true;
  }
  if (util::EqualsInsensitive(key, L"dark")) {
    const bool dark = value == L"1" || util::EqualsInsensitive(value, L"true");
    preset->is_dark = dark;
    return dark || value == L"0" || util::EqualsInsensitive(value, L"false");
  }
  const auto field = std::find_if(std::begin(kColorKeys), std::end(kColorKeys), [&](const ColorKey& entry) { return util::EqualsInsensitive(key, entry.key); });
  return field == std::end(kColorKeys) || ParseColorHex(value, &(preset->colors.*field->member));
}

bool ParsePresets(
    const std::wstring& content,
    std::vector<ThemePreset>* presets,
    std::wstring* error
) {
  presets->clear();
  ThemePreset current;
  bool in_preset = false;
  auto finish = [&]() {
    if (in_preset) {
      if (current.name.empty()) {
        return false;
      }
      if (current.colors.field == CLR_INVALID) {
        current.colors.field = current.colors.surface;
      }
      presets->push_back(current);
    }
    current = ThemePreset{};
    current.colors.field = CLR_INVALID;
    return true;
  };
  auto fail = [&](const std::wstring& text) {
    if (error) {
      *error = L"The theme preset file contains an entry RegKit cannot parse:\n" + text;
    }
    presets->clear();
    return false;
  };
  for (const std::wstring& line : util::SplitLines(content)) {
    if (line == kPresetSection) {
      if (!finish()) {
        return fail(kPresetSection);
      }
      in_preset = true;
      continue;
    }
    if (!in_preset) {
      continue;
    }
    const size_t sep = line.find(L'=');
    if (sep == std::wstring::npos || !ApplyField(&current, util::TrimWhitespace(std::wstring_view(line).substr(0, sep)), util::TrimWhitespace(std::wstring_view(line).substr(sep + 1)))) {
      return fail(line);
    }
  }
  return finish() || fail(kPresetSection);
}

bool WritePresetFile(
    const std::wstring& path,
    const std::vector<ThemePreset>& presets,
    std::wstring* error
) {
  std::vector<std::wstring> lines;
  for (const ThemePreset& preset : presets) {
    if (preset.name.empty()) {
      continue;
    }
    lines.push_back(kPresetSection);
    lines.push_back(L"name=" + preset.name);
    lines.push_back(preset.is_dark ? L"dark=1" : L"dark=0");
    for (const ColorKey& field : kColorKeys) {
      lines.push_back(std::wstring(field.key) + L"=" + FormatColorHex(preset.colors.*field.member));
    }
  }
  if (util::WriteTextFile(path, util::JoinLines(lines) + L"\r\n", false)) {
    return true;
  }
  if (error) {
    *error = L"Failed to write the theme preset file.";
  }
  return false;
}

} // namespace

std::vector<ThemePreset> ThemePresetStore::BuiltInPresets() {
  std::vector<ThemePreset> presets;
  presets.reserve(std::size(kBuiltInPresets));
  for (const BuiltInPreset& preset : kBuiltInPresets) {
    presets.push_back({preset.name, preset.colors, preset.is_dark});
  }
  return presets;
}

const ThemePreset* FindThemePreset(
    const std::vector<ThemePreset>& presets,
    std::wstring_view name
) {
  const auto found = std::find_if(presets.begin(), presets.end(), [&](const ThemePreset& preset) { return util::EqualsInsensitive(preset.name, name); });
  return found != presets.end() ? &*found : (presets.empty() ? nullptr : &presets.front());
}

bool ThemePresetStore::Load(
    std::vector<ThemePreset>* presets,
    std::wstring* error
) {
  if (error) {
    error->clear();
  }
  std::wstring content;
  return util::ReadTextFile(PresetsPath(), &content) && ParsePresets(content, presets, error);
}

bool ThemePresetStore::Save(
    const std::vector<ThemePreset>& presets,
    std::wstring* error
) {
  return WritePresetFile(PresetsPath(), presets, error);
}

bool ThemePresetStore::ImportFromFile(
    const std::wstring& path,
    std::vector<ThemePreset>* presets,
    std::wstring* error
) {
  std::wstring content;
  const bool read = util::ReadTextFile(path, &content);
  if (read && ParsePresets(content, presets, error) && !presets->empty()) {
    return true;
  }
  if (error && (!read || presets->empty())) {
    *error = read ? L"No theme presets were found in the file." : L"Failed to open the theme preset file.";
  }
  return false;
}

bool ThemePresetStore::ExportToFile(
    const std::wstring& path,
    const std::vector<ThemePreset>& presets,
    std::wstring* error
) {
  return WritePresetFile(path, presets, error);
}

std::wstring FormatColorHex(
    COLORREF color
) {
  wchar_t buffer[8] = {};
  swprintf_s(buffer, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
  return buffer;
}
bool ParseColorHex(
    const std::wstring& text,
    COLORREF* color
) {
  if (!color) {
    return false;
  }
  std::wstring value = util::TrimWhitespace(text);
  if (value.size() == 7 && value[0] == L'#') {
    unsigned int rgb = 0;
    if (swscanf_s(value.c_str() + 1, L"%06x", &rgb) == 1) {
      *color = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
      return true;
    }
  }
  if (value.rfind(L"0x", 0) == 0 && value.size() >= 8) {
    unsigned int rgb = 0;
    if (swscanf_s(value.c_str() + 2, L"%06x", &rgb) == 1) {
      *color = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
      return true;
    }
  }
  if (value.find(L',') != std::wstring::npos) {
    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;
    if (swscanf_s(value.c_str(), L"%u,%u,%u", &r, &g, &b) == 3) {
      if (r <= 255 && g <= 255 && b <= 255) {
        *color = RGB(r, g, b);
        return true;
      }
    }
  }
  return false;
}

} // namespace regkit
