// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "trace/trace_parser.h"

#include <string>

namespace regkit::trace
{

// trace key mapped to current control set and user
std::wstring NormalizeKeyPathBasic(const std::wstring& text);
// also follows symbolic links
std::wstring NormalizeKeyPath(const std::wstring& text);
std::wstring NormalizeSelectionPath(const std::wstring& text);
Normalizers PathNormalizers();
// maps HKLM\SYSTEM\ControlSetNNN to current control set, empty if unchanged
std::wstring MapControlSetToCurrent(const std::wstring& path);

} // namespace regkit::trace
