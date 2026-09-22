// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "records/escaped_fields.h"

namespace regkit::record_fields
{

namespace
{

constexpr std::wstring_view kSpecial = L"\\\t\r\n";

void AppendEscaped(std::wstring* output, std::wstring_view text)
{
    size_t start = 0;
    for (size_t special = text.find_first_of(kSpecial); special != std::wstring_view::npos;
         special = text.find_first_of(kSpecial, start))
    {
        output->append(text.substr(start, special - start));
        output->push_back(L'\\');
        const wchar_t character = text[special];
        output->push_back(character == L'\t' ? L't' : character == L'\r' ? L'r'
                                                  : character == L'\n'   ? L'n'
                                                                         : L'\\');
        start = special + 1;
    }
    output->append(text.substr(start));
}

void AppendUnescaped(std::wstring* output, std::wstring_view text)
{
    size_t start = 0;
    for (size_t slash = text.find(L'\\'); slash != std::wstring_view::npos && slash + 1 < text.size();
         slash = text.find(L'\\', start))
    {
        const wchar_t next = text[slash + 1];
        const wchar_t decoded = next == L't'    ? L'\t'
                                : next == L'r'  ? L'\r'
                                : next == L'n'  ? L'\n'
                                : next == L'\\' ? L'\\'
                                                : 0;
        if (!decoded)
        {
            output->append(text.substr(start, slash + 1 - start));
            start = slash + 1;
            continue;
        }
        output->append(text.substr(start, slash - start));
        output->push_back(decoded);
        start = slash + 2;
    }
    output->append(text.substr(start));
}

template <typename Fields>
void AppendFields(std::wstring* output, const Fields& fields)
{
    bool first = true;
    for (std::wstring_view field : fields)
    {
        if (!first)
        {
            output->push_back(L'\t');
        }
        AppendEscaped(output, field);
        first = false;
    }
    output->push_back(L'\n');
}

} // namespace

std::wstring Escape(std::wstring_view text)
{
    std::wstring escaped;
    escaped.reserve(text.size());
    AppendEscaped(&escaped, text);
    return escaped;
}

std::wstring Unescape(std::wstring_view text)
{
    std::wstring unescaped;
    unescaped.reserve(text.size());
    AppendUnescaped(&unescaped, text);
    return unescaped;
}

std::vector<std::wstring_view> Lines(std::wstring_view content)
{
    std::vector<std::wstring_view> lines;
    size_t start = 0;
    while (start < content.size())
    {
        const size_t end = content.find_first_of(L"\r\n", start);
        if (end == std::wstring_view::npos)
        {
            lines.push_back(content.substr(start));
            break;
        }
        lines.push_back(content.substr(start, end - start));
        start = end + (content[end] == L'\r' && end + 1 < content.size() && content[end + 1] == L'\n' ? 2 : 1);
    }
    return lines;
}

void AppendRecord(std::wstring* output, std::initializer_list<std::wstring_view> fields)
{
    AppendFields(output, fields);
}

void AppendRecord(std::wstring* output, std::span<const std::wstring> fields)
{
    AppendFields(output, fields);
}

std::vector<std::wstring> DecodeRecord(std::wstring_view line)
{
    std::vector<std::wstring> fields;
    size_t start = 0;
    for (;;)
    {
        const size_t separator = line.find(L'\t', start);
        std::wstring& field = fields.emplace_back();
        AppendUnescaped(&field, line.substr(start, separator == std::wstring_view::npos ? std::wstring_view::npos : separator - start));
        if (separator == std::wstring_view::npos)
        {
            return fields;
        }
        start = separator + 1;
    }
}

bool ParseUnsigned(std::wstring_view text, uint64_t maximum, uint64_t* value)
{
    if (text.empty())
    {
        return false;
    }
    uint64_t result = 0;
    for (const wchar_t character : text)
    {
        if (character < L'0' || character > L'9')
        {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(character - L'0');
        if (digit > maximum || result > (maximum - digit) / 10)
        {
            return false;
        }
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

} // namespace regkit::record_fields
