// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"
#include "win32/translation.h"

namespace regkit
{

using namespace window_detail;

ui::ColumnSet* MainWindow::Impl::ColumnSetFor(HWND list)
{
    if (list == browse_.values().hwnd())
    {
        return &browse_.columns();
    }
    if (list == history_list_)
    {
        return &history_columns_;
    }
    if (list == search_results_list_)
    {
        return IsCompareTabSelected() ? &compare_columns_ : &search_columns_;
    }
    return nullptr;
}

void MainWindow::Impl::ApplyColumns(HWND list)
{
    if (list == browse_.values().hwnd())
    {
        ApplyValueColumns();
    }
    else if (list == history_list_)
    {
        ApplyHistoryColumns();
    }
    else if (list == search_results_list_)
    {
        ApplySearchColumns(IsCompareTabSelected());
    }
}

void MainWindow::Impl::ShowHeaderMenu(HWND list, POINT screen_pt)
{
    ui::ColumnSet* set = ColumnSetFor(list);
    HWND header_hwnd = ListView_GetHeader(list);
    if (!set || !header_hwnd)
    {
        return;
    }
    const int unavailable_column = list == search_results_list_ && IsCompareTabSelected() && !IsCompareResultColumnAvailable() ? 4 : -1;
    POINT client_pt = screen_pt;
    ScreenToClient(header_hwnd, &client_pt);
    HDHITTESTINFO hit = {};
    hit.pt = client_pt;
    const int column_hit = static_cast<int>(SendMessageW(header_hwnd, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit)));

    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return;
    }
    const UINT fit_flags = MF_STRING | ((column_hit >= 0) ? 0 : MF_GRAYED);
    AppendMenuW(menu, fit_flags, cmd::kHeaderSizeToFit, util::Tr(L"Size column to fit"));
    AppendMenuW(menu, MF_STRING, cmd::kHeaderSizeAll, util::Tr(L"Size all columns to fit"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const auto shown = [&](size_t i) { return static_cast<int>(i) != unavailable_column && (i >= set->visible.size() || set->visible[i]); };
    for (size_t i = 0; i < set->items.size(); ++i)
    {
        if (static_cast<int>(i) != unavailable_column)
        {
            AppendMenuW(menu, MF_STRING | (shown(i) ? MF_CHECKED : MF_UNCHECKED), cmd::kHeaderToggleBase + static_cast<int>(i), set->items[i].title.c_str());
        }
    }

    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen_pt.x, screen_pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    if (command == cmd::kHeaderSizeToFit && column_hit >= 0)
    {
        const int subitem = GetListViewColumnSubItem(list, column_hit);
        ListView_SetColumnWidth(list, column_hit, LVSCW_AUTOSIZE_USEHEADER);
        if (subitem >= 0 && static_cast<size_t>(subitem) < set->widths.size())
        {
            set->widths[static_cast<size_t>(subitem)] = ListView_GetColumnWidth(list, column_hit);
        }
    }
    else if (command == cmd::kHeaderSizeAll)
    {
        int last_visible = -1;
        for (size_t i = 0; i < set->items.size(); ++i)
        {
            last_visible = shown(i) ? static_cast<int>(i) : last_visible;
        }
        for (size_t i = 0; i < set->items.size(); ++i)
        {
            const int display = shown(i) ? FindListViewColumnBySubItem(list, static_cast<int>(i)) : -1;
            if (display < 0)
            {
                continue;
            }
            if (static_cast<int>(i) == last_visible)
            {
                ListView_SetColumnWidth(list, display, CalcListViewColumnFitWidth(list, static_cast<int>(i), set->items[i].width));
            }
            else
            {
                ListView_SetColumnWidth(list, display, LVSCW_AUTOSIZE_USEHEADER);
            }
            set->widths[i] = ListView_GetColumnWidth(list, display);
        }
    }
    else if (command >= cmd::kHeaderToggleBase)
    {
        const size_t index = static_cast<size_t>(command - cmd::kHeaderToggleBase);
        if (index >= set->items.size() || index >= set->visible.size() || static_cast<int>(index) == unavailable_column)
        {
            return;
        }
        if (set->visible[index])
        {
            const int display = FindListViewColumnBySubItem(list, static_cast<int>(index));
            const int width = display >= 0 ? ListView_GetColumnWidth(list, display) : 0;
            set->widths[index] = width > 0 ? width : set->widths[index];
        }
        else if (set->widths[index] <= 0)
        {
            set->widths[index] = set->items[index].width;
        }
        set->visible[index] = !set->visible[index];
        ApplyColumns(list);
    }

    if (list == browse_.values().hwnd() && command != 0)
    {
        SaveSettings();
    }
}

} // namespace regkit
