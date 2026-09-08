// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <windows.h>

#include "frame/command_ids.h"

namespace regkit::frame {

struct ShortcutBinding {
  BYTE modifiers;
  WORD key;
  int command;
  const wchar_t* text;
};

inline constexpr ShortcutBinding kShortcutBindings[] = {
    {FCONTROL, 'C', cmd::kEditCopy, L"Ctrl+C"},
    {FCONTROL, 'V', cmd::kEditPaste, L"Ctrl+V"},
    {FCONTROL, 'A', cmd::kViewSelectAll, L"Ctrl+A"},
    {FCONTROL, 'Z', cmd::kEditUndo, L"Ctrl+Z"},
    {FCONTROL, 'Y', cmd::kEditRedo, L"Ctrl+Y"},
    {FCONTROL, 'F', cmd::kEditFind, L"Ctrl+F"},
    {FCONTROL, 'G', cmd::kEditGoTo, L"Ctrl+G"},
    {FCONTROL, 'H', cmd::kEditReplace, L"Ctrl+H"},
    {FCONTROL, 'S', cmd::kFileSave, L"Ctrl+S"},
    {FCONTROL, 'E', cmd::kFileExport, L"Ctrl+E"},
    {FCONTROL, 'N', cmd::kRegistryLocal, L"Ctrl+N"},
    {FCONTROL, 'R', cmd::kRegistryNetwork, L"Ctrl+R"},
    {FCONTROL, 'O', cmd::kRegistryOffline, L"Ctrl+O"},
    {FCONTROL, 'K', cmd::kViewFocusFilter, L"Ctrl+K"},
    {FCONTROL, 'W', cmd::kTabClose, L"Ctrl+W"},
    {FCONTROL, VK_TAB, cmd::kTabNext, L"Ctrl+Tab"},
    {FCONTROL | FSHIFT, VK_TAB, cmd::kTabPrevious, L"Ctrl+Shift+Tab"},
    {FCONTROL | FSHIFT, 'C', cmd::kEditCopyKey, L"Ctrl+Shift+C"},
    {FCONTROL | FSHIFT, 'N', cmd::kWindowNew, L"Ctrl+Shift+N"},
    {FCONTROL | FSHIFT, 'O', cmd::kFileOpenRegFile, L"Ctrl+Shift+O"},
    {FCONTROL | FSHIFT, 'H', cmd::kViewHistory, L"Ctrl+Shift+H"},
    {0, VK_DELETE, cmd::kEditDelete, L"Del"},
    {0, VK_F1, cmd::kHelpContents, L"F1"},
    {0, VK_F2, cmd::kEditRename, L"F2"},
    {0, VK_F5, cmd::kViewRefresh, L"F5"},
    {0, VK_F7, cmd::kNewKey, L"F7"},
    {FALT, VK_RETURN, cmd::kEditPermissions, L"Alt+Enter"},
    {FALT, VK_LEFT, cmd::kNavBack, L"Alt+Left"},
    {FALT, VK_RIGHT, cmd::kNavForward, L"Alt+Right"},
    {FALT, VK_UP, cmd::kNavUp, L"Alt+Up"},
};

} // namespace regkit::frame
