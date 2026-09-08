// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "regfile/reg_file.h"
#include "win32/text_transform.h"

#include "registry/registry_path.h"
#include "registry/value_format.h"
#include "win32/file_text.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cwctype>

namespace regkit::regfile {
namespace {

std::wstring Trim(std::wstring_view text) {
  size_t first = 0;
  while (first < text.size() && iswspace(text[first])) {
    ++first;
  }
  size_t last = text.size();
  while (last > first && iswspace(text[last - 1])) {
    --last;
  }
  return std::wstring(text.substr(first, last - first));
}

bool ParseQuoted(std::wstring_view text, std::wstring* output,
                 size_t* closing = nullptr) {
  if (!output || text.empty() || text.front() != L'"') {
    return false;
  }
  output->clear();
  bool escaped = false;
  for (size_t index = 1; index < text.size(); ++index) {
    const wchar_t character = text[index];
    if (escaped) {
      switch (character) {
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
    } else if (character == L'\\') {
      escaped = true;
    } else if (character == L'"') {
      if (closing) {
        *closing = index;
        return true;
      }
      for (size_t rest = index + 1; rest < text.size(); ++rest) {
        if (text[rest] != L' ' && text[rest] != L'	') {
          return false;
        }
      }
      return true;
    } else {
      output->push_back(character);
    }
  }
  return false;
}

size_t FindAssignment(std::wstring_view text) {
  if (text.empty() || text.front() != L'"') {
    return text.find(L'=');
  }
  bool escaped = false;
  for (size_t index = 1; index < text.size(); ++index) {
    const wchar_t character = text[index];
    if (escaped) {
      escaped = false;
    } else if (character == L'\\') {
      escaped = true;
    } else if (character == L'"') {
      return text.find(L'=', index + 1);
    }
  }
  return std::wstring_view::npos;
}

DWORD TypeFromCode(unsigned long code) {
  switch (code) {
  case 0x0:
    return REG_NONE;
  case 0x1:
    return REG_SZ;
  case 0x2:
    return REG_EXPAND_SZ;
  case 0x3:
    return REG_BINARY;
  case 0x4:
    return REG_DWORD;
  case 0x5:
    return REG_DWORD_BIG_ENDIAN;
  case 0x6:
    return REG_LINK;
  case 0x7:
    return REG_MULTI_SZ;
  case 0x8:
    return REG_RESOURCE_LIST;
  case 0x9:
    return REG_FULL_RESOURCE_DESCRIPTOR;
  case 0xA:
    return REG_RESOURCE_REQUIREMENTS_LIST;
  case 0xB:
    return REG_QWORD;
  default:
    return static_cast<DWORD>(code);
  }
}

DWORD TypeCode(DWORD type) {
  return value_format::NormalizeType(type);
}

std::wstring Escape(std::wstring_view text) {
  std::wstring output;
  output.reserve(text.size());
  for (wchar_t character : text) {
    switch (character) {
    case L'\\':
      output += L"\\\\";
      break;
    case L'"':
      output += L"\\\"";
      break;
    case L'\n':
      output += L"\\n";
      break;
    case L'\r':
      output += L"\\r";
      break;
    case L'\t':
      output += L"\\t";
      break;
    case L'\0':
      output += L"\\0";
      break;
    default:
      output.push_back(character);
      break;
    }
  }
  return output;
}

std::wstring Hex(std::span<const BYTE> data) {
  std::wstring output;
  output.reserve(data.size() * 3);
  for (size_t index = 0; index < data.size(); ++index) {
    if (index != 0) {
      output.push_back(L',');
    }
    wchar_t byte[3] = {};
    swprintf_s(byte, L"%02x", data[index]);
    output += byte;
  }
  return output;
}

constexpr size_t kRegFileLineLimit = 80;

void AppendWrapped(std::wstring* output, const std::wstring& line) {
  size_t start = 0;
  size_t indent = 0;
  while (true) {
    const size_t remaining = line.size() - start;
    if (indent + remaining <= kRegFileLineLimit) {
      output->append(indent, L' ');
      output->append(line, start, std::wstring::npos);
      output->append(L"\r\n");
      return;
    }
    const size_t room = kRegFileLineLimit - indent - 1;
    size_t split = line.rfind(L',', start + room - 1);
    if (split == std::wstring::npos || split < start) {
      split = start + room - 1;
    }
    output->append(indent, L' ');
    output->append(line, start, split - start + 1);
    output->append(L"\\\r\n");
    start = split + 1;
    indent = 2;
  }
}

bool IsCanonicalString(const std::vector<BYTE>& data) {
  if (data.size() < sizeof(wchar_t) || data.size() % sizeof(wchar_t) != 0) {
    return false;
  }
  const wchar_t* text = reinterpret_cast<const wchar_t*>(data.data());
  const size_t count = data.size() / sizeof(wchar_t);
  if (text[count - 1] != L'\0') {
    return false;
  }
  for (size_t i = 0; i + 1 < count; ++i) {
    if (text[i] == L'\0') {
      return false;
    }
  }
  return true;
}

std::wstring SerializeValue(const Value& value) {
  const DWORD type = value_format::NormalizeType(value.type);
  if (type == REG_SZ && value.type == REG_SZ &&
      IsCanonicalString(value.data)) {
    std::wstring text;
    if (value_format::DecodeString(value.data, &text)) {
      return L"\"" + Escape(text) + L"\"";
    }
  }
  if (type == REG_DWORD && value.type == REG_DWORD &&
      value.data.size() == sizeof(DWORD)) {
    DWORD number = 0;
    std::memcpy(&number, value.data.data(), sizeof(number));
    wchar_t output[16] = {};
    swprintf_s(output, L"dword:%08x", number);
    return output;
  }
  if (type == REG_BINARY && value.type == REG_BINARY) {
    return L"hex:" + Hex(value.data);
  }
  wchar_t code[16] = {};
  swprintf_s(code, L"%x", TypeCode(value.type));
  return L"hex(" + std::wstring(code) + L"):" + Hex(value.data);
}

} // namespace

void Writer::AppendRemovedKey(std::wstring_view path) {
  if (output_.size() >
      std::wstring_view(L"Windows Registry Editor Version 5.00\r\n\r\n")
          .size()) {
    output_ += L"\r\n";
  }
  output_ += L"[-";
  output_.append(path);
  output_ += L"]\r\n";
}

void Writer::AppendRemovedValues(const std::vector<std::wstring>& names) {
  for (const std::wstring& name : names) {
    std::wstring line;
    if (name.empty()) {
      line = L"@=-";
    } else {
      line.push_back(L'"');
      line += Escape(name);
      line += L"\"=-";
    }
    AppendWrapped(&output_, line);
  }
}

Writer::Writer()
    : output_(L"Windows Registry Editor Version 5.00\r\n\r\n") {}

void Writer::AppendKey(std::wstring_view path,
                       std::vector<const Value*> values, bool sorted) {
  if (output_.size() >
      std::wstring_view(L"Windows Registry Editor Version 5.00\r\n\r\n")
          .size()) {
    output_ += L"\r\n";
  }
  output_.push_back(L'[');
  output_.append(path);
  output_ += L"]\r\n";
  if (sorted) {
  std::sort(values.begin(), values.end(),
            [](const Value* left, const Value* right) {
              if (left->name.empty() != right->name.empty()) {
                return left->name.empty();
              }
              return _wcsicmp(left->name.c_str(), right->name.c_str()) < 0;
            });
  }
  for (const Value* value : values) {
    std::wstring line;
    if (value->name.empty()) {
      line = L"@=";
    } else {
      line.push_back(L'"');
      line += Escape(value->name);
      line += L"\"=";
    }
    line += SerializeValue(*value);
    AppendWrapped(&output_, line);
  }
}

std::wstring Writer::Finish() && {
  return std::move(output_);
}

bool Parse(std::wstring_view content, Document* output,
           const std::atomic_bool* cancel, bool* cancelled,
           std::wstring* error) {
  if (!output) {
    return false;
  }
  output->keys.clear();
  output->key_order.clear();
  if (cancelled) {
    *cancelled = false;
  }
  auto stopped = [&] {
    const bool value = cancel && cancel->load();
    if (value && cancelled) {
      *cancelled = true;
    }
    return value;
  };

  std::vector<std::wstring> lines;
  std::wstring continued;
  size_t start = 0;
  while (start < content.size()) {
    if (stopped()) {
      return false;
    }
    size_t end = content.find(L'\n', start);
    if (end == std::wstring_view::npos) {
      end = content.size();
    }
    std::wstring line(content.substr(start, end - start));
    if (!line.empty() && line.back() == L'\r') {
      line.pop_back();
    }
    start = end + 1;
    continued += line;
    while (!continued.empty() &&
           (continued.back() == L' ' || continued.back() == L'\t')) {
      continued.pop_back();
    }
    if (!continued.empty() && continued.back() == L'\\') {
      continued.pop_back();
      continue;
    }
    lines.push_back(std::move(continued));
    continued.clear();
  }
  if (!continued.empty()) {
    lines.push_back(std::move(continued));
  }

  auto fail = [&](const std::wstring& line) {
    if (error) {
      std::wstring shown = line.size() > 80 ? line.substr(0, 80) + L"..." : line;
      *error = L"The file contains an entry RegKit cannot parse:\n" + shown;
    }
    return false;
  };

  Key* current_key = nullptr;
  for (const auto& raw : lines) {
    if (stopped()) {
      return false;
    }
    const std::wstring line = Trim(raw);
    if (line.empty() || line.front() == L';' ||
        registry_path::StartsWith(line, L"Windows Registry Editor") ||
        registry_path::StartsWith(line, L"REGEDIT4")) {
      continue;
    }
    if (line.front() == L'[' && line.back() == L']') {
      std::wstring path = Trim(
          std::wstring_view(line).substr(1, line.size() - 2));
      bool removed = false;
      if (!path.empty() && path.front() == L'-') {
        removed = true;
        path = Trim(std::wstring_view(path).substr(1));
      }
      if (path.empty()) {
        return fail(line);
      }
      const std::wstring lower = util::ToLower(path);
      auto [iterator, inserted] =
          output->keys.try_emplace(lower, Key{path, {}});
      if (inserted) {
        output->key_order.push_back(path);
      }
      iterator->second.removed = removed;
      current_key = removed ? nullptr : &iterator->second;
      continue;
    }
    if (!current_key) {
      return fail(line);
    }
    const size_t equals = FindAssignment(line);
    if (equals == std::wstring::npos) {
      return fail(line);
    }
    const std::wstring name_text = Trim(
        std::wstring_view(line).substr(0, equals));
    const std::wstring data_text = Trim(
        std::wstring_view(line).substr(equals + 1));
    if (name_text.empty() || data_text.empty()) {
      return fail(line);
    }

    Value value;
    if (name_text == L"@") {
      value.name.clear();
    } else if (!ParseQuoted(name_text, &value.name)) {
      return fail(line);
    }

    if (data_text == L"-") {
      current_key->values.erase(util::ToLower(value.name));
      current_key->removed_values.push_back(value.name);
      continue;
    }

    if (data_text.front() == L'"') {
      std::wstring text;
      if (!ParseQuoted(data_text, &text)) {
        return fail(line);
      }
      value.type = REG_SZ;
      value.data = value_format::StringData(text);
    } else if (registry_path::StartsWith(data_text, L"dword:")) {
      const std::wstring number_text = Trim(
          std::wstring_view(data_text).substr(6));
      if (number_text.empty()) {
        return fail(line);
      }
      wchar_t* stop = nullptr;
      errno = 0;
      const unsigned long parsed = wcstoul(number_text.c_str(), &stop, 16);
      if (!stop || *stop != L'\0' || errno == ERANGE ||
          parsed > 0xFFFFFFFFul) {
        return fail(line);
      }
      const DWORD number = static_cast<DWORD>(parsed);
      value.type = REG_DWORD;
      value.data.resize(sizeof(number));
      std::memcpy(value.data.data(), &number, sizeof(number));
    } else if (registry_path::StartsWith(data_text, L"hex")) {
      const size_t colon = data_text.find(L':');
      if (colon == std::wstring::npos) {
        return fail(line);
      }
      value.type = REG_BINARY;
      const size_t open = data_text.find(L'(');
      const size_t close = data_text.find(L')');
      if (open != std::wstring::npos && close != std::wstring::npos &&
          close > open && close < colon) {
        const std::wstring code =
            data_text.substr(open + 1, close - open - 1);
        wchar_t* stop = nullptr;
        errno = 0;
        const unsigned long parsed = wcstoul(code.c_str(), &stop, 16);
        if (code.empty() || !stop || *stop != L'\0' || errno == ERANGE) {
          return fail(line);
        }
        value.type = TypeFromCode(parsed);
      } else if (colon != 3) {
        return fail(line);
      }
      if (!value_format::ParseHex(
              std::wstring_view(data_text).substr(colon + 1), &value.data)) {
        return fail(line);
      }
    } else {
      return fail(line);
    }
    current_key->values[util::ToLower(value.name)] = std::move(value);
  }
  return true;
}

bool Load(const std::wstring& path, Document* output, std::wstring* error,
          const std::atomic_bool* cancel, bool* cancelled) {
  std::wstring content;
  if (!util::ReadTextFile(path, &content, nullptr, 32ull * 1024ull * 1024ull)) {
    if (error) {
      *error = L"Failed to read registry file.";
    }
    return false;
  }
  return Parse(content, output, cancel, cancelled, error);
}

std::wstring Serialize(const Document& document) {
  Writer writer;
  for (const auto& ordered_path : document.key_order) {
    auto key = document.keys.find(util::ToLower(ordered_path));
    if (key == document.keys.end()) {
      continue;
    }
    if (key->second.removed) {
      writer.AppendRemovedKey(key->second.path);
      continue;
    }
    std::vector<const Value*> values;
    values.reserve(key->second.values.size());
    for (const auto& entry : key->second.values) {
      values.push_back(&entry.second);
    }
    writer.AppendKey(key->second.path, std::move(values));
    writer.AppendRemovedValues(key->second.removed_values);
  }
  return std::move(writer).Finish();
}

} // namespace regkit::regfile
