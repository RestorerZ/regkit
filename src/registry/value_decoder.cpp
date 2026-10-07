// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "registry/value_decoder.h"
#include "registry/value_format.h"
#include "win32/file_text.h"
#include "win32/handle_owner.h"
#include "win32/text_transform.h"
#include "win32/translation.h"

#include <objbase.h>
#include <sddl.h>
#include <wincrypt.h>
#include <ws2tcpip.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace regkit::value_decoder
{

namespace
{

// file times use 100ns ticks & start before unix epoch
constexpr uint64_t kUnixEpochTicks = 116444736000000000ull;
constexpr uint64_t kMaxFileTime = 0x8000000000000000ull;

std::wstring AtOffset(const wchar_t* message, size_t offset)
{
    return std::wstring(message) + L"\r\n" + util::TrLabel(L"Offset", std::to_wstring(offset));
}

bool HasTextForm(DWORD type, const std::vector<BYTE>& data)
{
    if (type == REG_SZ || type == REG_EXPAND_SZ)
    {
        return (data.size() % sizeof(wchar_t)) == 0;
    }
    if (type != REG_BINARY && type != REG_NONE)
    {
        return false;
    }
    // show text transforms for binary data only when every byte is ASCII
    for (const BYTE byte : data)
    {
        if (byte > 0x7F)
        {
            return false;
        }
    }
    return true;
}

bool SourceText(DWORD type, const std::vector<BYTE>& data, std::wstring* text, std::wstring* error)
{
    text->clear();
    if (type == REG_SZ || type == REG_EXPAND_SZ)
    {
        if ((data.size() % sizeof(wchar_t)) != 0)
        {
            *error = util::Tr(L"Text value has an odd byte count.");
            return false;
        }
        size_t count = data.size() / sizeof(wchar_t);
        std::wstring value(count, L'\0');
        if (count != 0)
        {
            std::memcpy(value.data(), data.data(), count * sizeof(wchar_t));
        }
        if (!value.empty() && value.back() == L'\0')
        {
            value.pop_back();
        }
        if (value.find(L'\0') != std::wstring::npos)
        {
            *error = util::Tr(L"Text value contains an embedded NUL.");
            return false;
        }
        *text = std::move(value);
        return true;
    }
    if (type == REG_BINARY || type == REG_NONE)
    {
        text->reserve(data.size());
        for (const BYTE byte : data)
        {
            if (byte > 0x7F)
            {
                *error = util::Tr(L"Binary value isn't ASCII text.");
                return false;
            }
            text->push_back(static_cast<wchar_t>(byte));
        }
        return true;
    }
    *error = util::Tr(L"This value type has no text form.");
    return false;
}

int Base64Index(wchar_t character)
{
    if (character >= L'A' && character <= L'Z')
    {
        return character - L'A';
    }
    if (character >= L'a' && character <= L'z')
    {
        return character - L'a' + 26;
    }
    if (character >= L'0' && character <= L'9')
    {
        return character - L'0' + 52;
    }
    if (character == L'+')
    {
        return 62;
    }
    if (character == L'/')
    {
        return 63;
    }
    return -1;
}

bool ValidateBase64(const std::wstring& text, size_t padding, std::wstring* error)
{
    const size_t body = text.size() - padding;
    for (size_t i = 0; i < body; ++i)
    {
        if (Base64Index(text[i]) < 0)
        {
            *error = AtOffset(util::Tr(L"Invalid Base64 character."), i);
            return false;
        }
    }
    // unused padding bits must be zero
    if (padding == 2)
    {
        if ((Base64Index(text[body - 1]) & 0x0F) != 0)
        {
            *error = util::Tr(L"Invalid Base64 padding bits.");
            return false;
        }
    }
    else if (padding == 1)
    {
        if ((Base64Index(text[body - 1]) & 0x03) != 0)
        {
            *error = util::Tr(L"Invalid Base64 padding bits.");
            return false;
        }
    }
    return true;
}

bool DecodeWithCrypto(const std::wstring& text, DWORD flags, std::vector<BYTE>* out, std::wstring* error)
{
    out->clear();
    if (text.empty())
    {
        return true;
    }
    if (text.size() > static_cast<size_t>(MAXDWORD))
    {
        *error = util::Tr(L"Input is too large to decode.");
        return false;
    }
    DWORD size = 0;
    if (!CryptStringToBinaryW(text.c_str(), static_cast<DWORD>(text.size()), flags, nullptr, &size, nullptr, nullptr))
    {
        *error = util::Tr(L"The text couldn't be decoded.");
        return false;
    }
    out->resize(size);
    if (size == 0)
    {
        return true;
    }
    if (!CryptStringToBinaryW(text.c_str(), static_cast<DWORD>(text.size()), flags, out->data(), &size, nullptr, nullptr))
    {
        out->clear();
        *error = util::Tr(L"The text couldn't be decoded.");
        return false;
    }
    out->resize(size);
    return true;
}

bool TransformBase64(const std::wstring& text, std::vector<BYTE>* out, std::wstring* error)
{
    if ((text.size() % 4) != 0)
    {
        *error = util::Tr(L"Base64 length isn't a multiple of four.");
        return false;
    }
    size_t padding = 0;
    while (padding < 2 && padding < text.size() && text[text.size() - 1 - padding] == L'=')
    {
        ++padding;
    }
    if (!ValidateBase64(text, padding, error))
    {
        return false;
    }
    return DecodeWithCrypto(text, CRYPT_STRING_BASE64, out, error);
}

bool TransformBase64Url(const std::wstring& text, std::vector<BYTE>* out, std::wstring* error)
{
    std::wstring normalized;
    normalized.reserve(text.size() + 2);
    for (size_t i = 0; i < text.size(); ++i)
    {
        const wchar_t character = text[i];
        if (character == L'-')
        {
            normalized.push_back(L'+');
        }
        else if (character == L'_')
        {
            normalized.push_back(L'/');
        }
        else if (character == L'=')
        {
            normalized.push_back(L'=');
        }
        else if (Base64Index(character) >= 0 && character != L'+' && character != L'/')
        {
            normalized.push_back(character);
        }
        else
        {
            *error = AtOffset(util::Tr(L"Invalid Base64URL character."), i);
            return false;
        }
    }
    size_t padding = 0;
    while (padding < 2 && padding < normalized.size() && normalized[normalized.size() - 1 - padding] == L'=')
    {
        ++padding;
    }
    if (normalized.find(L'=') != std::wstring::npos && normalized.find(L'=') != normalized.size() - padding)
    {
        *error = util::Tr(L"Base64URL padding isn't at the end.");
        return false;
    }
    const size_t remainder = normalized.size() % 4;
    if (remainder == 1)
    {
        *error = util::Tr(L"Base64URL length isn't valid.");
        return false;
    }
    if (remainder != 0)
    {
        if (padding != 0)
        {
            *error = util::Tr(L"Base64URL padding is incomplete.");
            return false;
        }
        padding = 4 - remainder;
        normalized.append(padding, L'=');
    }
    if (!ValidateBase64(normalized, padding, error))
    {
        return false;
    }
    return DecodeWithCrypto(normalized, CRYPT_STRING_BASE64, out, error);
}

bool HexSeparator(wchar_t character)
{
    return character == L' ' || character == L'\t' || character == L'\r' || character == L'\n' || character == L':' ||
           character == L'-';
}

bool TransformHex(const std::wstring& text, std::vector<BYTE>* out, std::wstring* error)
{
    std::wstring normalized;
    normalized.reserve(text.size());
    size_t index = 0;
    while (index < text.size())
    {
        if (HexSeparator(text[index]))
        {
            ++index;
            continue;
        }
        if (text[index] == L'0' && index + 1 < text.size() && (text[index + 1] == L'x' || text[index + 1] == L'X'))
        {
            index += 2;
            continue;
        }
        if (util::HexDigitValue(text[index]) < 0)
        {
            *error = AtOffset(util::Tr(L"Invalid hex character."), index);
            return false;
        }
        if (index + 1 >= text.size() || util::HexDigitValue(text[index + 1]) < 0)
        {
            *error = AtOffset(util::Tr(L"Incomplete hex byte."), index);
            return false;
        }
        normalized.push_back(text[index]);
        normalized.push_back(text[index + 1]);
        index += 2;
    }
    return DecodeWithCrypto(normalized, CRYPT_STRING_HEX, out, error);
}

bool TransformPercent(const std::wstring& text, std::vector<BYTE>* out, std::wstring* error)
{
    out->clear();
    out->reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        const wchar_t character = text[i];
        if (character == L'%')
        {
            if (i + 2 >= text.size())
            {
                *error = AtOffset(util::Tr(L"Incomplete percent escape."), i);
                return false;
            }
            const int high = util::HexDigitValue(text[i + 1]);
            const int low = util::HexDigitValue(text[i + 2]);
            if (high < 0 || low < 0)
            {
                *error = AtOffset(util::Tr(L"Invalid percent escape."), i);
                return false;
            }
            out->push_back(static_cast<BYTE>((high << 4) | low));
            i += 2;
            continue;
        }
        if (character > 0x7F)
        {
            *error = util::Tr(L"Percent encoded text must be ASCII.");
            return false;
        }
        out->push_back(static_cast<BYTE>(character));
    }
    return true;
}

std::wstring FormatSystemTime(const SYSTEMTIME& time)
{
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}

bool AppendTimeFields(uint64_t ticks, std::vector<Field>* fields, std::wstring* error)
{
    if (ticks >= kMaxFileTime)
    {
        *error = util::Tr(L"Invalid FILETIME.");
        return false;
    }
    FILETIME file_time = {};
    file_time.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFull);
    file_time.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    SYSTEMTIME utc = {};
    if (!FileTimeToSystemTime(&file_time, &utc))
    {
        *error = util::Tr(L"Invalid FILETIME.");
        return false;
    }
    fields->push_back({L"UTC", FormatSystemTime(utc)});
    SYSTEMTIME local = {};
    if (SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
    {
        fields->push_back({util::Tr(L"Local"), FormatSystemTime(local)});
    }
    return true;
}

Decoded Failure(std::wstring error)
{
    Decoded decoded;
    decoded.error = std::move(error);
    return decoded;
}

Decoded Success(Field first, Field second = {})
{
    Decoded decoded;
    decoded.ok = true;
    decoded.fields.push_back(std::move(first));
    if (!second.name.empty())
    {
        decoded.fields.push_back(std::move(second));
    }
    return decoded;
}

bool ValidSurrogates(const std::wstring& text)
{
    for (size_t i = 0; i < text.size(); ++i)
    {
        const wchar_t unit = text[i];
        if (unit >= 0xD800 && unit <= 0xDBFF)
        {
            if (i + 1 >= text.size() || text[i + 1] < 0xDC00 || text[i + 1] > 0xDFFF)
            {
                return false;
            }
            ++i;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF)
        {
            return false;
        }
    }
    return true;
}

Decoded DecodeUtf8(const BYTE* data, size_t size)
{
    if (size > static_cast<size_t>(INT_MAX))
    {
        return Failure(util::Tr(L"Input is too large to decode."));
    }
    if (size == 0)
    {
        return Success({util::Tr(L"Text"), L""}, {util::Tr(L"Bytes"), L"0"});
    }
    std::wstring text = util::Utf8ToWide(std::string_view(reinterpret_cast<const char*>(data), size));
    if (text.empty())
    {
        return Failure(util::Tr(L"Invalid UTF-8."));
    }
    return Success({util::Tr(L"Text"), std::move(text)}, {util::Tr(L"Bytes"), std::to_wstring(size)});
}

Decoded DecodeUtf16(const BYTE* data, size_t size, bool big_endian)
{
    if ((size % sizeof(wchar_t)) != 0)
    {
        return Failure(util::Tr(L"UTF-16 needs an even byte count."));
    }
    size_t units = size / sizeof(wchar_t);
    std::wstring text(units, L'\0');
    if (units != 0)
    {
        std::memcpy(text.data(), data, size);
    }
    if (big_endian)
    {
        for (wchar_t& unit : text)
        {
            unit = static_cast<wchar_t>((unit >> 8) | (unit << 8));
        }
    }
    // remove the BOM after byte order has been corrected
    if (!text.empty() && text.front() == 0xFEFF)
    {
        text.erase(text.begin());
        --units;
    }
    if (!ValidSurrogates(text))
    {
        return Failure(util::Tr(L"Invalid UTF-16 surrogate pair."));
    }
    return Success({util::Tr(L"Text"), std::move(text)}, {util::Tr(L"Code units"), std::to_wstring(units)});
}

Decoded DecodeAscii(const BYTE* data, size_t size)
{
    std::wstring text;
    text.reserve(size);
    for (size_t i = 0; i < size; ++i)
    {
        if (data[i] > 0x7F)
        {
            return Failure(AtOffset(util::Tr(L"A byte isn't ASCII."), i));
        }
        text.push_back(static_cast<wchar_t>(data[i]));
    }
    return Success({util::Tr(L"Text"), std::move(text)}, {util::Tr(L"Bytes"), std::to_wstring(size)});
}

Decoded DecodeFileTime(const BYTE* data, size_t size)
{
    if (size != 8)
    {
        return Failure(util::Tr(L"A FILETIME needs exactly 8 bytes."));
    }
    const uint64_t ticks = value_format::ReadUnsigned({data, size}, size);
    Decoded decoded;
    wchar_t raw[32] = {};
    swprintf_s(raw, L"%llu", static_cast<unsigned long long>(ticks));
    decoded.fields.push_back({util::Tr(L"Raw ticks"), raw});
    std::wstring error;
    if (!AppendTimeFields(ticks, &decoded.fields, &error))
    {
        return Failure(std::move(error));
    }
    decoded.ok = true;
    return decoded;
}

Decoded DecodeSystemTime(const BYTE* data, size_t size)
{
    if (size != sizeof(SYSTEMTIME))
    {
        return Failure(util::Tr(L"A SYSTEMTIME needs exactly 16 bytes."));
    }
    SYSTEMTIME time = {};
    std::memcpy(&time, data, sizeof(time));
    FILETIME probe = {};
    // let windows reject invalid dates
    if (time.wMonth < 1 || time.wMonth > 12 || time.wDay < 1 || time.wDay > 31 || time.wHour > 23 ||
        time.wMinute > 59 || time.wSecond > 59 || time.wMilliseconds > 999 || time.wDayOfWeek > 6 ||
        !SystemTimeToFileTime(&time, &probe))
    {
        return Failure(util::Tr(L"Invalid SYSTEMTIME."));
    }
    Decoded decoded;
    decoded.ok = true;
    decoded.fields.push_back({util::Tr(L"Date and time"), FormatSystemTime(time)});
    decoded.fields.push_back({util::Tr(L"Year"), std::to_wstring(time.wYear)});
    decoded.fields.push_back({util::Tr(L"Month"), std::to_wstring(time.wMonth)});
    decoded.fields.push_back({util::Tr(L"Day"), std::to_wstring(time.wDay)});
    decoded.fields.push_back({util::Tr(L"Day of week"), std::to_wstring(time.wDayOfWeek)});
    decoded.fields.push_back({util::Tr(L"Hour"), std::to_wstring(time.wHour)});
    decoded.fields.push_back({util::Tr(L"Minute"), std::to_wstring(time.wMinute)});
    decoded.fields.push_back({util::Tr(L"Second"), std::to_wstring(time.wSecond)});
    decoded.fields.push_back({util::Tr(L"Milliseconds"), std::to_wstring(time.wMilliseconds)});
    return decoded;
}

Decoded DecodeUnix(const BYTE* data, size_t size, bool milliseconds)
{
    if (size != 4 && size != 8)
    {
        return Failure(util::Tr(L"Unix time needs exactly 4 or 8 bytes."));
    }
    const uint64_t value = value_format::ReadUnsigned({data, size}, size);
    const uint64_t scale = milliseconds ? 10000ull : 10000000ull;
    // check scaling & epoch addition before converting to FILETIME
    if (value > (0xFFFFFFFFFFFFFFFFull - kUnixEpochTicks) / scale)
    {
        return Failure(util::Tr(L"Unix time is out of range."));
    }
    Decoded decoded;
    wchar_t raw[32] = {};
    swprintf_s(raw, L"%llu", static_cast<unsigned long long>(value));
    decoded.fields.push_back({milliseconds ? util::Tr(L"Milliseconds") : util::Tr(L"Seconds"), raw});
    std::wstring error;
    if (!AppendTimeFields(kUnixEpochTicks + value * scale, &decoded.fields, &error))
    {
        return Failure(util::Tr(L"Unix time is out of range."));
    }
    decoded.ok = true;
    return decoded;
}

Decoded DecodeGuid(const BYTE* data, size_t size)
{
    if (size != sizeof(GUID))
    {
        return Failure(util::Tr(L"A GUID needs exactly 16 bytes."));
    }
    GUID guid = {};
    std::memcpy(&guid, data, sizeof(guid));
    wchar_t text[64] = {};
    if (StringFromGUID2(guid, text, static_cast<int>(std::size(text))) == 0)
    {
        return Failure(util::Tr(L"The GUID couldn't be formatted."));
    }
    return Success({L"GUID", text});
}

bool SidFits(const BYTE* data, size_t size)
{
    if (size < 8)
    {
        return false;
    }
    const size_t count = data[1];
    return size >= 8 + count * 4;
}

Decoded DecodeSid(const BYTE* data, size_t size)
{
    if (!SidFits(data, size))
    {
        return Failure(util::Tr(L"Truncated structure."));
    }
    std::vector<BYTE> copy(data, data + size);
    PSID sid = reinterpret_cast<PSID>(copy.data());
    if (!IsValidSid(sid))
    {
        return Failure(util::Tr(L"Invalid SID."));
    }
    util::UniqueLocal<LPWSTR> text;
    if (!ConvertSidToStringSidW(sid, text.put()) || !text)
    {
        return Failure(util::Tr(L"Invalid SID."));
    }
    Decoded decoded;
    decoded.ok = true;
    decoded.fields.push_back({L"SID", text.get()});
    decoded.fields.push_back({util::Tr(L"Length"), std::to_wstring(GetLengthSid(sid))});

    DWORD name_size = 0;
    DWORD domain_size = 0;
    SID_NAME_USE use = SidTypeUnknown;
    LookupAccountSidW(nullptr, sid, nullptr, &name_size, nullptr, &domain_size, &use);
    if (name_size != 0 && domain_size != 0)
    {
        std::wstring name(name_size, L'\0');
        std::wstring domain(domain_size, L'\0');
        if (LookupAccountSidW(nullptr, sid, name.data(), &name_size, domain.data(), &domain_size, &use))
        {
            name.resize(name_size);
            domain.resize(domain_size);
            decoded.fields.push_back({util::Tr(L"Account"), domain.empty() ? name : domain + L"\\" + name});
        }
    }
    return decoded;
}

bool AclFits(const BYTE* data, size_t size, DWORD offset)
{
    if (offset == 0)
    {
        return true;
    }
    if (offset > size || size - offset < sizeof(ACL))
    {
        return false;
    }
    ACL header = {};
    std::memcpy(&header, data + offset, sizeof(header));
    return header.AclSize <= size - offset;
}

bool SidAtFits(const BYTE* data, size_t size, DWORD offset)
{
    if (offset == 0)
    {
        return true;
    }
    return offset <= size && SidFits(data + offset, size - offset);
}

Decoded DecodeSecurityDescriptor(const BYTE* data, size_t size)
{
    if (size < sizeof(SECURITY_DESCRIPTOR_RELATIVE))
    {
        return Failure(util::Tr(L"Truncated structure."));
    }
    SECURITY_DESCRIPTOR_RELATIVE header = {};
    std::memcpy(&header, data, sizeof(header));
    // registry security descriptors must use offsets within the same buffer
    if ((header.Control & SE_SELF_RELATIVE) == 0)
    {
        return Failure(util::Tr(L"Not a self relative security descriptor."));
    }
    if (!SidAtFits(data, size, header.Owner) || !SidAtFits(data, size, header.Group) ||
        !AclFits(data, size, header.Dacl) || !AclFits(data, size, header.Sacl))
    {
        return Failure(util::Tr(L"Truncated structure."));
    }
    std::vector<BYTE> copy(data, data + size);
    PSECURITY_DESCRIPTOR descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(copy.data());
    if (!IsValidSecurityDescriptor(descriptor))
    {
        return Failure(util::Tr(L"Invalid security descriptor."));
    }
    const bool dacl = (header.Control & SE_DACL_PRESENT) != 0;
    const bool sacl = (header.Control & SE_SACL_PRESENT) != 0;
    const SECURITY_INFORMATION information = (header.Owner ? OWNER_SECURITY_INFORMATION : 0) | (header.Group ? GROUP_SECURITY_INFORMATION : 0) |
                                             (dacl ? DACL_SECURITY_INFORMATION : 0) | (sacl ? SACL_SECURITY_INFORMATION : 0);
    util::UniqueLocal<LPWSTR> sddl;
    if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, information, sddl.put(), nullptr) ||
        !sddl)
    {
        return Failure(util::Tr(L"The security descriptor couldn't be converted."));
    }
    const auto presence = [](bool present, DWORD offset) {
        return !present ? util::Tr(L"absent") : offset ? util::Tr(L"present")
                                                       : L"NULL";
    };
    Decoded decoded;
    decoded.ok = true;
    decoded.fields.push_back({L"SDDL", sddl.get()});
    decoded.fields.push_back({util::Tr(L"Owner"), presence(header.Owner != 0, header.Owner)});
    decoded.fields.push_back({util::Tr(L"Group"), presence(header.Group != 0, header.Group)});
    decoded.fields.push_back({L"DACL", presence(dacl, header.Dacl)});
    decoded.fields.push_back({L"SACL", presence(sacl, header.Sacl)});
    return decoded;
}

Decoded DecodeAddress(const BYTE* data, size_t size, bool ipv6)
{
    const size_t expected = ipv6 ? sizeof(IN6_ADDR) : sizeof(IN_ADDR);
    if (size != expected)
    {
        return Failure(ipv6 ? util::Tr(L"An IPv6 address needs exactly 16 bytes.") : util::Tr(L"An IPv4 address needs exactly 4 bytes."));
    }
    IN6_ADDR address = {};
    std::memcpy(&address, data, size);
    wchar_t text[INET6_ADDRSTRLEN] = {};
    if (!InetNtopW(ipv6 ? AF_INET6 : AF_INET, &address, text, std::size(text)))
    {
        return Failure(util::Tr(L"The address couldn't be formatted."));
    }
    return Success({util::Tr(L"Address"), text});
}

struct PathRule
{
    const wchar_t* key_text;
    bool key_ends_with;
    const wchar_t* value_name;
    size_t size;
    DecoderId id;
};

constexpr PathRule kPathRules[] = {
    {L"\\Control\\Windows", true, L"ShutdownTime", 8, DecoderId::kFileTime},
    {L"\\Windows NT\\CurrentVersion", true, L"InstallTime", 8, DecoderId::kFileTime},
    {L"\\Windows NT\\CurrentVersion", true, L"InstallDate", 4, DecoderId::kUnixSeconds},
    {L"\\NetworkList\\Profiles\\", false, L"DateCreated", 16, DecoderId::kSystemTime},
    {L"\\NetworkList\\Profiles\\", false, L"DateLastConnected", 16, DecoderId::kSystemTime},
};

} // namespace

std::vector<TransformEntry> AvailableTransforms(DWORD type, const std::vector<BYTE>& data)
{
    std::vector<TransformEntry> entries;
    entries.push_back({TransformId::kNone, util::Tr(L"None")});
    if (HasTextForm(type, data))
    {
        entries.push_back({TransformId::kBase64, L"Base64"});
        entries.push_back({TransformId::kBase64Url, L"Base64URL"});
        entries.push_back({TransformId::kHex, util::Tr(L"Hex bytes")});
        entries.push_back({TransformId::kPercent, util::Tr(L"URI percent encoding")});
    }
    return entries;
}

std::vector<DecoderEntry> AvailableDecoders(const BYTE* data, size_t size)
{
    std::vector<DecoderEntry> entries;
    entries.push_back({DecoderId::kRawBytes, util::Tr(L"Raw bytes")});
    entries.push_back({DecoderId::kUtf8, util::Tr(L"UTF-8 text")});
    if ((size % sizeof(wchar_t)) == 0)
    {
        entries.push_back({DecoderId::kUtf16Le, util::Tr(L"UTF-16 LE text")});
        entries.push_back({DecoderId::kUtf16Be, util::Tr(L"UTF-16 BE text")});
    }
    entries.push_back({DecoderId::kAscii, util::Tr(L"ASCII text")});
    // show fixed size structures only for their exact byte counts
    if (size == 8)
    {
        entries.push_back({DecoderId::kFileTime, util::Tr(L"Windows FILETIME")});
    }
    if (size == sizeof(SYSTEMTIME))
    {
        entries.push_back({DecoderId::kSystemTime, util::Tr(L"Windows SYSTEMTIME")});
    }
    if (size == 4 || size == 8)
    {
        entries.push_back({DecoderId::kUnixSeconds, util::Tr(L"Unix time (seconds)")});
        entries.push_back({DecoderId::kUnixMilliseconds, util::Tr(L"Unix time (milliseconds)")});
    }
    if (size == sizeof(GUID))
    {
        entries.push_back({DecoderId::kGuid, L"GUID"});
    }
    if (SidFits(data, size))
    {
        entries.push_back({DecoderId::kSid, L"SID"});
    }
    if (size >= sizeof(SECURITY_DESCRIPTOR_RELATIVE))
    {
        entries.push_back({DecoderId::kSecurityDescriptor, util::Tr(L"Security descriptor")});
    }
    if (size == sizeof(IN_ADDR))
    {
        entries.push_back({DecoderId::kIpv4, util::Tr(L"IPv4 address")});
    }
    if (size == sizeof(IN6_ADDR))
    {
        entries.push_back({DecoderId::kIpv6, util::Tr(L"IPv6 address")});
    }
    return entries;
}

bool Transform(TransformId id, DWORD type, const std::vector<BYTE>& source, std::vector<BYTE>* out, std::wstring* error)
{
    if (!out || !error)
    {
        return false;
    }
    error->clear();
    if (id == TransformId::kNone)
    {
        return true;
    }
    std::wstring text;
    if (!SourceText(type, source, &text, error))
    {
        return false;
    }
    switch (id)
    {
    case TransformId::kBase64:
        return TransformBase64(text, out, error);
    case TransformId::kBase64Url:
        return TransformBase64Url(text, out, error);
    case TransformId::kHex:
        return TransformHex(text, out, error);
    case TransformId::kPercent:
        return TransformPercent(text, out, error);
    default:
        break;
    }
    *error = util::Tr(L"Unknown encoding.");
    return false;
}

Decoded Decode(DecoderId id, const BYTE* data, size_t size)
{
    if (!data && size != 0)
    {
        return Failure(util::Tr(L"No data."));
    }
    switch (id)
    {
    case DecoderId::kRawBytes:
        return Success({util::Tr(L"Bytes"), std::to_wstring(size)});
    case DecoderId::kUtf8:
        return DecodeUtf8(data, size);
    case DecoderId::kUtf16Le:
        return DecodeUtf16(data, size, false);
    case DecoderId::kUtf16Be:
        return DecodeUtf16(data, size, true);
    case DecoderId::kAscii:
        return DecodeAscii(data, size);
    case DecoderId::kFileTime:
        return DecodeFileTime(data, size);
    case DecoderId::kSystemTime:
        return DecodeSystemTime(data, size);
    case DecoderId::kUnixSeconds:
        return DecodeUnix(data, size, false);
    case DecoderId::kUnixMilliseconds:
        return DecodeUnix(data, size, true);
    case DecoderId::kGuid:
        return DecodeGuid(data, size);
    case DecoderId::kSid:
        return DecodeSid(data, size);
    case DecoderId::kSecurityDescriptor:
        return DecodeSecurityDescriptor(data, size);
    case DecoderId::kIpv4:
        return DecodeAddress(data, size, false);
    case DecoderId::kIpv6:
        return DecodeAddress(data, size, true);
    }
    return Failure(util::Tr(L"Unknown interpretation."));
}

DecoderId Suggest(const std::wstring& key_path, const std::wstring& value_name, size_t size)
{
    for (const PathRule& rule : kPathRules)
    {
        if (size == rule.size && util::EqualsInsensitive(value_name, rule.value_name) &&
            (rule.key_ends_with ? util::EndsWithInsensitive(key_path, rule.key_text)
                                : util::ContainsInsensitive(key_path, rule.key_text)))
        {
            return rule.id;
        }
    }
    return DecoderId::kRawBytes;
}

} // namespace regkit::value_decoder
