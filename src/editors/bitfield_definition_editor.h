// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "editors/bitfield_definition.h"

namespace regkit::editors {

bool EditBitfieldDefinition(HWND owner, bitfield::Definition* definition, bool lock_width);
void ShowBitfieldDefinitionEditor(HWND owner);

} // namespace regkit::editors
