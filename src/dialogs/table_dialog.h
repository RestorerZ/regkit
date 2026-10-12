// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "records/table.h"
#include "win32/windows_config.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace regkit::editors
{

struct TablesRequest
{
    std::wstring title;
    std::wstring identifier;
    std::vector<records::Table> tables;
    std::wstring action_label;
    std::function<void(HWND)> action;
    // rows of tables[check_table] get checkboxes; on_check applies a click and returns false to undo it
    int check_table = -1;
    std::vector<bool> checked;
    std::function<bool(HWND, size_t, bool)> on_check;
};

void ShowTables(HWND owner, const TablesRequest& request);

} // namespace regkit::editors
