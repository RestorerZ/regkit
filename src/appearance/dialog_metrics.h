// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

namespace regkit::appearance::metrics {

inline constexpr int kDialogContentMargin = 12;
inline constexpr int kDialogButtonRightMargin = 16;
inline constexpr int kDialogButtonBottomMargin = 14;
inline constexpr int kBlockGap = 12;
inline constexpr int kRowGap = 2;
inline constexpr int kCheckHeight = 20;
inline constexpr int kRowPitch = 22;
inline constexpr int kLabelHeight = 18;
inline constexpr int kControlHeight = 22;
inline constexpr int kDetailLineHeight = 16;
inline constexpr int kControlPitch = 26;
inline constexpr int kCheckInset = 3;
inline constexpr int kLabelInset = 2;
inline constexpr int kGroupTop = 18;
inline constexpr int kGroupBottom = 8;
inline constexpr int kGroupInset = 12;
inline constexpr int kLabelGap = 8;
inline constexpr int kButtonMinWidth = 70;
inline constexpr int kButtonHeight = 22;
inline constexpr int kButtonGap = 10;

inline int Scaled(
    int value,
    UINT dpi
) {
  return dpi == 96 ? value : MulDiv(value, static_cast<int>(dpi), 96);
}

} // namespace regkit::appearance::metrics
