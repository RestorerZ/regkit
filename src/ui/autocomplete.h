// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace regkit::appearance
{

using AutoCompleteSuggest = std::function<std::vector<std::wstring>(const std::wstring&)>;

bool AttachAutoComplete(HWND edit, AutoCompleteSuggest suggest);
void SetAutoCompleteEnabled(bool enabled);
void SetKeySuggest(AutoCompleteSuggest suggest);
std::vector<std::wstring> SuggestKeys(const std::wstring& text);
std::vector<std::wstring> SuggestComboPaths(HWND combo, const std::wstring& text);
void ApplyAutoCompleteTheme();

} // namespace regkit::appearance
