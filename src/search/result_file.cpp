// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search/result_file.h"

#include "records/escaped_fields.h"
#include "win32/file_text.h"

#include <cerrno>
#include <cstdint>
#include <string_view>
#include <utility>

namespace regkit::search {

namespace {

constexpr wchar_t kRecordVersionTag[] = L"#regkit-search-2";
constexpr uint64_t kMaxResultFileBytes = 256ull * 1024 * 1024;

std::wstring FileTimeToString(const FILETIME& time) {
  const unsigned long long value =
      (static_cast<unsigned long long>(time.dwHighDateTime) << 32) |
      static_cast<unsigned long long>(time.dwLowDateTime);
  return std::to_wstring(value);
}

bool ParseNumber(const std::wstring& text, unsigned long long limit,
                 unsigned long long* out) {
  if (!out || text.empty() ||
      text.find_first_not_of(L"0123456789") != std::wstring::npos) {
    return false;
  }
  errno = 0;
  wchar_t* end = nullptr;
  const unsigned long long value = wcstoull(text.c_str(), &end, 10);
  if (!end || *end != L'\0' || errno == ERANGE || value > limit) {
    return false;
  }
  *out = value;
  return true;
}

MatchField ToMatchField(int value) {
  return value < 0 || value > static_cast<int>(MatchField::kData)
             ? MatchField::kNone
             : static_cast<MatchField>(value);
}

Result ParseLegacyRecord(const std::vector<std::wstring>& fields) {
  Result result;
  result.key_path = record_fields::Unescape(fields[0]);
  result.value_name = record_fields::Unescape(fields[2]);
  result.type = static_cast<DWORD>(_wtoi(fields[5].c_str()));
  result.data_text = record_fields::Unescape(fields[6]);
  result.data_size =
      static_cast<DWORD>(_wtoi(record_fields::Unescape(fields[7]).c_str()));
  const size_t base = fields.size() >= 14 ? 10 : 9;
  result.kind = _wtoi(fields[base].c_str()) != 0 ? ResultKind::kKey
                                                 : ResultKind::kValue;
  result.match_field = ToMatchField(_wtoi(fields[base + 1].c_str()));
  const int start = _wtoi(fields[base + 2].c_str());
  result.match_start = start < 0 ? 0u : static_cast<uint32_t>(start);
  result.match_length = static_cast<uint32_t>(_wtoi(fields[base + 3].c_str()));
  bool loaded = true;
  if (fields.size() > base + 4) {
    loaded = _wtoi(fields[base + 4].c_str()) != 0;
  }
  result.data_state = result.kind == ResultKind::kKey ? DataState::kNotApplicable
                      : loaded                        ? DataState::kLoaded
                                                      : DataState::kNotLoaded;
  return result;
}

bool ParseVersionedRecord(const std::vector<std::wstring>& fields,
                          Result* out) {
  if (!out) {
    return false;
  }
  Result result;
  result.key_path = record_fields::Unescape(fields[0]);
  result.value_name = record_fields::Unescape(fields[1]);
  result.data_text = record_fields::Unescape(fields[2]);

  unsigned long long type = 0;
  unsigned long long data_size = 0;
  unsigned long long modified = 0;
  unsigned long long match_field = 0;
  unsigned long long match_start = 0;
  unsigned long long match_length = 0;
  unsigned long long kind = 0;
  unsigned long long state = 0;
  if (!ParseNumber(fields[3], MAXDWORD, &type) ||
      !ParseNumber(fields[4], MAXDWORD, &data_size) ||
      !ParseNumber(fields[5], MAXULONGLONG, &modified) ||
      !ParseNumber(fields[6], static_cast<unsigned long long>(MatchField::kData),
                  &match_field) ||
      !ParseNumber(fields[7], UINT32_MAX, &match_start) ||
      !ParseNumber(fields[8], UINT32_MAX, &match_length) ||
      !ParseNumber(fields[9],
                  static_cast<unsigned long long>(ResultKind::kTraceValue),
                  &kind) ||
      !ParseNumber(fields[10], static_cast<unsigned long long>(DataState::kLoaded),
                  &state)) {
    return false;
  }

  result.type = static_cast<DWORD>(type);
  result.data_size = static_cast<DWORD>(data_size);
  result.modified.dwLowDateTime = static_cast<DWORD>(modified & 0xFFFFFFFFull);
  result.modified.dwHighDateTime = static_cast<DWORD>(modified >> 32);
  result.match_field = static_cast<MatchField>(match_field);
  result.match_start = static_cast<uint32_t>(match_start);
  result.match_length = static_cast<uint32_t>(match_length);
  result.kind = static_cast<ResultKind>(kind);
  result.data_state = static_cast<DataState>(state);
  if (fields.size() > 11) {
    unsigned long long source = 0;
    if (!ParseNumber(fields[11], UINT16_MAX, &source)) {
      return false;
    }
    result.source = static_cast<uint16_t>(source);
  }
  *out = std::move(result);
  return true;
}

} // namespace

bool ParseResults(const std::wstring& content,
                  std::vector<Result>* out) {
  if (!out) {
    return false;
  }
  std::vector<Result> results;
  bool versioned = false;
  size_t start = 0;
  while (start < content.size()) {
    size_t end = content.find(L'\n', start);
    if (end == std::wstring::npos) {
      end = content.size();
    }
    std::wstring line = content.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == L'\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    if (line == kRecordVersionTag) {
      versioned = true;
      continue;
    }
    const auto fields = record_fields::Split(line);
    if (versioned) {
      Result record;
      if (fields.size() < 11 || fields.size() > 12 ||
          !ParseVersionedRecord(fields, &record)) {
        return false;
      }
      results.push_back(std::move(record));
      continue;
    }
    if (fields.size() < 13) {
      continue;
    }
    results.push_back(ParseLegacyRecord(fields));
  }
  *out = std::move(results);
  return true;
}

std::wstring SerializeResults(const std::vector<Result>& results) {
  std::wstring content = kRecordVersionTag;
  content += L'\n';
  for (const auto& result : results) {
    content += record_fields::Escape(result.key_path) + L'\t';
    content += record_fields::Escape(result.value_name) + L'\t';
    content += record_fields::Escape(result.data_text) + L'\t';
    content += std::to_wstring(result.type) + L'\t';
    content += std::to_wstring(result.data_size) + L'\t';
    content += FileTimeToString(result.modified) + L'\t';
    content += std::to_wstring(static_cast<int>(result.match_field)) + L'\t';
    content += std::to_wstring(result.match_start) + L'\t';
    content += std::to_wstring(result.match_length) + L'\t';
    content += std::to_wstring(static_cast<int>(result.kind)) + L'\t';
    content += std::to_wstring(static_cast<int>(result.data_state)) + L'\t';
    content += std::to_wstring(result.source) + L'\n';
  }
  return content;
}

bool LoadResults(const std::wstring& path,
                 std::vector<Result>* results) {
  if (!results || path.empty()) {
    return false;
  }
  std::vector<BYTE> bytes;
  if (!util::ReadFileBytes(path, &bytes, kMaxResultFileBytes)) {
    return false;
  }
  size_t offset = 0;
  if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB &&
      bytes[2] == 0xBF) {
    offset = 3;
  }
  const std::string_view payload(
      reinterpret_cast<const char*>(bytes.data() + offset),
      bytes.size() - offset);
  const std::wstring content = util::Utf8ToWide(payload);
  if (content.empty() && !payload.empty()) {
    return false;
  }
  return ParseResults(content, results);
}

bool SaveResults(const std::wstring& path,
                 const std::vector<Result>& results) {
  return !path.empty() &&
         util::WriteTextFile(path, SerializeResults(results), false);
}

} // namespace regkit::search
