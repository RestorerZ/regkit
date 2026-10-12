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
#include "ui/default_font.h"
#include "ui/feedback.h"
#include "ui/gdi_cache.h"
#include "ui/icon_loader.h"
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

int MappedSubItem(const std::vector<int>& map, int display_index)
{
    return (display_index >= 0 && static_cast<size_t>(display_index) < map.size())
               ? map[static_cast<size_t>(display_index)]
               : display_index;
}

int GetListViewColumnSubItem(HWND list, int display_index)
{
    if (!list || display_index < 0)
    {
        return display_index;
    }
    LVCOLUMNW col = {};
    col.mask = LVCF_SUBITEM;
    if (ListView_GetColumn(list, display_index, &col))
    {
        return col.iSubItem;
    }
    return display_index;
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
    const COLORREF old_color = SetTextColor(hdc, Theme::Current().FocusColor());
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

int FindListViewColumnBySubItem(HWND list, int subitem)
{
    if (!list || subitem < 0)
    {
        return -1;
    }
    HWND header = ListView_GetHeader(list);
    int count = header ? Header_GetItemCount(header) : 0;
    for (int i = 0; i < count; ++i)
    {
        if (GetListViewColumnSubItem(list, i) == subitem)
        {
            return i;
        }
    }
    return -1;
}

int FetchListViewItemText(HWND list, int index, int column, std::wstring* buffer)
{
    if (!list || !buffer)
    {
        return 0;
    }
    if (buffer->empty())
    {
        buffer->resize(1);
    }
    // the returned length is what fit, so grow while the text fills the buffer
    constexpr size_t kMaxText = 1u << 20;
    LVITEMW item = {};
    item.iSubItem = column;
    for (;;)
    {
        item.pszText = buffer->data();
        item.cchTextMax = static_cast<int>(buffer->size());
        const int length = static_cast<int>(
            SendMessageW(list, LVM_GETITEMTEXTW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item))
        );
        if (static_cast<size_t>(length) + 1 < buffer->size() || buffer->size() >= kMaxText)
        {
            return length;
        }
        buffer->resize(buffer->size() * 2);
    }
}

int CalcListViewColumnFitWidth(HWND list, int column, int min_width)
{
    if (!list || column < 0)
    {
        return min_width;
    }
    int display_index = FindListViewColumnBySubItem(list, column);
    if (display_index < 0)
    {
        return min_width;
    }
    int width = min_width;
    wchar_t header_text[256] = {};
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT;
    col.pszText = header_text;
    col.cchTextMax = static_cast<int>(_countof(header_text));
    if (ListView_GetColumn(list, display_index, &col))
    {
        int header_width = ListView_GetStringWidth(list, header_text) + 18;
        width = std::max(width, header_width);
    }

    int count = ListView_GetItemCount(list);
    std::wstring buffer;
    buffer.resize(256);
    for (int i = 0; i < count; ++i)
    {
        int length = FetchListViewItemText(list, i, column, &buffer);
        if (length > 0)
        {
            int text_width = ListView_GetStringWidth(list, buffer.c_str()) + 18;
            if (text_width > width)
            {
                width = text_width;
            }
        }
    }
    return width;
}

} // namespace regkit::window_detail
