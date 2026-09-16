// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "registry/value_format.h"

#include "win32/text_transform.h"

#include <shlwapi.h>

#include <cstring>
#include <cwctype>

namespace regkit::value_format {

namespace {

struct TypeLabel {
  DWORD type;
  const wchar_t* name;
};

constexpr TypeLabel kTypeLabels[] = {
    {REG_NONE, L"REG_NONE"},
    {REG_SZ, L"REG_SZ"},
    {REG_EXPAND_SZ, L"REG_EXPAND_SZ"},
    {REG_MULTI_SZ, L"REG_MULTI_SZ"},
    {REG_DWORD, L"REG_DWORD"},
    {REG_QWORD, L"REG_QWORD"},
    {REG_BINARY, L"REG_BINARY"},
    {REG_RESOURCE_LIST, L"REG_RESOURCE_LIST"},
    {REG_FULL_RESOURCE_DESCRIPTOR, L"REG_FULL_RESOURCE_DESCRIPTOR"},
    {REG_RESOURCE_REQUIREMENTS_LIST, L"REG_RESOURCE_REQUIREMENTS_LIST"},
    {REG_LINK, L"REG_LINK"},
    {REG_DWORD_BIG_ENDIAN, L"REG_DWORD_BIG_ENDIAN"},
};

const TypeLabel* FindTypeLabel(
    DWORD type
) {
  for (const TypeLabel& label : kTypeLabels) {
    if (label.type == (type & 0xFFFF)) {
      return &label;
    }
  }
  return nullptr;
}

} // namespace

DWORD NormalizeType(
    DWORD type
) {
  const TypeLabel* label = FindTypeLabel(type);
  return label ? label->type : type;
}

std::wstring TypeName(
    DWORD type
) {
  const TypeLabel* label = FindTypeLabel(type);
  if (label && label->type == type) {
    return label->name;
  }
  wchar_t buffer[64] = {};
  swprintf_s(buffer, L"%s (0x%X)", label ? label->name : L"REG_UNKNOWN", type);
  return buffer;
}

std::wstring Data(
    DWORD type,
    const BYTE* data,
    DWORD size
) {
  if (!data || size == 0) {
    return {};
  }
  switch (NormalizeType(type)) {
  case REG_SZ:
  case REG_EXPAND_SZ:
  case REG_LINK:
    {
      std::wstring text(reinterpret_cast<const wchar_t*>(data), size / sizeof(wchar_t));
      while (!text.empty() && text.back() == L'\0') {
        text.pop_back();
      }
      return text;
    }
  case REG_MULTI_SZ:
    {
      std::wstring joined;
      for (const std::wstring& item : MultiStringItems({data, size - size % sizeof(wchar_t)})) {
        if (!joined.empty()) {
          joined += L' ';
        }
        joined += item;
      }
      return joined;
    }
  case REG_DWORD:
  case REG_DWORD_BIG_ENDIAN:
    if (size >= sizeof(DWORD)) {
      DWORD value = 0;
      std::memcpy(&value, data, sizeof(value));
      if (NormalizeType(type) == REG_DWORD_BIG_ENDIAN) {
        value = _byteswap_ulong(value);
      }
      wchar_t buffer[32] = {};
      swprintf_s(buffer, L"0x%08X (%u)", value, value);
      return buffer;
    }
    break;
  case REG_QWORD:
    if (size >= sizeof(unsigned long long)) {
      unsigned long long value = 0;
      std::memcpy(&value, data, sizeof(value));
      wchar_t buffer[48] = {};
      swprintf_s(buffer, L"0x%016llX (%llu)", value, value);
      return buffer;
    }
    break;
  default:
    break;
  }
  return util::ToHex({data, size});
}

bool IsLocalIndirectSource(
    const std::wstring& value
) {
  if (value.size() < 2) {
    return false;
  }
  if (value[1] == L'{') {
    return true;
  }
  std::wstring source = value.substr(1, value.find_last_of(L',') == std::wstring::npos ? std::wstring::npos : value.find_last_of(L',') - 1);
  source = util::ExpandEnvironmentStringsDynamic(source);
  if (source.size() < 3 || source.find(L"://") != std::wstring::npos || !iswalpha(source[0]) || source[1] != L':' ||
      (source[2] != L'\\' && source[2] != L'/')) {
    return false;
  }
  const std::wstring root = source.substr(0, 3);
  const UINT drive_type = GetDriveTypeW(root.c_str());
  return drive_type == DRIVE_FIXED || drive_type == DRIVE_RAMDISK;
}

std::wstring DisplayData(
    DWORD type,
    const BYTE* data,
    DWORD size,
    bool resolve_indirect
) {
  const DWORD base_type = NormalizeType(type);
  std::wstring value = Data(type, data, size);
  if (value.empty()) {
    return value;
  }
  if (resolve_indirect && (base_type == REG_SZ || base_type == REG_EXPAND_SZ) &&
      value.front() == L'@' && IsLocalIndirectSource(value)) {
    std::wstring resolved(1024, L'\0');
    HRESULT result =
        SHLoadIndirectString(value.c_str(), resolved.data(), static_cast<UINT>(resolved.size()), nullptr);
    if (result == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
      resolved.assign(4096, L'\0');
      result = SHLoadIndirectString(value.c_str(), resolved.data(), static_cast<UINT>(resolved.size()), nullptr);
    }
    if (SUCCEEDED(result)) {
      while (!resolved.empty() && resolved.back() == L'\0') {
        resolved.pop_back();
      }
      if (!resolved.empty()) {
        return resolved;
      }
    }
  }
  if (base_type == REG_EXPAND_SZ) {
    std::wstring expanded = util::ExpandEnvironmentStringsDynamic(value);
    if (!expanded.empty() && expanded != value) {
      return expanded;
    }
  }
  return value;
}

bool ParseHex(
    std::wstring_view text,
    std::vector<BYTE>* output
) {
  if (!output) {
    return false;
  }
  output->clear();
  int high = -1;
  for (size_t index = 0; index < text.size(); ++index) {
    const wchar_t character = text[index];
    const int value = util::HexDigitValue(character);
    if (value < 0) {
      if (character == L' ' || character == L'\t' || character == L'\r' ||
          character == L'\n' || character == L',' || character == L';' ||
          character == L'-') {
        continue;
      }
      if ((character == L'x' || character == L'X') && high == 0 &&
          index > 0 && text[index - 1] == L'0') {
        high = -1;
        continue;
      }
      return false;
    }
    if (high < 0) {
      high = value;
    } else {
      output->push_back(static_cast<BYTE>((high << 4) | value));
      high = -1;
    }
  }
  return high < 0;
}

std::vector<BYTE> StringData(
    std::wstring_view text
) {
  std::vector<BYTE> data((text.size() + 1) * sizeof(wchar_t));
  std::memcpy(data.data(), text.data(), text.size() * sizeof(wchar_t));
  return data;
}

bool DecodeString(
    std::span<const BYTE> data,
    std::wstring* output
) {
  if (!output || data.size() % sizeof(wchar_t) != 0) {
    return false;
  }
  output->assign(reinterpret_cast<const wchar_t*>(data.data()), data.size() / sizeof(wchar_t));
  while (!output->empty() && output->back() == L'\0') {
    output->pop_back();
  }
  return output->find(L'\0') == std::wstring::npos;
}

std::vector<std::wstring> MultiStringItems(
    std::span<const BYTE> data
) {
  std::vector<std::wstring> items;
  if (data.size() % sizeof(wchar_t) != 0) {
    return items;
  }
  const wchar_t* current = reinterpret_cast<const wchar_t*>(data.data());
  size_t remaining = data.size() / sizeof(wchar_t);
  while (remaining > 0 && *current) {
    const size_t length = wcsnlen_s(current, remaining);
    if (length == remaining) {
      break;
    }
    items.emplace_back(current, length);
    current += length + 1;
    remaining -= length + 1;
  }
  return items;
}

std::vector<BYTE> MultiStringData(
    const std::vector<std::wstring>& items
) {
  size_t characters = 1;
  for (const auto& item : items) {
    characters += item.size() + 1;
  }
  std::vector<BYTE> data(characters * sizeof(wchar_t), 0);
  wchar_t* output = reinterpret_cast<wchar_t*>(data.data());
  for (const auto& item : items) {
    std::memcpy(output, item.data(), item.size() * sizeof(wchar_t));
    output += item.size() + 1;
  }
  return data;
}

std::wstring MultiStringText(
    const std::vector<BYTE>& data
) {
  std::wstring text;
  for (const auto& item : MultiStringItems(data)) {
    text += item;
    text += L"\r\n";
  }
  return text;
}

std::vector<BYTE> MultiStringData(
    std::wstring_view lines
) {
  std::vector<std::wstring> items;
  size_t start = 0;
  while (start <= lines.size()) {
    size_t end = lines.find_first_of(L"\r\n", start);
    if (end == std::wstring_view::npos) {
      end = lines.size();
    }
    if (end > start) {
      items.emplace_back(lines.substr(start, end - start));
    }
    if (end == lines.size()) {
      break;
    }
    start = end + 1;
    if (lines[end] == L'\r' && start < lines.size() &&
        lines[start] == L'\n') {
      ++start;
    }
  }
  return MultiStringData(items);
}

} // namespace regkit::value_format
