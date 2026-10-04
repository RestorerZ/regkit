// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"
#include "win32/translation.h"

namespace regkit
{

using namespace window_detail;

void MainWindow::Impl::ShowHeaderMenu(HWND list, std::vector<ColumnInfo>& columns, std::vector<int>& widths, std::vector<bool>& visible, POINT screen_pt, int unavailable_column)
{
    HWND header_hwnd = ListView_GetHeader(list);
    if (!header_hwnd)
    {
        return;
    }
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

    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (static_cast<int>(i) == unavailable_column)
        {
            continue;
        }
        const UINT state = i < visible.size() && visible[i] ? MF_CHECKED : MF_UNCHECKED;
        AppendMenuW(menu, MF_STRING | state, cmd::kHeaderToggleBase + static_cast<int>(i), columns[i].title.c_str());
    }

    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen_pt.x, screen_pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    if (command == cmd::kHeaderSizeToFit && column_hit >= 0)
    {
        const int subitem = GetListViewColumnSubItem(list, column_hit);
        ListView_SetColumnWidth(list, column_hit, LVSCW_AUTOSIZE_USEHEADER);
        if (subitem >= 0 && static_cast<size_t>(subitem) < widths.size())
        {
            widths[static_cast<size_t>(subitem)] = ListView_GetColumnWidth(list, column_hit);
        }
    }
    else if (command == cmd::kHeaderSizeAll)
    {
        int last_visible = -1;
        for (size_t i = 0; i < columns.size(); ++i)
        {
            if (static_cast<int>(i) != unavailable_column && (i >= visible.size() || visible[i]))
            {
                last_visible = static_cast<int>(i);
            }
        }
        for (size_t i = 0; i < columns.size(); ++i)
        {
            if (static_cast<int>(i) == unavailable_column || (i < visible.size() && !visible[i]))
            {
                continue;
            }
            const int display = FindListViewColumnBySubItem(list, static_cast<int>(i));
            if (display < 0)
            {
                continue;
            }
            int width = 0;
            if (static_cast<int>(i) == last_visible)
            {
                width = CalcListViewColumnFitWidth(list, static_cast<int>(i), columns[i].width);
                ListView_SetColumnWidth(list, display, width);
            }
            else
            {
                ListView_SetColumnWidth(list, display, LVSCW_AUTOSIZE_USEHEADER);
                width = ListView_GetColumnWidth(list, display);
            }
            widths[i] = width;
        }
    }
    else if (command >= cmd::kHeaderToggleBase)
    {
        const int index = command - cmd::kHeaderToggleBase;
        if (index < 0 || static_cast<size_t>(index) >= columns.size() || index == unavailable_column)
        {
            return;
        }
        const bool show = !(static_cast<size_t>(index) < visible.size() && visible[static_cast<size_t>(index)]);
        if (list == browse_.values().hwnd())
        {
            ToggleValueColumn(index, show);
        }
        else if (list == history_list_)
        {
            ToggleHistoryColumn(index, show);
        }
        else if (list == search_results_list_)
        {
            ToggleSearchColumn(index, show);
        }
    }

    if (list == browse_.values().hwnd() && command != 0)
    {
        SaveSettings();
    }
}

void MainWindow::Impl::ShowValueHeaderMenu(POINT screen_pt)
{
    ShowHeaderMenu(browse_.values().hwnd(), browse_.columns().items, browse_.columns().widths, browse_.columns().visible, screen_pt);
}

void MainWindow::Impl::ShowHistoryHeaderMenu(POINT screen_pt)
{
    ShowHeaderMenu(history_list_, history_columns_, history_column_widths_, history_column_visible_, screen_pt);
}

void MainWindow::Impl::ShowSearchHeaderMenu(POINT screen_pt)
{
    const bool compare = IsCompareTabSelected();
    auto& columns = compare ? compare_columns_ : search_columns_;
    auto& widths = compare ? compare_column_widths_ : search_column_widths_;
    auto& visible = compare ? compare_column_visible_ : search_column_visible_;
    ShowHeaderMenu(search_results_list_, columns, widths, visible, screen_pt, compare && !IsCompareResultColumnAvailable() ? 4 : -1);
}

void MainWindow::Impl::ToggleValueColumn(int column, bool visible)
{
    if (column < 0 || static_cast<size_t>(column) >= browse_.columns().visible.size())
    {
        return;
    }
    if (visible == browse_.columns().visible[static_cast<size_t>(column)])
    {
        return;
    }

    if (visible)
    {
        int width = browse_.columns().widths[static_cast<size_t>(column)];
        if (width <= 0)
        {
            width = browse_.columns().items[static_cast<size_t>(column)].width;
        }
        browse_.columns().visible[static_cast<size_t>(column)] = true;
        browse_.columns().widths[static_cast<size_t>(column)] = width;
    }
    else
    {
        int display_index = FindListViewColumnBySubItem(browse_.values().hwnd(), column);
        int width = display_index >= 0 ? ListView_GetColumnWidth(browse_.values().hwnd(), display_index)
                                       : browse_.columns().widths[static_cast<size_t>(column)];
        if (width > 0)
        {
            browse_.columns().widths[static_cast<size_t>(column)] = width;
        }
        browse_.columns().visible[static_cast<size_t>(column)] = false;
    }
    ApplyValueColumns();
}

void MainWindow::Impl::ToggleHistoryColumn(int column, bool visible)
{
    if (column < 0 || static_cast<size_t>(column) >= history_column_visible_.size())
    {
        return;
    }
    if (visible == history_column_visible_[static_cast<size_t>(column)])
    {
        return;
    }

    if (visible)
    {
        int width = history_column_widths_[static_cast<size_t>(column)];
        if (width <= 0)
        {
            width = history_columns_[static_cast<size_t>(column)].width;
        }
        history_column_visible_[static_cast<size_t>(column)] = true;
        history_column_widths_[static_cast<size_t>(column)] = width;
    }
    else
    {
        int display_index = FindListViewColumnBySubItem(history_list_, column);
        int width = display_index >= 0 ? ListView_GetColumnWidth(history_list_, display_index)
                                       : history_column_widths_[static_cast<size_t>(column)];
        if (width > 0)
        {
            history_column_widths_[static_cast<size_t>(column)] = width;
        }
        history_column_visible_[static_cast<size_t>(column)] = false;
    }
    ApplyHistoryColumns();
}

void MainWindow::Impl::ToggleSearchColumn(int column, bool visible)
{
    bool compare = IsCompareTabSelected();
    auto& columns = compare ? compare_columns_ : search_columns_;
    auto& widths = compare ? compare_column_widths_ : search_column_widths_;
    auto& visibility = compare ? compare_column_visible_ : search_column_visible_;
    if (column < 0 || static_cast<size_t>(column) >= visibility.size() || static_cast<size_t>(column) >= columns.size())
    {
        return;
    }
    if (visible == visibility[static_cast<size_t>(column)])
    {
        return;
    }

    if (visible)
    {
        int width = widths[static_cast<size_t>(column)];
        if (width <= 0)
        {
            width = columns[static_cast<size_t>(column)].width;
        }
        visibility[static_cast<size_t>(column)] = true;
        widths[static_cast<size_t>(column)] = width;
    }
    else
    {
        int display_index = FindListViewColumnBySubItem(search_results_list_, column);
        int width = display_index >= 0 ? ListView_GetColumnWidth(search_results_list_, display_index)
                                       : widths[static_cast<size_t>(column)];
        if (width > 0)
        {
            widths[static_cast<size_t>(column)] = width;
        }
        visibility[static_cast<size_t>(column)] = false;
    }
    ApplySearchColumns(compare);
}

} // namespace regkit
