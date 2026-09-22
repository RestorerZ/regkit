// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search/replace.h"
#include "win32/text_transform.h"

#include <windows.h>

#include <utility>

namespace regkit::search
{

Replacer::Replacer(const ReplaceOptions& options)
    : query_(options.find_text), replacement_(options.replace_text), use_regex_(options.use_regex),
      match_case_(options.match_case), match_whole_(options.match_whole), valid_(!query_.empty())
{
    if (!valid_ || !use_regex_)
    {
        return;
    }
    regex::Options regex_options;
    regex_options.ignore_case = !match_case_;
    regex_options.whole = match_whole_;
    pattern_ = regex::Compile(query_, regex_options, &error_);
    session_ = regex::Session(pattern_);
    valid_ = session_.valid();
}

Replacer::Replacer(const Replacer& other)
    : query_(other.query_), replacement_(other.replacement_), pattern_(other.pattern_), session_(other.pattern_),
      error_(other.error_), use_regex_(other.use_regex_), match_case_(other.match_case_),
      match_whole_(other.match_whole_), valid_(other.valid_)
{
}

const regex::Error& Replacer::error() const noexcept
{
    return error_;
}

bool Replacer::valid() const noexcept
{
    return valid_;
}

regex::Status Replacer::Replace(const std::wstring& text, std::wstring* result) const
{
    if (!result || !valid_)
    {
        return regex::Status::kFailed;
    }
    if (use_regex_)
    {
        return session_.Replace(text, replacement_, result, nullptr);
    }

    if (match_whole_)
    {
        const bool matched = match_case_
                                 ? text == query_
                                 : CompareStringOrdinal(text.c_str(), static_cast<int>(text.size()), query_.c_str(), static_cast<int>(query_.size()), TRUE) == CSTR_EQUAL;
        if (!matched)
        {
            return regex::Status::kNoMatch;
        }
        *result = replacement_;
        return regex::Status::kMatch;
    }

    if (match_case_)
    {
        size_t position = text.find(query_);
        if (position == std::wstring::npos)
        {
            return regex::Status::kNoMatch;
        }
        std::wstring replaced;
        size_t cursor = 0;
        while (position != std::wstring::npos)
        {
            replaced.append(text, cursor, position - cursor);
            replaced.append(replacement_);
            cursor = position + query_.size();
            position = text.find(query_, cursor);
        }
        replaced.append(text, cursor, std::wstring::npos);
        *result = std::move(replaced);
        return regex::Status::kMatch;
    }

    size_t cursor = 0;
    std::wstring replaced;
    bool matched = false;
    while (cursor < text.size())
    {
        const int position = util::FindInsensitive(std::wstring_view(text).substr(cursor), query_);
        if (position < 0)
        {
            break;
        }
        const size_t match = cursor + static_cast<size_t>(position);
        replaced.append(text, cursor, match - cursor);
        replaced.append(replacement_);
        cursor = match + query_.size();
        matched = true;
    }
    if (!matched)
    {
        return regex::Status::kNoMatch;
    }
    replaced.append(text, cursor, std::wstring::npos);
    *result = std::move(replaced);
    return regex::Status::kMatch;
}

} // namespace regkit::search
