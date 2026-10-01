// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "regfile/reg_file.h"
#include "win32/text_transform.h"

#include "registry/key_algorithms.h"
#include "registry/registry_path.h"
#include "registry/value_format.h"
#include "win32/file_text.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cwctype>

namespace regkit::regfile
{
namespace
{

bool ParseQuoted(std::wstring_view text, std::wstring* output, size_t* closing = nullptr)
{
    if (!output || text.empty() || text.front() != L'"')
    {
        return false;
    }
    output->clear();
    bool escaped = false;
    for (size_t index = 1; index < text.size(); ++index)
    {
        const wchar_t character = text[index];
        if (escaped)
        {
            switch (character)
            {
            case L'n':
                output->push_back(L'\n');
                break;
            case L'r':
                output->push_back(L'\r');
                break;
            case L't':
                output->push_back(L'\t');
                break;
            case L'0':
                output->push_back(L'\0');
                break;
            default:
                output->push_back(character);
                break;
            }
            escaped = false;
        }
        else if (character == L'\\')
        {
            escaped = true;
        }
        else if (character == L'"')
        {
            if (closing)
            {
                *closing = index;
                return true;
            }
            for (size_t rest = index + 1; rest < text.size(); ++rest)
            {
                if (text[rest] != L' ' && text[rest] != L'\t')
                {
                    return false;
                }
            }
            return true;
        }
        else
        {
            output->push_back(character);
        }
    }
    return false;
}

size_t FindAssignment(std::wstring_view text)
{
    if (text.empty() || text.front() != L'"')
    {
        return text.find(L'=');
    }
    bool escaped = false;
    for (size_t index = 1; index < text.size(); ++index)
    {
        const wchar_t character = text[index];
        if (escaped)
        {
            escaped = false;
        }
        else if (character == L'\\')
        {
            escaped = true;
        }
        else if (character == L'"')
        {
            return text.find(L'=', index + 1);
        }
    }
    return std::wstring_view::npos;
}

std::wstring Escape(std::wstring_view text)
{
    std::wstring output;
    output.reserve(text.size());
    for (wchar_t character : text)
    {
        if (character == L'\\' || character == L'"')
        {
            output.push_back(L'\\');
        }
        output.push_back(character);
    }
    return output;
}

void AppendHexLine(std::wstring* output, std::wstring_view prefix, const std::vector<BYTE>& data)
{
    constexpr size_t kHexLineLimit = 77;
    constexpr wchar_t kDigits[] = L"0123456789abcdef";
    output->append(prefix);
    size_t line = prefix.size();
    for (size_t index = 0; index < data.size(); ++index)
    {
        output->push_back(kDigits[data[index] >> 4]);
        output->push_back(kDigits[data[index] & 0x0F]);
        if (index + 1 == data.size())
        {
            break;
        }
        output->push_back(L',');
        line += 3;
        if (line >= kHexLineLimit)
        {
            output->append(L"\\\r\n  ");
            line = 2;
        }
    }
    output->append(L"\r\n");
}

std::wstring ValueNamePrefix(const std::wstring& name)
{
    return name.empty() ? L"@=" : L"\"" + Escape(name) + L"\"=";
}

void AppendValue(std::wstring* output, const Value& value)
{
    const std::wstring name = ValueNamePrefix(value.name);
    std::wstring text;
    if (value.type == REG_SZ && value_format::DecodeString(value.data, &text) && text.find_first_of(L"\r\n") == std::wstring::npos)
    {
        output->append(name).append(L"\"").append(Escape(text)).append(L"\"\r\n");
        return;
    }
    if (value.type == REG_DWORD && value.data.size() == sizeof(DWORD))
    {
        DWORD number = 0;
        std::memcpy(&number, value.data.data(), sizeof(number));
        wchar_t text[24] = {};
        swprintf_s(text, L"dword:%08x\r\n", number);
        output->append(name).append(text);
        return;
    }
    wchar_t code[16] = {};
    swprintf_s(code, L"hex(%x):", value.type);
    AppendHexLine(output, name + (value.type == REG_BINARY ? L"hex:" : code), value.data);
}

constexpr std::wstring_view kRegFileHeader = L"Windows Registry Editor Version 5.00\r\n\r\n";

} // namespace

Writer::Writer()
    : output_(kRegFileHeader)
{
}

void Writer::AppendKeyHeader(std::wstring_view prefix, std::wstring_view path)
{
    if (output_.size() > kRegFileHeader.size())
    {
        output_ += L"\r\n";
    }
    output_.append(prefix).append(path).append(L"]\r\n");
}

void Writer::AppendRemovedKey(std::wstring_view path)
{
    AppendKeyHeader(L"[-", path);
}

void Writer::AppendRemovedValues(const std::vector<std::wstring>& names)
{
    for (const std::wstring& name : names)
    {
        output_.append(ValueNamePrefix(name)).append(L"-\r\n");
    }
}

void Writer::AppendKey(std::wstring_view path, std::vector<const Value*> values, bool sorted)
{
    AppendKeyHeader(L"[", path);
    if (sorted)
    {
        std::sort(values.begin(), values.end(), [](const Value* left, const Value* right) {
            return left->name.empty() != right->name.empty() ? left->name.empty()
                                                             : util::CompareInsensitive(left->name, right->name) < 0;
        });
    }
    for (const Value* value : values)
    {
        AppendValue(&output_, *value);
    }
}

std::wstring Writer::Finish() &&
{
    if (output_.size() > kRegFileHeader.size())
    {
        output_ += L"\r\n";
    }
    return std::move(output_);
}

LONG AppendRegistryTree(Writer* writer, HKEY root, const std::wstring& subkey, const std::wstring& display_path, REGSAM view, bool recurse)
{
    struct Pending
    {
        std::wstring subkey;
        std::wstring display;
    };
    std::vector<Pending> pending{{subkey, display_path}};
    LONG top_status = ERROR_SUCCESS;
    for (bool top = true; !pending.empty(); top = false)
    {
        const Pending current = std::move(pending.back());
        pending.pop_back();
        registry_backend::KeyContents contents;
        const LONG status = registry_backend::ReadKeyContents(root, current.subkey, view, true, &contents);
        if (top)
        {
            top_status = status;
        }
        if (status != ERROR_SUCCESS)
        {
            continue;
        }
        std::vector<const Value*> values;
        values.reserve(contents.values.size());
        for (const Value& value : contents.values)
        {
            values.push_back(&value);
        }
        writer->AppendKey(current.display, std::move(values), false);
        for (auto child = contents.subkeys.rbegin(); recurse && child != contents.subkeys.rend(); ++child)
        {
            pending.push_back({registry_path::JoinSubkey(current.subkey, *child), current.display + L"\\" + *child});
        }
    }
    return top_status;
}

bool Parse(std::wstring_view content, Document* output, const std::atomic_bool* cancel, bool* cancelled, std::wstring* error)
{
    if (!output)
    {
        return false;
    }
    output->keys.clear();
    output->key_order.clear();
    if (cancelled)
    {
        *cancelled = false;
    }
    auto stopped = [&] {
        const bool value = cancel && cancel->load();
        if (value && cancelled)
        {
            *cancelled = true;
        }
        return value;
    };

    std::vector<std::wstring> lines;
    std::wstring continued;
    bool continuing = false;
    size_t start = 0;
    while (start < content.size())
    {
        if (stopped())
        {
            return false;
        }
        size_t end = content.find(L'\n', start);
        if (end == std::wstring_view::npos)
        {
            end = content.size();
        }
        std::wstring line(content.substr(start, end - start));
        if (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }
        start = end + 1;
        if (continuing)
        {
            const size_t first = line.find_first_not_of(L" \t");
            line.erase(0, first == std::wstring::npos ? line.size() : first);
        }
        continued += line;
        while (!continued.empty() && (continued.back() == L' ' || continued.back() == L'\t'))
        {
            continued.pop_back();
        }
        if (!continued.empty() && continued.back() == L'\\')
        {
            continued.pop_back();
            continuing = true;
            continue;
        }
        continuing = false;
        lines.push_back(std::move(continued));
        continued.clear();
    }
    if (!continued.empty())
    {
        lines.push_back(std::move(continued));
    }

    auto fail = [&](const std::wstring& line) {
        if (error)
        {
            std::wstring shown = line.size() > 80 ? line.substr(0, 80) + L"..." : line;
            *error = L"The file contains an entry RegKit can't parse:\n" + shown;
        }
        return false;
    };

    Key* current_key = nullptr;
    for (const auto& raw : lines)
    {
        if (stopped())
        {
            return false;
        }
        const std::wstring line = util::TrimWhitespace(raw);
        if (line.empty() || line.front() == L';' || util::StartsWithInsensitive(line, L"Windows Registry Editor") ||
            util::StartsWithInsensitive(line, L"REGEDIT4"))
        {
            continue;
        }
        if (line.front() == L'[' && line.back() == L']')
        {
            std::wstring path = util::TrimWhitespace(std::wstring_view(line).substr(1, line.size() - 2));
            bool removed = false;
            if (!path.empty() && path.front() == L'-')
            {
                removed = true;
                path = util::TrimWhitespace(std::wstring_view(path).substr(1));
            }
            if (path.empty())
            {
                return fail(line);
            }
            const std::wstring lower = util::ToLower(path);
            auto [iterator, inserted] = output->keys.try_emplace(lower, Key{path, {}});
            if (inserted)
            {
                output->key_order.push_back(path);
            }
            iterator->second.removed = removed;
            current_key = removed ? nullptr : &iterator->second;
            continue;
        }
        if (!current_key)
        {
            return fail(line);
        }
        const size_t equals = FindAssignment(line);
        if (equals == std::wstring::npos)
        {
            return fail(line);
        }
        const std::wstring name_text = util::TrimWhitespace(std::wstring_view(line).substr(0, equals));
        const std::wstring data_text = util::TrimWhitespace(std::wstring_view(line).substr(equals + 1));
        if (name_text.empty() || data_text.empty())
        {
            return fail(line);
        }

        Value value;
        if (name_text == L"@")
        {
            value.name.clear();
        }
        else if (!ParseQuoted(name_text, &value.name))
        {
            return fail(line);
        }

        if (data_text == L"-")
        {
            current_key->values.erase(util::ToLower(value.name));
            current_key->removed_values.push_back(value.name);
            continue;
        }

        if (data_text.front() == L'"')
        {
            std::wstring text;
            if (!ParseQuoted(data_text, &text))
            {
                return fail(line);
            }
            value.type = REG_SZ;
            value.data = value_format::StringData(text);
        }
        else if (util::StartsWithInsensitive(data_text, L"dword:"))
        {
            const std::wstring number_text = util::TrimWhitespace(std::wstring_view(data_text).substr(6));
            if (number_text.empty())
            {
                return fail(line);
            }
            wchar_t* stop = nullptr;
            errno = 0;
            const unsigned long parsed = wcstoul(number_text.c_str(), &stop, 16);
            if (!stop || *stop != L'\0' || errno == ERANGE || parsed > 0xFFFFFFFFul)
            {
                return fail(line);
            }
            const DWORD number = static_cast<DWORD>(parsed);
            value.type = REG_DWORD;
            value.data.resize(sizeof(number));
            std::memcpy(value.data.data(), &number, sizeof(number));
        }
        else if (util::StartsWithInsensitive(data_text, L"hex"))
        {
            const size_t colon = data_text.find(L':');
            if (colon == std::wstring::npos)
            {
                return fail(line);
            }
            value.type = REG_BINARY;
            const size_t open = data_text.find(L'(');
            const size_t close = data_text.find(L')');
            if (open != std::wstring::npos && close != std::wstring::npos && close > open && close < colon)
            {
                const std::wstring code = data_text.substr(open + 1, close - open - 1);
                wchar_t* stop = nullptr;
                errno = 0;
                const unsigned long parsed = wcstoul(code.c_str(), &stop, 16);
                if (code.empty() || !stop || *stop != L'\0' || errno == ERANGE)
                {
                    return fail(line);
                }
                value.type = static_cast<DWORD>(parsed);
            }
            else if (colon != 3)
            {
                return fail(line);
            }
            if (!value_format::ParseHex(std::wstring_view(data_text).substr(colon + 1), &value.data))
            {
                return fail(line);
            }
        }
        else
        {
            return fail(line);
        }
        current_key->values[util::ToLower(value.name)] = std::move(value);
    }
    return true;
}

bool Load(const std::wstring& path, Document* output, std::wstring* error, const std::atomic_bool* cancel, bool* cancelled)
{
    std::wstring content;
    if (!util::ReadTextFile(path, &content, nullptr, 32ull * 1024ull * 1024ull))
    {
        if (error)
        {
            *error = L"Failed to read registry file.";
        }
        return false;
    }
    return Parse(content, output, cancel, cancelled, error);
}

std::wstring Serialize(const Document& document)
{
    Writer writer;
    for (const auto& ordered_path : document.key_order)
    {
        auto key = document.keys.find(util::ToLower(ordered_path));
        if (key == document.keys.end())
        {
            continue;
        }
        if (key->second.removed)
        {
            writer.AppendRemovedKey(key->second.path);
            continue;
        }
        std::vector<const Value*> values;
        values.reserve(key->second.values.size());
        for (const auto& entry : key->second.values)
        {
            values.push_back(&entry.second);
        }
        writer.AppendKey(key->second.path, std::move(values));
        writer.AppendRemovedValues(key->second.removed_values);
    }
    return std::move(writer).Finish();
}

} // namespace regkit::regfile
