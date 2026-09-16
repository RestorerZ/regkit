// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "changes/value_comments.h"

#include "records/escaped_fields.h"
#include "win32/file_text.h"
#include "win32/text_transform.h"

#include <cwctype>

namespace regkit::changes {
std::wstring ValueComments::ValueKey(
    const std::wstring& path,
    const std::wstring& name,
    DWORD type
) {
  std::wstring key = util::ToLower(path);
  key.push_back(L'\t');
  key.append(util::ToLower(name));
  key.push_back(L'\t');
  key.append(std::to_wstring(type));
  return key;
}

std::wstring ValueComments::NameKey(
    const std::wstring& name,
    DWORD type
) {
  std::wstring key = util::ToLower(name);
  key.push_back(L'\t');
  key.append(std::to_wstring(type));
  return key;
}

bool ValueComments::Load(
    const std::wstring& path
) {
  std::wstring content;
  if (!util::ReadTextFile(path, &content)) {
    return false;
  }
  CommentDocument document;
  if (!ParseComments(content, &document)) {
    return false;
  }
  Clear();
  Merge(document);
  return true;
}

bool ValueComments::Save(
    const std::wstring& path
) const {
  return !path.empty() &&
         util::WriteTextFile(path, SerializeComments(*this), false);
}

void ValueComments::Clear() {
  value_entries_.clear();
  name_entries_.clear();
}

void ValueComments::Merge(
    const CommentDocument& document
) {
  for (const CommentEntry& entry : document.value_entries) {
    value_entries_[ValueKey(entry.path, entry.name, entry.type)] = entry;
  }
  for (const CommentEntry& entry : document.name_entries) {
    name_entries_[NameKey(entry.name, entry.type)] = entry;
  }
}

const std::unordered_map<std::wstring, CommentEntry>&
ValueComments::value_entries() const noexcept {
  return value_entries_;
}

std::unordered_map<std::wstring, CommentEntry>&
ValueComments::value_entries() noexcept {
  return value_entries_;
}

const std::unordered_map<std::wstring, CommentEntry>&
ValueComments::name_entries() const noexcept {
  return name_entries_;
}

std::unordered_map<std::wstring, CommentEntry>&
ValueComments::name_entries() noexcept {
  return name_entries_;
}

bool ParseComments(
    const std::wstring& content,
    CommentDocument* out
) {
  if (!out) {
    return false;
  }
  CommentDocument document;
  for (const std::wstring& line : record_fields::Lines(content)) {
    if (line.empty()) {
      continue;
    }
    const auto fields = record_fields::Split(line);
    if (fields.size() < 5) {
      return false;
    }
    const bool value_entry = util::EqualsInsensitive(fields[0], L"value");
    if (!value_entry && !util::EqualsInsensitive(fields[0], L"name")) {
      return false;
    }
    CommentEntry entry;
    entry.path = record_fields::Unescape(fields[1]);
    entry.name = record_fields::Unescape(fields[2]);
    try {
      size_t consumed = 0;
      const unsigned long parsed = std::stoul(fields[3], &consumed);
      if (consumed != fields[3].size()) {
        return false;
      }
      entry.type = static_cast<DWORD>(parsed);
    } catch (...) {
      return false;
    }
    entry.text = record_fields::Unescape(fields[4]);
    if (util::IsBlank(entry.text)) {
      continue;
    }
    if (value_entry) {
      document.value_entries.push_back(std::move(entry));
    } else {
      document.name_entries.push_back(std::move(entry));
    }
  }
  *out = std::move(document);
  return true;
}

std::wstring SerializeComments(
    const ValueComments& comments
) {
  std::wstring content;
  auto append = [&](const wchar_t* kind, const std::wstring& path, const CommentEntry& entry) {
    if (util::IsBlank(entry.text)) {
      return;
    }
    content.append(kind).append(L"\t").append(record_fields::Escape(path)).append(L"\t");
    content.append(record_fields::Escape(entry.name)).append(L"\t").append(std::to_wstring(entry.type)).append(L"\t");
    content.append(record_fields::Escape(entry.text)).append(L"\n");
  };
  for (const auto& pair : comments.value_entries()) {
    append(L"value", pair.second.path, pair.second);
  }
  for (const auto& pair : comments.name_entries()) {
    append(L"name", L"", pair.second);
  }
  return content;
}
} // namespace regkit::changes
