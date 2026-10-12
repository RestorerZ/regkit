// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "trace/trace_parser.h"

#include <string>

namespace regkit::trace
{

// a trace key as a registry path, with the current control set and the current user mapped
std::wstring NormalizeKeyPathBasic(const std::wstring& text);
// as above, with symbolic links followed to their targets
std::wstring NormalizeKeyPath(const std::wstring& text);
std::wstring NormalizeSelectionPath(const std::wstring& text);
Normalizers PathNormalizers();
// HKLM\SYSTEM\ControlSetNNN mapped to the control set in use, empty when nothing changes
std::wstring MapControlSetToCurrent(const std::wstring& path);

} // namespace regkit::trace
