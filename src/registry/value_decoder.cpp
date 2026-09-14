// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "registry/value_decoder.h"

#include <objbase.h>
#include <sddl.h>
#include <wincrypt.h>
#include <ws2tcpip.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace regkit::value_decoder {

namespace {

constexpr uint64_t kUnixEpochTicks = 116444736000000000ull;
constexpr uint64_t kMaxFileTime = 0x8000000000000000ull;

bool HasTextForm(
    DWORD type,
    const std::vector<BYTE>& data
) {
  if (type == REG_SZ || type == REG_EXPAND_SZ) {
    return (data.size() % sizeof(wchar_t)) == 0;
  }
  if (type != REG_BINARY && type != REG_NONE) {
    return false;
  }
  for (const BYTE byte : data) {
    if (byte > 0x7F) {
      return false;
    }
  }
  return true;
}

bool SourceText(
    DWORD type,
    const std::vector<BYTE>& data,
    std::wstring* text,
    std::wstring* error
) {
  text->clear();
  if (type == REG_SZ || type == REG_EXPAND_SZ) {
    if ((data.size() % sizeof(wchar_t)) != 0) {
      *error = L"Text value has an odd byte count.";
      return false;
    }
    size_t count = data.size() / sizeof(wchar_t);
    std::wstring value(count, L'\0');
    if (count != 0) {
      std::memcpy(value.data(), data.data(), count * sizeof(wchar_t));
    }
    if (!value.empty() && value.back() == L'\0') {
      value.pop_back();
    }
    if (value.find(L'\0') != std::wstring::npos) {
      *error = L"Text value contains an embedded NUL.";
      return false;
    }
    *text = std::move(value);
    return true;
  }
  if (type == REG_BINARY || type == REG_NONE) {
    text->reserve(data.size());
    for (const BYTE byte : data) {
      if (byte > 0x7F) {
        *error = L"Binary value is not ASCII text.";
        return false;
      }
      text->push_back(static_cast<wchar_t>(byte));
    }
    return true;
  }
  *error = L"This value type has no text form.";
  return false;
}

int Base64Index(
    wchar_t character
) {
  if (character >= L'A' && character <= L'Z') {
    return character - L'A';
  }
  if (character >= L'a' && character <= L'z') {
    return character - L'a' + 26;
  }
  if (character >= L'0' && character <= L'9') {
    return character - L'0' + 52;
  }
  if (character == L'+') {
    return 62;
  }
  if (character == L'/') {
    return 63;
  }
  return -1;
}

bool ValidateBase64(
    const std::wstring& text,
    size_t padding,
    std::wstring* error
) {
  const size_t body = text.size() - padding;
  for (size_t i = 0; i < body; ++i) {
    if (Base64Index(text[i]) < 0) {
      *error = L"Invalid Base64 character at offset " + std::to_wstring(i) + L".";
      return false;
    }
  }
  if (padding == 2) {
    if ((Base64Index(text[body - 1]) & 0x0F) != 0) {
      *error = L"Invalid Base64 padding bits.";
      return false;
    }
  } else if (padding == 1) {
    if ((Base64Index(text[body - 1]) & 0x03) != 0) {
      *error = L"Invalid Base64 padding bits.";
      return false;
    }
  }
  return true;
}

bool DecodeWithCrypto(
    const std::wstring& text,
    DWORD flags,
    std::vector<BYTE>* out,
    std::wstring* error
) {
  out->clear();
  if (text.empty()) {
    return true;
  }
  if (text.size() > static_cast<size_t>(MAXDWORD)) {
    *error = L"Input is too large to decode.";
    return false;
  }
  DWORD size = 0;
  if (!CryptStringToBinaryW(text.c_str(), static_cast<DWORD>(text.size()), flags, nullptr, &size, nullptr, nullptr)) {
    *error = L"The text could not be decoded.";
    return false;
  }
  out->resize(size);
  if (size == 0) {
    return true;
  }
  if (!CryptStringToBinaryW(text.c_str(), static_cast<DWORD>(text.size()), flags, out->data(), &size, nullptr, nullptr)) {
    out->clear();
    *error = L"The text could not be decoded.";
    return false;
  }
  out->resize(size);
  return true;
}

bool TransformBase64(
    const std::wstring& text,
    std::vector<BYTE>* out,
    std::wstring* error
) {
  if ((text.size() % 4) != 0) {
    *error = L"Base64 length is not a multiple of four.";
    return false;
  }
  size_t padding = 0;
  while (padding < 2 && padding < text.size() &&
         text[text.size() - 1 - padding] == L'=') {
    ++padding;
  }
  if (!ValidateBase64(text, padding, error)) {
    return false;
  }
  return DecodeWithCrypto(text, CRYPT_STRING_BASE64, out, error);
}

bool TransformBase64Url(
    const std::wstring& text,
    std::vector<BYTE>* out,
    std::wstring* error
) {
  std::wstring normalized;
  normalized.reserve(text.size() + 2);
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t character = text[i];
    if (character == L'-') {
      normalized.push_back(L'+');
    } else if (character == L'_') {
      normalized.push_back(L'/');
    } else if (character == L'=') {
      normalized.push_back(L'=');
    } else if (Base64Index(character) >= 0 && character != L'+' && character != L'/') {
      normalized.push_back(character);
    } else {
      *error = L"Invalid Base64URL character at offset " + std::to_wstring(i) + L".";
      return false;
    }
  }
  size_t padding = 0;
  while (padding < 2 && padding < normalized.size() &&
         normalized[normalized.size() - 1 - padding] == L'=') {
    ++padding;
  }
  if (normalized.find(L'=') != std::wstring::npos &&
      normalized.find(L'=') != normalized.size() - padding) {
    *error = L"Base64URL padding is not at the end.";
    return false;
  }
  const size_t remainder = normalized.size() % 4;
  if (remainder == 1) {
    *error = L"Base64URL length is not valid.";
    return false;
  }
  if (remainder != 0) {
    if (padding != 0) {
      *error = L"Base64URL padding is incomplete.";
      return false;
    }
    padding = 4 - remainder;
    normalized.append(padding, L'=');
  }
  if (!ValidateBase64(normalized, padding, error)) {
    return false;
  }
  return DecodeWithCrypto(normalized, CRYPT_STRING_BASE64, out, error);
}

bool HexDigit(
    wchar_t character
) {
  return (character >= L'0' && character <= L'9') ||
         (character >= L'a' && character <= L'f') ||
         (character >= L'A' && character <= L'F');
}

bool HexSeparator(
    wchar_t character
) {
  return character == L' ' || character == L'\t' || character == L'\r' ||
         character == L'\n' || character == L':' || character == L'-';
}

bool TransformHex(
    const std::wstring& text,
    std::vector<BYTE>* out,
    std::wstring* error
) {
  std::wstring normalized;
  normalized.reserve(text.size());
  size_t index = 0;
  while (index < text.size()) {
    if (HexSeparator(text[index])) {
      ++index;
      continue;
    }
    if (text[index] == L'0' && index + 1 < text.size() &&
        (text[index + 1] == L'x' || text[index + 1] == L'X')) {
      index += 2;
      continue;
    }
    if (!HexDigit(text[index])) {
      *error = L"Invalid hex character at offset " + std::to_wstring(index) + L".";
      return false;
    }
    if (index + 1 >= text.size() || !HexDigit(text[index + 1])) {
      *error = L"Incomplete hex byte at offset " + std::to_wstring(index) + L".";
      return false;
    }
    normalized.push_back(text[index]);
    normalized.push_back(text[index + 1]);
    index += 2;
  }
  return DecodeWithCrypto(normalized, CRYPT_STRING_HEX, out, error);
}

int HexValue(
    wchar_t character
) {
  if (character >= L'0' && character <= L'9') {
    return character - L'0';
  }
  if (character >= L'a' && character <= L'f') {
    return character - L'a' + 10;
  }
  if (character >= L'A' && character <= L'F') {
    return character - L'A' + 10;
  }
  return -1;
}

bool TransformPercent(
    const std::wstring& text,
    std::vector<BYTE>* out,
    std::wstring* error
) {
  out->clear();
  out->reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t character = text[i];
    if (character == L'%') {
      if (i + 2 >= text.size()) {
        *error = L"Incomplete percent escape at offset " + std::to_wstring(i) + L".";
        return false;
      }
      const int high = HexValue(text[i + 1]);
      const int low = HexValue(text[i + 2]);
      if (high < 0 || low < 0) {
        *error = L"Invalid percent escape at offset " + std::to_wstring(i) + L".";
        return false;
      }
      out->push_back(static_cast<BYTE>((high << 4) | low));
      i += 2;
      continue;
    }
    if (character > 0x7F) {
      *error = L"Percent encoded text must be ASCII.";
      return false;
    }
    out->push_back(static_cast<BYTE>(character));
  }
  return true;
}

std::wstring FormatSystemTime(
    const SYSTEMTIME& time
) {
  wchar_t buffer[64] = {};
  swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
  return buffer;
}

bool AppendTimeFields(
    uint64_t ticks,
    std::vector<Field>* fields,
    std::wstring* error
) {
  if (ticks >= kMaxFileTime) {
    *error = L"Invalid FILETIME.";
    return false;
  }
  FILETIME file_time = {};
  file_time.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFull);
  file_time.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  SYSTEMTIME utc = {};
  if (!FileTimeToSystemTime(&file_time, &utc)) {
    *error = L"Invalid FILETIME.";
    return false;
  }
  fields->push_back({L"UTC", FormatSystemTime(utc)});
  SYSTEMTIME local = {};
  if (SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
    fields->push_back({L"Local", FormatSystemTime(local)});
  }
  return true;
}

uint64_t ReadUnsigned(
    const BYTE* data,
    size_t size
) {
  uint64_t value = 0;
  std::memcpy(&value, data, size);
  return value;
}

Decoded Failure(
    std::wstring error
) {
  Decoded decoded;
  decoded.error = std::move(error);
  return decoded;
}

Decoded DecodeWideText(
    std::wstring text,
    size_t units
) {
  Decoded decoded;
  decoded.ok = true;
  decoded.fields.push_back({L"Text", std::move(text)});
  decoded.fields.push_back({L"Code units", std::to_wstring(units)});
  return decoded;
}

bool ValidSurrogates(
    const std::wstring& text
) {
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t unit = text[i];
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      if (i + 1 >= text.size() || text[i + 1] < 0xDC00 || text[i + 1] > 0xDFFF) {
        return false;
      }
      ++i;
      continue;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
      return false;
    }
  }
  return true;
}

Decoded DecodeUtf8(
    const BYTE* data,
    size_t size
) {
  if (size > static_cast<size_t>(INT_MAX)) {
    return Failure(L"Input is too large to decode.");
  }
  if (size == 0) {
    return DecodeWideText(std::wstring(), 0);
  }
  const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(data), static_cast<int>(size), nullptr, 0);
  if (needed <= 0) {
    return Failure(L"Invalid UTF-8.");
  }
  std::wstring text(static_cast<size_t>(needed), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(data), static_cast<int>(size), text.data(), needed) != needed) {
    return Failure(L"Invalid UTF-8.");
  }
  Decoded decoded;
  decoded.ok = true;
  decoded.fields.push_back({L"Text", std::move(text)});
  decoded.fields.push_back({L"Bytes", std::to_wstring(size)});
  return decoded;
}

Decoded DecodeUtf16(
    const BYTE* data,
    size_t size,
    bool big_endian
) {
  if ((size % sizeof(wchar_t)) != 0) {
    return Failure(L"UTF-16 needs an even byte count.");
  }
  size_t units = size / sizeof(wchar_t);
  std::wstring text(units, L'\0');
  if (units != 0) {
    std::memcpy(text.data(), data, size);
  }
  if (big_endian) {
    for (wchar_t& unit : text) {
      unit = static_cast<wchar_t>((unit >> 8) | (unit << 8));
    }
  }
  if (!text.empty() && text.front() == 0xFEFF) {
    text.erase(text.begin());
    --units;
  }
  if (!ValidSurrogates(text)) {
    return Failure(L"Invalid UTF-16 surrogate pair.");
  }
  return DecodeWideText(std::move(text), units);
}

Decoded DecodeAscii(
    const BYTE* data,
    size_t size
) {
  std::wstring text;
  text.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    if (data[i] > 0x7F) {
      return Failure(L"Byte at offset " + std::to_wstring(i) + L" is not ASCII.");
    }
    text.push_back(static_cast<wchar_t>(data[i]));
  }
  Decoded decoded;
  decoded.ok = true;
  decoded.fields.push_back({L"Text", std::move(text)});
  decoded.fields.push_back({L"Bytes", std::to_wstring(size)});
  return decoded;
}

Decoded DecodeFileTime(
    const BYTE* data,
    size_t size
) {
  if (size != 8) {
    return Failure(L"A FILETIME needs exactly 8 bytes.");
  }
  const uint64_t ticks = ReadUnsigned(data, size);
  Decoded decoded;
  wchar_t raw[32] = {};
  swprintf_s(raw, L"%llu", static_cast<unsigned long long>(ticks));
  decoded.fields.push_back({L"Raw ticks", raw});
  std::wstring error;
  if (!AppendTimeFields(ticks, &decoded.fields, &error)) {
    return Failure(std::move(error));
  }
  decoded.ok = true;
  return decoded;
}

Decoded DecodeSystemTime(
    const BYTE* data,
    size_t size
) {
  if (size != sizeof(SYSTEMTIME)) {
    return Failure(L"A SYSTEMTIME needs exactly 16 bytes.");
  }
  SYSTEMTIME time = {};
  std::memcpy(&time, data, sizeof(time));
  FILETIME probe = {};
  if (time.wMonth < 1 || time.wMonth > 12 || time.wDay < 1 || time.wDay > 31 ||
      time.wHour > 23 || time.wMinute > 59 || time.wSecond > 59 ||
      time.wMilliseconds > 999 || time.wDayOfWeek > 6 ||
      !SystemTimeToFileTime(&time, &probe)) {
    return Failure(L"Invalid SYSTEMTIME.");
  }
  Decoded decoded;
  decoded.ok = true;
  decoded.fields.push_back({L"Date and time", FormatSystemTime(time)});
  decoded.fields.push_back({L"Year", std::to_wstring(time.wYear)});
  decoded.fields.push_back({L"Month", std::to_wstring(time.wMonth)});
  decoded.fields.push_back({L"Day", std::to_wstring(time.wDay)});
  decoded.fields.push_back({L"Day of week", std::to_wstring(time.wDayOfWeek)});
  decoded.fields.push_back({L"Hour", std::to_wstring(time.wHour)});
  decoded.fields.push_back({L"Minute", std::to_wstring(time.wMinute)});
  decoded.fields.push_back({L"Second", std::to_wstring(time.wSecond)});
  decoded.fields.push_back({L"Milliseconds", std::to_wstring(time.wMilliseconds)});
  return decoded;
}

Decoded DecodeUnix(
    const BYTE* data,
    size_t size,
    bool milliseconds
) {
  if (size != 4 && size != 8) {
    return Failure(L"Unix time needs exactly 4 or 8 bytes.");
  }
  const uint64_t value = ReadUnsigned(data, size);
  const uint64_t scale = milliseconds ? 10000ull : 10000000ull;
  if (value > (0xFFFFFFFFFFFFFFFFull - kUnixEpochTicks) / scale) {
    return Failure(L"Unix time is out of range.");
  }
  Decoded decoded;
  wchar_t raw[32] = {};
  swprintf_s(raw, L"%llu", static_cast<unsigned long long>(value));
  decoded.fields.push_back({milliseconds ? L"Milliseconds" : L"Seconds", raw});
  std::wstring error;
  if (!AppendTimeFields(kUnixEpochTicks + value * scale, &decoded.fields, &error)) {
    return Failure(L"Unix time is out of range.");
  }
  decoded.ok = true;
  return decoded;
}

Decoded DecodeGuid(
    const BYTE* data,
    size_t size
) {
  if (size != sizeof(GUID)) {
    return Failure(L"A GUID needs exactly 16 bytes.");
  }
  GUID guid = {};
  std::memcpy(&guid, data, sizeof(guid));
  wchar_t text[64] = {};
  if (StringFromGUID2(guid, text, static_cast<int>(std::size(text))) == 0) {
    return Failure(L"The GUID could not be formatted.");
  }
  Decoded decoded;
  decoded.ok = true;
  decoded.fields.push_back({L"GUID", text});
  return decoded;
}

bool SidFits(
    const BYTE* data,
    size_t size
) {
  if (size < 8) {
    return false;
  }
  const size_t count = data[1];
  return size >= 8 + count * 4;
}

Decoded DecodeSid(
    const BYTE* data,
    size_t size
) {
  if (!SidFits(data, size)) {
    return Failure(L"Truncated structure.");
  }
  std::vector<BYTE> copy(data, data + size);
  PSID sid = reinterpret_cast<PSID>(copy.data());
  if (!IsValidSid(sid)) {
    return Failure(L"Invalid SID.");
  }
  LPWSTR text = nullptr;
  if (!ConvertSidToStringSidW(sid, &text) || !text) {
    return Failure(L"Invalid SID.");
  }
  Decoded decoded;
  decoded.ok = true;
  decoded.fields.push_back({L"SID", text});
  LocalFree(text);
  decoded.fields.push_back({L"Length", std::to_wstring(GetLengthSid(sid))});

  DWORD name_size = 0;
  DWORD domain_size = 0;
  SID_NAME_USE use = SidTypeUnknown;
  LookupAccountSidW(nullptr, sid, nullptr, &name_size, nullptr, &domain_size, &use);
  if (name_size != 0 && domain_size != 0) {
    std::wstring name(name_size, L'\0');
    std::wstring domain(domain_size, L'\0');
    if (LookupAccountSidW(nullptr, sid, name.data(), &name_size, domain.data(), &domain_size, &use)) {
      name.resize(name_size);
      domain.resize(domain_size);
      decoded.fields.push_back({L"Account", domain.empty() ? name : domain + L"\\" + name});
    }
  }
  return decoded;
}

bool AclFits(
    const BYTE* data,
    size_t size,
    DWORD offset
) {
  if (offset == 0) {
    return true;
  }
  if (offset > size || size - offset < sizeof(ACL)) {
    return false;
  }
  ACL header = {};
  std::memcpy(&header, data + offset, sizeof(header));
  return header.AclSize <= size - offset;
}

bool SidAtFits(
    const BYTE* data,
    size_t size,
    DWORD offset
) {
  if (offset == 0) {
    return true;
  }
  return offset <= size && SidFits(data + offset, size - offset);
}

Decoded DecodeSecurityDescriptor(
    const BYTE* data,
    size_t size
) {
  if (size < sizeof(SECURITY_DESCRIPTOR_RELATIVE)) {
    return Failure(L"Truncated structure.");
  }
  SECURITY_DESCRIPTOR_RELATIVE header = {};
  std::memcpy(&header, data, sizeof(header));
  if ((header.Control & SE_SELF_RELATIVE) == 0) {
    return Failure(L"Not a self-relative security descriptor.");
  }
  if (!SidAtFits(data, size, header.Owner) || !SidAtFits(data, size, header.Group) ||
      !AclFits(data, size, header.Dacl) || !AclFits(data, size, header.Sacl)) {
    return Failure(L"Truncated structure.");
  }
  std::vector<BYTE> copy(data, data + size);
  PSECURITY_DESCRIPTOR descriptor =
      reinterpret_cast<PSECURITY_DESCRIPTOR>(copy.data());
  if (!IsValidSecurityDescriptor(descriptor)) {
    return Failure(L"Invalid security descriptor.");
  }
  SECURITY_INFORMATION information = 0;
  if (header.Owner != 0) {
    information |= OWNER_SECURITY_INFORMATION;
  }
  if (header.Group != 0) {
    information |= GROUP_SECURITY_INFORMATION;
  }
  if (header.Dacl != 0) {
    information |= DACL_SECURITY_INFORMATION;
  }
  if (header.Sacl != 0) {
    information |= SACL_SECURITY_INFORMATION;
  }
  LPWSTR sddl = nullptr;
  if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, information, &sddl, nullptr) || !sddl) {
    return Failure(L"The security descriptor could not be converted.");
  }
  Decoded decoded;
  decoded.ok = true;
  decoded.fields.push_back({L"SDDL", sddl});
  LocalFree(sddl);
  decoded.fields.push_back({L"Owner", header.Owner != 0 ? L"present" : L"absent"});
  decoded.fields.push_back({L"Group", header.Group != 0 ? L"present" : L"absent"});
  decoded.fields.push_back({L"DACL", header.Dacl != 0 ? L"present" : L"absent"});
  decoded.fields.push_back({L"SACL", header.Sacl != 0 ? L"present" : L"absent"});
  return decoded;
}

Decoded DecodeAddress(
    const BYTE* data,
    size_t size,
    bool ipv6
) {
  const size_t expected = ipv6 ? sizeof(IN6_ADDR) : sizeof(IN_ADDR);
  if (size != expected) {
    return Failure(ipv6 ? L"An IPv6 address needs exactly 16 bytes."
                        : L"An IPv4 address needs exactly 4 bytes.");
  }
  wchar_t text[INET6_ADDRSTRLEN] = {};
  if (ipv6) {
    IN6_ADDR address = {};
    std::memcpy(&address, data, sizeof(address));
    if (!InetNtopW(AF_INET6, &address, text, std::size(text))) {
      return Failure(L"The address could not be formatted.");
    }
  } else {
    IN_ADDR address = {};
    std::memcpy(&address, data, sizeof(address));
    if (!InetNtopW(AF_INET, &address, text, std::size(text))) {
      return Failure(L"The address could not be formatted.");
    }
  }
  Decoded decoded;
  decoded.ok = true;
  decoded.fields.push_back({L"Address", text});
  return decoded;
}

bool EndsWithInsensitive(
    const std::wstring& text,
    const wchar_t* suffix
) {
  const size_t length = wcslen(suffix);
  return text.size() >= length &&
         _wcsicmp(text.c_str() + (text.size() - length), suffix) == 0;
}

struct PathRule {
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

std::vector<TransformEntry> AvailableTransforms(
    DWORD type,
    const std::vector<BYTE>& data
) {
  std::vector<TransformEntry> entries;
  entries.push_back({TransformId::kNone, L"None"});
  if (HasTextForm(type, data)) {
    entries.push_back({TransformId::kBase64, L"Base64"});
    entries.push_back({TransformId::kBase64Url, L"Base64URL"});
    entries.push_back({TransformId::kHex, L"Hex bytes"});
    entries.push_back({TransformId::kPercent, L"URI percent encoding"});
  }
  return entries;
}

std::vector<DecoderEntry> AvailableDecoders(
    const BYTE* data,
    size_t size
) {
  std::vector<DecoderEntry> entries;
  entries.push_back({DecoderId::kRawBytes, L"Raw bytes"});
  entries.push_back({DecoderId::kUtf8, L"UTF-8 text"});
  if ((size % sizeof(wchar_t)) == 0) {
    entries.push_back({DecoderId::kUtf16Le, L"UTF-16 LE text"});
    entries.push_back({DecoderId::kUtf16Be, L"UTF-16 BE text"});
  }
  entries.push_back({DecoderId::kAscii, L"ASCII text"});
  if (size == 8) {
    entries.push_back({DecoderId::kFileTime, L"Windows FILETIME"});
  }
  if (size == sizeof(SYSTEMTIME)) {
    entries.push_back({DecoderId::kSystemTime, L"Windows SYSTEMTIME"});
  }
  if (size == 4 || size == 8) {
    entries.push_back({DecoderId::kUnixSeconds, L"Unix time (seconds)"});
    entries.push_back({DecoderId::kUnixMilliseconds, L"Unix time (milliseconds)"});
  }
  if (size == sizeof(GUID)) {
    entries.push_back({DecoderId::kGuid, L"GUID"});
  }
  if (SidFits(data, size)) {
    entries.push_back({DecoderId::kSid, L"SID"});
  }
  if (size >= sizeof(SECURITY_DESCRIPTOR_RELATIVE)) {
    entries.push_back({DecoderId::kSecurityDescriptor, L"Security descriptor"});
  }
  if (size == sizeof(IN_ADDR)) {
    entries.push_back({DecoderId::kIpv4, L"IPv4 address"});
  }
  if (size == sizeof(IN6_ADDR)) {
    entries.push_back({DecoderId::kIpv6, L"IPv6 address"});
  }
  return entries;
}

bool Transform(
    TransformId id,
    DWORD type,
    const std::vector<BYTE>& source,
    std::vector<BYTE>* out,
    std::wstring* error
) {
  if (!out || !error) {
    return false;
  }
  error->clear();
  if (id == TransformId::kNone) {
    return true;
  }
  std::wstring text;
  if (!SourceText(type, source, &text, error)) {
    return false;
  }
  switch (id) {
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
  *error = L"Unknown encoding.";
  return false;
}

Decoded Decode(
    DecoderId id,
    const BYTE* data,
    size_t size
) {
  if (!data && size != 0) {
    return Failure(L"No data.");
  }
  switch (id) {
  case DecoderId::kRawBytes:
    {
      Decoded decoded;
      decoded.ok = true;
      decoded.fields.push_back({L"Bytes", std::to_wstring(size)});
      return decoded;
    }
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
  return Failure(L"Unknown interpretation.");
}

DecoderId Suggest(
    DWORD type,
    const std::wstring& key_path,
    const std::wstring& value_name,
    size_t size
) {
  (void)type;
  for (const PathRule& rule : kPathRules) {
    if (size != rule.size ||
        _wcsicmp(value_name.c_str(), rule.value_name) != 0) {
      continue;
    }
    const bool matched = rule.key_ends_with
                             ? EndsWithInsensitive(key_path, rule.key_text)
                             : key_path.find(rule.key_text) != std::wstring::npos;
    if (matched) {
      return rule.id;
    }
  }
  return DecoderId::kRawBytes;
}

} // namespace regkit::value_decoder
