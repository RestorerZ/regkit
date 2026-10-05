// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "registry/value_decoder.h"

namespace regkit::resource_list
{

bool IsResourceType(DWORD type) noexcept;
// decodes REG_RESOURCE_LIST, REG_FULL_RESOURCE_DESCRIPTOR & REG_RESOURCE_REQUIREMENTS_LIST
value_decoder::Decoded Decode(DWORD type, const BYTE* data, size_t size);

} // namespace regkit::resource_list
