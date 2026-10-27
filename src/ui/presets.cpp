// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/presets.h"

#include "win32/file_text.h"
#include "win32/shell_paths.h"
#include "win32/text_transform.h"
#include "win32/translation.h"

namespace regkit
{

namespace
{

std::wstring PresetsPath()
{
    const std::wstring folder = util::GetAppDataFolder();
    return folder.empty() ? std::wstring() : util::JoinPath(folder, L"theme_presets.rktheme");
}

// parser error line, CRLF counts as one line break
std::wstring LineText(const std::wstring& content, int number)
{
    size_t start = 0;
    for (int line = 1; line < number; ++line)
    {
        const size_t end = content.find_first_of(L"\r\n", start);
        if (end == std::wstring::npos)
        {
            return std::wstring();
        }
        start = end + (content.compare(end, 2, L"\r\n") == 0 ? 2 : 1);
    }
    const size_t end = content.find_first_of(L"\r\n", start);
    return util::TrimWhitespace(std::wstring_view(content).substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
}

bool ParsePresets(const std::wstring& content, std::vector<ThemePreset>* presets, std::wstring* error)
{
    const win32ui::ThemePresetResult result = win32ui::ParseThemePresets(content, presets);
    if (result.error == win32ui::ThemePresetError::none)
    {
        return true;
    }
    if (error)
    {
        *error = util::TrDetail(L"The theme preset file contains an entry RegKit can't parse.", LineText(content, result.line));
    }
    return false;
}

bool WritePresetFile(const std::wstring& path, const std::vector<ThemePreset>& presets, std::wstring* error)
{
    if (win32ui::WriteThemePresets(path, presets).error == win32ui::ThemePresetError::none)
    {
        return true;
    }
    if (error)
    {
        *error = util::Tr(L"Failed to write the theme preset file.");
    }
    return false;
}

} // namespace

std::wstring ThemePresetStore::Path()
{
    return PresetsPath();
}

bool ThemePresetStore::Load(std::vector<ThemePreset>* presets, std::wstring* error)
{
    if (error)
    {
        error->clear();
    }
    std::wstring content;
    return util::ReadStateFile(PresetsPath(), &content, error) && !content.empty() && ParsePresets(content, presets, error);
}

bool ThemePresetStore::Save(const std::vector<ThemePreset>& presets, std::wstring* error)
{
    return WritePresetFile(PresetsPath(), presets, error);
}

bool ThemePresetStore::ImportFromFile(const std::wstring& path, std::vector<ThemePreset>* presets, std::wstring* error)
{
    std::wstring content;
    const bool read = util::ReadTextFile(path, &content, nullptr, util::kMaxStateFileBytes);
    if (read && ParsePresets(content, presets, error) && !presets->empty())
    {
        return true;
    }
    if (error && (!read || presets->empty()))
    {
        *error = read ? util::Tr(L"No theme presets were found in the file.") : util::Tr(L"Failed to open the theme preset file.");
    }
    return false;
}

bool ThemePresetStore::ExportToFile(const std::wstring& path, const std::vector<ThemePreset>& presets, std::wstring* error)
{
    return WritePresetFile(path, presets, error);
}

} // namespace regkit
