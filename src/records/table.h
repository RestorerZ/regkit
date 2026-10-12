// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace regkit::records
{

struct Table
{
    std::wstring title;
    std::vector<std::wstring> columns;
    std::vector<std::vector<std::wstring>> rows;
};

} // namespace regkit::records
