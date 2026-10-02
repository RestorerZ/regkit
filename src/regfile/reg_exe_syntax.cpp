// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "regfile/reg_exe_syntax.h"

#include "registry/value_format.h"
#include "win32/text_transform.h"

namespace regkit::reg_exe
{
namespace
{

struct ValueType
{
    const wchar_t* name;
    DWORD type;
};

constexpr ValueType kTypes[] = {
    {L"REG_SZ", REG_SZ},
    {L"REG_MULTI_SZ", REG_MULTI_SZ},
    {L"REG_EXPAND_SZ", REG_EXPAND_SZ},
    {L"REG_DWORD", REG_DWORD},
    {L"REG_DWORD_LITTLE_ENDIAN", REG_DWORD_LITTLE_ENDIAN},
    {L"REG_DWORD_BIG_ENDIAN", REG_DWORD_BIG_ENDIAN},
    {L"REG_QWORD", REG_QWORD},
    {L"REG_QWORD_LITTLE_ENDIAN", REG_QWORD_LITTLE_ENDIAN},
    {L"REG_BINARY", REG_BINARY},
    {L"REG_NONE", REG_NONE},
    {L"REG_LINK", REG_LINK},
    {L"REG_FULL_RESOURCE_DESCRIPTOR", REG_FULL_RESOURCE_DESCRIPTOR},
};

bool Fail(std::wstring* error, const std::wstring& message)
{
    if (error)
    {
        *error = message;
    }
    return false;
}

} // namespace

bool IsSwitch(std::wstring_view text, std::wstring_view name)
{
    return !text.empty() && (text[0] == L'/' || text[0] == L'-') && util::EqualsInsensitive(text.substr(1), name);
}

bool ParseOptions(const std::vector<std::wstring>& args, size_t first, Options* options, std::vector<std::wstring>* positional, bool separator_switch, std::wstring* error)
{
    for (size_t i = first; i < args.size(); ++i)
    {
        const std::wstring& arg = args[i];
        auto next = [&](std::wstring* out) -> bool {
            if (i + 1 >= args.size())
            {
                return Fail(error, L"Missing argument for " + arg);
            }
            *out = args[++i];
            return true;
        };
        if (IsSwitch(arg, L"v"))
        {
            if (!next(&options->value_name))
                return false;
            options->has_value = true;
        }
        else if (IsSwitch(arg, L"ve"))
        {
            options->default_value = true;
            options->has_value = true;
        }
        else if (IsSwitch(arg, L"va"))
        {
            options->all_values = true;
        }
        else if (IsSwitch(arg, L"t"))
        {
            if (!next(&options->type_text))
                return false;
        }
        else if (IsSwitch(arg, L"d"))
        {
            if (!next(&options->data))
                return false;
            options->has_data = true;
        }
        else if (IsSwitch(arg, L"s"))
        {
            // /s selects multi string separator for add and recursion elsewhere
            if (separator_switch)
            {
                if (!next(&options->separator))
                    return false;
            }
            else
            {
                options->recurse = true;
            }
        }
        else if (IsSwitch(arg, L"f") || IsSwitch(arg, L"y"))
        {
            options->force = true;
        }
        else if (IsSwitch(arg, L"reg:32"))
        {
            options->view = KEY_WOW64_32KEY;
        }
        else if (IsSwitch(arg, L"reg:64"))
        {
            options->view = KEY_WOW64_64KEY;
        }
        else if (!arg.empty() && (arg[0] == L'/' || arg[0] == L'-'))
        {
            return Fail(error, L"Invalid option: " + arg);
        }
        else
        {
            positional->push_back(arg);
        }
    }
    return true;
}

bool ParseType(std::wstring_view text, DWORD* type)
{
    for (const ValueType& entry : kTypes)
    {
        if (util::EqualsInsensitive(text, entry.name))
        {
            *type = entry.type;
            return true;
        }
    }
    return false;
}

std::wstring TypeName(DWORD type)
{
    for (const ValueType& entry : kTypes)
    {
        if (entry.type == type)
        {
            return entry.name;
        }
    }
    return value_format::TypeName(type);
}

bool BuildData(DWORD type, std::wstring_view text, std::wstring_view separator, std::vector<BYTE>* data, std::wstring* error)
{
    data->clear();
    switch (type)
    {
    case REG_SZ:
    case REG_EXPAND_SZ:
    case REG_LINK:
    case REG_NONE: // REG_NONE uses text
        *data = value_format::StringData(text);
        return true;
    case REG_MULTI_SZ:
        {
            std::vector<std::wstring> items;
            for (size_t start = 0; !text.empty();)
            {
                const size_t end = separator.empty() ? std::wstring_view::npos : text.find(separator, start);
                items.emplace_back(text.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start));
                if (end == std::wstring_view::npos)
                {
                    break;
                }
                start = end + separator.size();
            }
            if (!items.empty() && items.back().empty())
            {
                items.pop_back();
            }
            for (const std::wstring& item : items)
            {
                if (item.empty() && items.size() > 1)
                {
                    return Fail(error, L"Invalid multi-string data: " + std::wstring(text));
                }
            }
            *data = value_format::MultiStringData(items);
            return true;
        }
    case REG_DWORD:
    case REG_DWORD_BIG_ENDIAN:
    case REG_QWORD:
        {
            std::wstring number = util::TrimWhitespace(text);
            if (!number.empty() && number.front() == L'+')
            {
                number.erase(0, 1);
            }
            unsigned long long value = 0;
            if (!util::ParseUnsignedNumber(number, 10, &value))
            {
                return Fail(error, L"Invalid numeric data: " + std::wstring(text));
            }
            if (type != REG_QWORD && value > 0xFFFFFFFFull)
            {
                return Fail(error, L"Numeric data out of range for a DWORD: " + std::wstring(text));
            }
            *data = value_format::UnsignedBytes(value, type == REG_QWORD ? sizeof(ULONGLONG) : sizeof(DWORD));
            return true;
        }
    default:
        {
            std::wstring digits(text);
            for (wchar_t character : digits)
            {
                if (util::HexDigitValue(character) < 0)
                {
                    return Fail(error, L"Invalid binary data: " + std::wstring(text));
                }
            }
            if (digits.size() % 2 != 0)
            {
                digits.insert(digits.begin(), L'0');
            }
            return value_format::ParseHex(digits, data) || Fail(error, L"Invalid binary data: " + std::wstring(text));
        }
    }
}

} // namespace regkit::reg_exe
