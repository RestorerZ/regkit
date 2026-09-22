// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search/result_file.h"

#include "records/escaped_fields.h"
#include "win32/file_text.h"

#include <cstdint>
#include <string_view>
#include <utility>

namespace regkit::search
{

namespace
{

constexpr wchar_t kRecordVersionTag[] = L"#regkit-search-2";
constexpr uint64_t kMaxResultFileBytes = 256ull * 1024 * 1024;

MatchField ToMatchField(int value)
{
    return value < 0 || value > static_cast<int>(MatchField::kDefault) ? MatchField::kNone
                                                                    : static_cast<MatchField>(value);
}

Result ParseLegacyRecord(const std::vector<std::wstring>& fields)
{
    Result result;
    result.key_path = fields[0];
    result.value_name = fields[2];
    result.type = static_cast<DWORD>(_wtoi(fields[5].c_str()));
    result.data_text = fields[6];
    result.data_size = static_cast<DWORD>(_wtoi(fields[7].c_str()));
    const size_t base = fields.size() >= 14 ? 10 : 9;
    result.kind = _wtoi(fields[base].c_str()) != 0 ? ResultKind::kKey : ResultKind::kValue;
    result.match_field = ToMatchField(_wtoi(fields[base + 1].c_str()));
    const int start = _wtoi(fields[base + 2].c_str());
    result.match_start = start < 0 ? 0u : static_cast<uint32_t>(start);
    result.match_length = static_cast<uint32_t>(_wtoi(fields[base + 3].c_str()));
    bool loaded = true;
    if (fields.size() > base + 4)
    {
        loaded = _wtoi(fields[base + 4].c_str()) != 0;
    }
    result.data_state = result.kind == ResultKind::kKey ? DataState::kNotApplicable
                        : loaded                        ? DataState::kLoaded
                                                        : DataState::kNotLoaded;
    return result;
}

bool ParseVersionedRecord(std::vector<std::wstring>&& fields, Result* out)
{
    uint64_t type = 0;
    uint64_t data_size = 0;
    uint64_t modified = 0;
    uint64_t match_field = 0;
    uint64_t match_start = 0;
    uint64_t match_length = 0;
    uint64_t kind = 0;
    uint64_t state = 0;
    uint64_t source = 0;
    if (!record_fields::ParseUnsigned(fields[3], MAXDWORD, &type) ||
        !record_fields::ParseUnsigned(fields[4], MAXDWORD, &data_size) ||
        !record_fields::ParseUnsigned(fields[5], UINT64_MAX, &modified) ||
        !record_fields::ParseUnsigned(fields[6], static_cast<uint64_t>(MatchField::kDefault), &match_field) ||
        !record_fields::ParseUnsigned(fields[7], UINT32_MAX, &match_start) ||
        !record_fields::ParseUnsigned(fields[8], UINT32_MAX, &match_length) ||
        !record_fields::ParseUnsigned(fields[9], static_cast<uint64_t>(ResultKind::kTraceValue), &kind) ||
        !record_fields::ParseUnsigned(fields[10], static_cast<uint64_t>(DataState::kLoaded), &state) ||
        (fields.size() > 11 && !record_fields::ParseUnsigned(fields[11], UINT16_MAX, &source)))
    {
        return false;
    }
    Result result;
    result.key_path = std::move(fields[0]);
    result.value_name = std::move(fields[1]);
    result.data_text = std::move(fields[2]);
    result.type = static_cast<DWORD>(type);
    result.data_size = static_cast<DWORD>(data_size);
    result.modified.dwLowDateTime = static_cast<DWORD>(modified & 0xFFFFFFFFull);
    result.modified.dwHighDateTime = static_cast<DWORD>(modified >> 32);
    result.match_field = static_cast<MatchField>(match_field);
    result.match_start = static_cast<uint32_t>(match_start);
    result.match_length = static_cast<uint32_t>(match_length);
    result.kind = static_cast<ResultKind>(kind);
    result.data_state = static_cast<DataState>(state);
    result.source = static_cast<uint16_t>(source);
    *out = std::move(result);
    return true;
}

} // namespace

bool ParseResults(const std::wstring& content, std::vector<Result>* out)
{
    if (!out)
    {
        return false;
    }
    std::vector<Result> results;
    bool versioned = false;
    for (const std::wstring_view line : record_fields::Lines(content))
    {
        if (line.empty())
        {
            continue;
        }
        if (line == kRecordVersionTag)
        {
            versioned = true;
            continue;
        }
        auto fields = record_fields::DecodeRecord(line);
        if (versioned)
        {
            Result record;
            if (fields.size() < 11 || fields.size() > 12 || !ParseVersionedRecord(std::move(fields), &record))
            {
                return false;
            }
            results.push_back(std::move(record));
            continue;
        }
        if (fields.size() < 13)
        {
            continue;
        }
        results.push_back(ParseLegacyRecord(fields));
    }
    *out = std::move(results);
    return true;
}

std::wstring SerializeResults(const std::vector<Result>& results)
{
    std::wstring content = kRecordVersionTag;
    content += L'\n';
    for (const Result& result : results)
    {
        const uint64_t modified =
            (static_cast<uint64_t>(result.modified.dwHighDateTime) << 32) | result.modified.dwLowDateTime;
        record_fields::AppendRecord(
            &content,
            {result.key_path, result.value_name, result.data_text, std::to_wstring(result.type), std::to_wstring(result.data_size), std::to_wstring(modified), std::to_wstring(static_cast<int>(result.match_field)), std::to_wstring(result.match_start), std::to_wstring(result.match_length), std::to_wstring(static_cast<int>(result.kind)), std::to_wstring(static_cast<int>(result.data_state)), std::to_wstring(result.source)}
        );
    }
    return content;
}

bool LoadResults(const std::wstring& path, std::vector<Result>* results)
{
    if (!results || path.empty())
    {
        return false;
    }
    std::wstring content;
    return util::ReadTextFile(path, &content, nullptr, kMaxResultFileBytes) && ParseResults(content, results);
}

bool SaveResults(const std::wstring& path, const std::vector<Result>& results)
{
    return !path.empty() && util::WriteTextFile(path, SerializeResults(results), false);
}

} // namespace regkit::search
