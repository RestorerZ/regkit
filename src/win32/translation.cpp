// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "win32/translation.h"

#include "win32/file_text.h"
#include "win32/handle_owner.h"
#include "win32/shell_paths.h"
#include "win32/text_transform.h"

#include <cwchar>
#include <cwctype>
#include <string_view>
#include <unordered_map>

namespace regkit::util
{
namespace
{

struct TextHash
{
    using is_transparent = void;
    size_t operator()(std::wstring_view text) const noexcept
    {
        return std::hash<std::wstring_view>{}(text);
    }
};

std::unordered_map<std::wstring, std::wstring, TextHash, std::equal_to<>> catalog;

std::wstring LanguageFolder()
{
    return JoinPath(GetModuleDirectory(), L"assets\\lang");
}

std::wstring Unquote(std::wstring_view line)
{
    std::wstring text;
    const size_t first = line.find(L'"');
    const size_t last = line.rfind(L'"');
    if (first == std::wstring_view::npos || last <= first)
    {
        return text;
    }
    constexpr std::wstring_view escapes = L"a\ab\bf\fn\nr\rt\tv\v";
    for (size_t i = first + 1; i < last; ++i)
    {
        wchar_t ch = line[i];
        if (ch == L'\\' && i + 1 < last)
        {
            ch = line[++i];
            const size_t escape = escapes.find(ch);
            if (escape != std::wstring_view::npos && escape % 2 == 0)
            {
                ch = escapes[escape + 1];
            }
        }
        text.push_back(ch);
    }
    return text;
}

std::wstring Directives(std::wstring_view text)
{
    std::wstring out;
    for (size_t i = text.find(L'%'); i != std::wstring_view::npos; i = text.find(L'%', i + 1))
    {
        if (i + 1 < text.size() && text[i + 1] == L'%')
        {
            ++i;
            continue;
        }
        while (++i < text.size() && !iswalpha(text[i]))
        {
        }
        while (i < text.size() && wcschr(L"hlLjztI", text[i]))
        {
            out.push_back(text[i++]);
        }
        if (i < text.size())
        {
            out.push_back(text[i]);
        }
    }
    return out;
}

void ParseCatalog(std::wstring_view content)
{
    std::wstring context;
    std::wstring id;
    std::wstring str;
    std::wstring* target = nullptr;
    bool has_context = false;
    bool fuzzy = false;
    auto flush = [&] {
        if (!id.empty() && !str.empty() && !fuzzy && Directives(id) == Directives(str))
        {
            catalog[has_context ? context + L'\x04' + id : id] = str;
        }
        context.clear();
        id.clear();
        str.clear();
        target = nullptr;
        has_context = false;
        fuzzy = false;
    };
    while (!content.empty())
    {
        const size_t end = content.find(L'\n');
        std::wstring_view line = content.substr(0, end);
        content = end == std::wstring_view::npos ? std::wstring_view{} : content.substr(end + 1);
        if (!line.empty() && line.back() == L'\r')
        {
            line.remove_suffix(1);
        }
        if (target == &str && (line.empty() || line.front() == L'#' || line.starts_with(L"msgctxt ") || line.starts_with(L"msgid ")))
        {
            flush();
        }
        if (line.starts_with(L"#,") && line.find(L"fuzzy") != std::wstring_view::npos)
        {
            fuzzy = true;
        }
        else if (line.starts_with(L"msgctxt "))
        {
            context = Unquote(line);
            target = &context;
            has_context = true;
        }
        else if (line.starts_with(L"msgid "))
        {
            id = Unquote(line);
            target = &id;
        }
        else if (line.starts_with(L"msgstr "))
        {
            str = Unquote(line);
            target = &str;
        }
        else if (line.starts_with(L"msgid_plural") || line.starts_with(L"msgstr["))
        {
            target = nullptr;
        }
        else if (!line.empty() && line.front() == L'"' && target)
        {
            *target += Unquote(line);
        }
    }
    flush();
}

void TranslateText(HWND hwnd)
{
    const std::wstring text = WindowText(hwnd);
    const wchar_t* translated = Tr(text.c_str());
    if (translated != text.c_str())
    {
        SetWindowTextW(hwnd, translated);
    }
}

} // namespace

bool LoadLanguage(const std::wstring& code)
{
    catalog.clear();
    std::wstring languages = code + L'\0';
    ULONG count = 0;
    ULONG size = 0;
    if (code.empty())
    {
        if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size))
        {
            return false;
        }
        languages.resize(size);
        if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, languages.data(), &size))
        {
            return false;
        }
    }
    for (const wchar_t* language = languages.c_str(); *language; language += wcslen(language) + 1)
    {
        // de-AT falls back to de, zh-Hant-TW to zh-Hant and zh
        std::wstring candidate = language;
        while (!candidate.empty())
        {
            if (EqualsInsensitive(candidate, L"en"))
            {
                return false;
            }
            std::wstring content;
            if (ReadTextFile(JoinPath(LanguageFolder(), candidate + L".po"), &content))
            {
                ParseCatalog(content);
                return true;
            }
            const size_t dash = candidate.find_last_of(L'-');
            candidate.resize(dash == std::wstring::npos ? 0 : dash);
        }
    }
    return false;
}

std::vector<LanguagePack> InstalledLanguages()
{
    std::vector<LanguagePack> packs;
    WIN32_FIND_DATAW data = {};
    const UniqueFind find(FindFirstFileW(JoinPath(LanguageFolder(), L"*.po").c_str(), &data));
    if (!find)
    {
        return packs;
    }
    do
    {
        std::wstring code = data.cFileName;
        code.resize(code.size() - 3);
        wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
        packs.push_back({code, GetLocaleInfoEx(code.c_str(), LOCALE_SNATIVEDISPLAYNAME, name, LOCALE_NAME_MAX_LENGTH) ? name : code});
        CharUpperBuffW(packs.back().name.data(), 1);
    } while (FindNextFileW(find.get(), &data));
    return packs;
}

const wchar_t* Tr(const wchar_t* text)
{
    const auto it = catalog.find(std::wstring_view(text));
    return it == catalog.end() ? text : it->second.c_str();
}

std::wstring TrDetail(const wchar_t* headline, std::wstring_view detail)
{
    return std::wstring(Tr(headline)).append(L"\n").append(detail);
}

std::wstring TrLabel(const wchar_t* label, std::wstring_view value)
{
    return std::wstring(Tr(label)).append(L": ").append(value);
}

bool TranslateDialog(HWND dialog)
{
    if (catalog.empty())
    {
        return false;
    }
    TranslateText(dialog);
    EnumChildWindows(
        dialog,
        [](HWND child, LPARAM) -> BOOL {
            wchar_t class_name[16] = {};
            GetClassNameW(child, class_name, static_cast<int>(_countof(class_name)));
            if (EqualsInsensitive(class_name, L"Static") || EqualsInsensitive(class_name, L"Button"))
            {
                TranslateText(child);
            }
            return TRUE;
        },
        0
    );
    return true;
}

} // namespace regkit::util
