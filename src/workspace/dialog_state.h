// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace regkit::workspace
{

// last used dialog options, kept in the cache folder
using DialogState = std::map<std::wstring, std::wstring, std::less<>>;

DialogState LoadDialogState();
void SaveDialogState(const DialogState& state);

// one key list both reads the options into a dialog result and writes them back
class DialogFields
{
  public:
    DialogFields(DialogState* state, bool write)
        : state_(state), write_(write)
    {
    }
    void Field(std::wstring_view key, std::wstring* value);
    void Field(std::wstring_view key, bool* value);
    void Field(std::wstring_view key, uint64_t* value);
    template <typename T>
    void Field(std::wstring_view key, T* value, T last)
    {
        uint64_t number = static_cast<uint64_t>(*value);
        Field(key, &number);
        if (number <= static_cast<uint64_t>(last))
        {
            *value = static_cast<T>(number);
        }
    }

  private:
    DialogState* state_;
    bool write_;
};

} // namespace regkit::workspace
