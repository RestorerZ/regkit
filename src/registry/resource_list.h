// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "records/table.h"

#include <windows.h>

#include <optional>
#include <vector>

namespace regkit::resource_list
{

bool IsResourceType(DWORD type) noexcept;
// decodes REG_RESOURCE_LIST, REG_FULL_RESOURCE_DESCRIPTOR & REG_RESOURCE_REQUIREMENTS_LIST, nullopt when malformed
std::optional<std::vector<records::Table>> Decode(DWORD type, const BYTE* data, size_t size, bool translated = false);

} // namespace regkit::resource_list
