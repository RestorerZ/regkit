// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/dialog_layout.h"

#include "ui/theme.h"
#include "win32/translation.h"

#include <win32ui/dialog/dialog_layout.h>
#include <win32ui/dialog/single_line_edit.h>
#include <win32ui/list_view/list_view.h>

#include <commctrl.h>

namespace regkit::appearance
{

const win32ui::DialogTheme* DialogThemeHooks()
{
    static const win32ui::DialogTheme hooks = [] {
        win32ui::DialogTheme theme = win32ui::ThemeDialogHooks();
        theme.apply = ApplyDialogTheme;
        theme.prepare = [](HWND dialog) {
            util::TranslateDialog(dialog);
            for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            {
                wchar_t class_name[16] = {};
                GetClassNameW(child, class_name, static_cast<int>(_countof(class_name)));
                if (_wcsicmp(class_name, L"ComboBox") == 0)
                {
                    SendMessageW(child, CB_LIMITTEXT, 0, 0);
                }
                else if (_wcsicmp(class_name, L"Edit") == 0)
                {
                    SendMessageW(child, EM_SETLIMITTEXT, 0, 0);
                    const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
                    if ((style & ES_MULTILINE) && !(style & ES_READONLY))
                    {
                        win32ui::KeepSingleLine(child);
                    }
                }
            }
        };
        return theme;
    }();
    return &hooks;
}

win32ui::StatusBarHooks StatusBarThemeHooks()
{
    win32ui::StatusBarHooks hooks;
    hooks.frame = DialogThemeHooks();
    return hooks;
}

win32ui::TabStripHooks TabStripThemeHooks()
{
    win32ui::TabStripHooks hooks;
    hooks.frame = DialogThemeHooks();
    return hooks;
}

void ApplyDialogTheme(HWND dialog)
{
    win32ui::ApplyThemeToWindow(dialog);
    win32ui::AddSelectAllShortcut(dialog);
    // refresh each list header, grid button and cell tooltip
    EnumChildWindows(
        dialog,
        [](HWND child, LPARAM) -> BOOL {
            wchar_t class_name[32] = {};
            GetClassNameW(child, class_name, static_cast<int>(_countof(class_name)));
            if (wcscmp(class_name, WC_LISTVIEWW) == 0)
            {
                win32ui::RefreshListView(child);
            }
            return TRUE;
        },
        0
    );
    InvalidateRect(dialog, nullptr, TRUE);
}

} // namespace regkit::appearance
