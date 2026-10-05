// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <initializer_list>

namespace regkit::appearance
{

int TextFitWidth(HWND control);
int TextFitWidth(std::initializer_list<HWND> controls);
int PlaceButtonRow(std::initializer_list<HWND> buttons, int right, int y, int min_width, int height, int gap);
bool GrowDialogWidth(HWND dialog, int client_width);
void FitDialogHeight(HWND dialog, int client_height);
void LocalizeDialog(HWND dialog);

} // namespace regkit::appearance
