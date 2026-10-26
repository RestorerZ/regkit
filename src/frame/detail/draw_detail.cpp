// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/detail/draw_detail.h"

#include "frame/window_impl.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <functional>
#include <limits>
#include <string_view>
#include <pathcch.h>
#include <richedit.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <vssym32.h>
#include <windowsx.h>
#include <winternl.h>
#include "defaults/default_loader.h"
#include "dialogs/comment_editor.h"
#include "dialogs/security_dialog.h"
#include "dialogs/value_editor.h"
#include "frame/commands/command_ids.h"
#include "frame/window/message_dispatch.h"
#include "frame/window/message_ids.h"
#include "regfile/reg_file.h"
#include "registry/registry_path.h"
#include "registry/registry_store.h"
#include "registry/value_format.h"
#include "resource.h"
#include "search/result_file.h"
#include "trace/trace_loader.h"
#include "trace/trace_parser.h"
#include "ui/feedback.h"
#include "ui/list_view_support.h"
#include "win32/file_dialog.h"
#include "win32/file_text.h"
#include "win32/process_rights.h"
#include "win32/registry_native.h"
#include "win32/restart.h"
#include "win32/shell_paths.h"
#include "win32/system_error.h"
#include "win32/text_transform.h"
#include "workspace/settings.h"
#include "workspace/tab_state.h"

namespace regkit::window_detail
{

bool GetChildRectInParent(HWND parent, HWND child, RECT* rect)
{
    if (!parent || !child || !rect)
    {
        return false;
    }
    if (!GetWindowRect(child, rect))
    {
        return false;
    }
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(rect), 2);
    return true;
}

void DrawSearchMatchOverlay(HDC hdc, const RECT& cell, std::wstring_view text, int start, int length)
{
    if (text.empty() || start < 0 || length <= 0)
    {
        return;
    }
    int total = static_cast<int>(text.size());
    if (start >= total)
    {
        return;
    }
    if (start + length > total)
    {
        length = total - start;
    }
    constexpr int kMaxOverlayChars = 512;
    if (total > kMaxOverlayChars)
    {
        if (start >= kMaxOverlayChars)
        {
            return;
        }
        text = text.substr(0, static_cast<size_t>(kMaxOverlayChars));
        total = kMaxOverlayChars;
        if (start + length > total)
        {
            length = total - start;
        }
    }
    const int available = cell.right - cell.left;
    SIZE full = {};
    if (available <= 0 || !GetTextExtentPoint32W(hdc, text.data(), total, &full))
    {
        return;
    }
    int visible = total;
    if (full.cx > available)
    {
        SIZE ellipsis = {};
        if (!GetTextExtentPoint32W(hdc, L"...", 3, &ellipsis) || ellipsis.cx >= available)
        {
            return;
        }
        SIZE fitted = {};
        if (!GetTextExtentExPointW(hdc, text.data(), total, available - ellipsis.cx, &visible, nullptr, &fitted))
        {
            return;
        }
    }
    if (start >= visible)
    {
        return;
    }
    if (start + length > visible)
    {
        length = visible - start;
    }
    SIZE prefix = {};
    if (!GetTextExtentPoint32W(hdc, text.data(), start, &prefix))
    {
        return;
    }
    const int x = cell.left + prefix.cx;
    TEXTMETRICW metrics = {};
    GetTextMetricsW(hdc, &metrics);
    const int y = cell.top + (cell.bottom - cell.top - metrics.tmHeight) / 2;
    const int old_mode = SetBkMode(hdc, TRANSPARENT);
    const COLORREF old_color = SetTextColor(hdc, win32ui::CurrentThemeColors().focus);
    ExtTextOutW(hdc, x, y, ETO_CLIPPED, &cell, text.data() + start, static_cast<UINT>(length), nullptr);
    SetTextColor(hdc, old_color);
    SetBkMode(hdc, old_mode);
}

int SearchMatchSubItem(const search::Result& result)
{
    if (result.match_length == 0)
    {
        return -1;
    }
    switch (result.match_field)
    {
    case search::MatchField::kPath:
        return 0;
    case search::MatchField::kName:
        return 1;
    case search::MatchField::kData:
        return 3;
    default:
        return -1;
    }
}

} // namespace regkit::window_detail
