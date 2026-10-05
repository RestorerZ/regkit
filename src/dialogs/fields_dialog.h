// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace regkit::editors
{

struct FieldsRequest
{
    std::wstring title;
    std::wstring identifier;
    // an empty label starts a new group
    std::vector<std::pair<std::wstring, std::wstring>> fields;
    std::wstring action_label;
    std::function<void(HWND)> action;
};

void ShowFields(HWND owner, const FieldsRequest& request);

} // namespace regkit::editors
