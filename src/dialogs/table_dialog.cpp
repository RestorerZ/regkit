// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "dialogs/table_dialog.h"

#include "ui/dialog_layout.h"
#include "ui/dialog_support.h"
#include "ui/feedback.h"
#include "ui/list_view_support.h"
#include "win32/text_transform.h"
#include "win32/translation.h"

#include "resource.h"

#include <win32ui/dialog/dialog_layout.h>
#include <win32ui/dialog/dialog_metrics.h>
#include <win32ui/dialog/dialog_window.h>
#include <win32ui/dialog/message_dialog.h>
#include <win32ui/list_view/sort.h>
#include <win32ui/menu/popup_menu.h>
#include <win32ui/window_metrics.h>

#include <algorithm>
#include <commctrl.h>
#include <numeric>

namespace regkit::editors
{

namespace
{

constexpr size_t kInitialRows = 10;

struct State
{
    const TablesRequest* request = nullptr;
    std::vector<HWND> titles;
    std::vector<HWND> lists;
    std::vector<int> heights;
    std::vector<int> sort_columns;
    std::vector<bool> sort_ascending;
    int title_height = 0;
    UINT layout_dpi = 0;
    int right_margin_dip = 0;
    int bottom_offset_dip = 0;
    int min_width_dip = 0;
    int min_height_dip = 0;
    bool ready = false;
};

int CALLBACK CompareRows(LPARAM left, LPARAM right, int column, void* context)
{
    const auto& rows = static_cast<const records::Table*>(context)->rows;
    const auto cell = [&](LPARAM row) {
        const auto& cells = rows[static_cast<size_t>(row)];
        return static_cast<size_t>(column) < cells.size() ? cells[static_cast<size_t>(column)].c_str() : L"";
    };
    const wchar_t* a = cell(left);
    const wchar_t* b = cell(right);
    if (wcsncmp(a, L"0x", 2) == 0 && wcsncmp(b, L"0x", 2) == 0)
    {
        const unsigned long long x = wcstoull(a, nullptr, 16);
        const unsigned long long y = wcstoull(b, nullptr, 16);
        return x < y ? -1 : x > y ? 1
                                  : 0;
    }
    return util::CompareListText(a, b);
}

std::wstring Line(const std::vector<std::wstring>& cells)
{
    std::wstring line;
    for (size_t index = 0; index < cells.size(); ++index)
    {
        line.append(index ? L"\t" : L"").append(cells[index]);
    }
    return line.append(L"\r\n");
}

std::wstring TablesText(const TablesRequest& request)
{
    std::wstring text = request.identifier + L"\r\n";
    for (const records::Table& table : request.tables)
    {
        text.append(L"\r\n").append(table.title).append(L"\r\n").append(Line(table.columns));
        for (const auto& row : table.rows)
        {
            text.append(Line(row));
        }
    }
    return text;
}

// check table menu toggles its row, other tables use the default list menu
bool CheckRowMenu(void*, HWND list, int row, POINT screen)
{
    const HWND dialog = GetParent(list);
    const auto* state = reinterpret_cast<const State*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!state)
    {
        return false;
    }
    const auto found = std::find(state->lists.begin(), state->lists.end(), list);
    if (found == state->lists.end() || found - state->lists.begin() != state->request->check_table)
    {
        return false;
    }
    // empty space menus target the selected row
    row = row >= 0 ? row : ListView_GetNextItem(list, -1, LVNI_SELECTED);
    constexpr int kMenuCheck = 1;
    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return true;
    }
    const bool checked = row >= 0 && ListView_GetCheckState(list, row);
    if (row >= 0)
    {
        std::wstring name;
        appearance::FetchListViewItemText(list, row, 0, &name);
        win32ui::AppendMenuItem(menu, MF_STRING | (checked ? MF_CHECKED : 0), kMenuCheck, name.c_str(), L"Space");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    appearance::AppendRowMenuCommands(menu, list);
    const int chosen = win32ui::TrackContextMenu(menu, screen, dialog);
    if (chosen == kMenuCheck)
    {
        ListView_SetCheckState(list, row, !checked);
    }
    else
    {
        appearance::RunRowMenuCommand(list, chosen);
    }
    return true;
}

RECT ControlRect(HWND dialog, int id)
{
    RECT rect = {};
    GetWindowRect(GetDlgItem(dialog, id), &rect);
    MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

int Gap(HWND dialog)
{
    return win32ui::Scale(win32ui::metrics::kBlockGap, win32ui::DpiForWindow(dialog)) / 2;
}

void Layout(HWND dialog, State* state)
{
    if (!state->layout_dpi)
    {
        return;
    }
    const UINT dpi = win32ui::DpiForWindow(dialog);
    if (state->layout_dpi != dpi)
    {
        state->title_height = MulDiv(state->title_height, static_cast<int>(dpi), static_cast<int>(state->layout_dpi));
        for (int& height : state->heights)
        {
            height = MulDiv(height, static_cast<int>(dpi), static_cast<int>(state->layout_dpi));
        }
        state->layout_dpi = dpi;
    }
    RECT client = {};
    GetClientRect(dialog, &client);
    RECT top = ControlRect(dialog, IDC_TABLES_IDENTIFIER);
    RECT bottom = ControlRect(dialog, IDOK);
    // anchors apply after the procedure handles WM_SIZE
    top.right = client.right - win32ui::Scale(state->right_margin_dip, dpi);
    bottom.top = client.bottom - win32ui::Scale(state->bottom_offset_dip, dpi);
    const int gap = Gap(dialog);
    const size_t count = state->lists.size();
    int remaining = bottom.top - top.bottom - gap - static_cast<int>(count) * (state->title_height + gap);
    std::vector<size_t> order(count);
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [state](size_t a, size_t b) { return state->heights[a] < state->heights[b]; });
    std::vector<int> heights(count);
    for (size_t i = 0; i < count; ++i)
    {
        const size_t index = order[i];
        heights[index] = std::max(0, std::min(state->heights[index], remaining / static_cast<int>(count - i)));
        remaining -= heights[index];
    }
    const int width = top.right - top.left;
    int y = top.bottom + gap;
    for (size_t i = 0; i < count; ++i)
    {
        win32ui::Place(state->titles[i], top.left, y, width, state->title_height);
        y += state->title_height;
        win32ui::Place(state->lists[i], top.left, y, width, heights[i]);
        y += heights[i] + gap;
    }
}

int FillList(HWND list, const records::Table& table)
{
    int width = 0;
    std::vector<win32ui::ListColumn> columns;
    for (const std::wstring& title : table.columns)
    {
        columns.push_back({title});
    }
    win32ui::SetListColumns(list, columns);
    for (size_t row = 0; row < table.rows.size(); ++row)
    {
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(row);
        item.lParam = static_cast<LPARAM>(row);
        item.pszText = const_cast<wchar_t*>(table.rows[row].empty() ? L"" : table.rows[row][0].c_str());
        const int index = ListView_InsertItem(list, &item);
        for (size_t column = 1; column < table.rows[row].size(); ++column)
        {
            ListView_SetItemText(list, index, static_cast<int>(column), const_cast<wchar_t*>(table.rows[row][column].c_str()));
        }
    }
    for (size_t column = 0; column < table.columns.size(); ++column)
    {
        width += win32ui::FitListColumn(list, static_cast<int>(column));
    }
    // Reset columns restores fitted widths
    columns = win32ui::ListColumns(list);
    for (win32ui::ListColumn& column : columns)
    {
        column.default_width_dip = column.width_dip;
    }
    win32ui::SetListColumns(list, columns);
    const UINT dpi = win32ui::DpiForWindow(list);
    return width + win32ui::SystemMetricForDpi(SM_CXVSCROLL, dpi) + 2 * win32ui::SystemMetricForDpi(SM_CXEDGE, dpi);
}

void CreateLists(HWND dialog, State* state)
{
    TEXTMETRICW metrics = {};
    HDC dc = GetDC(dialog);
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
    const HGDIOBJ previous = SelectObject(dc, font);
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previous);
    ReleaseDC(dialog, dc);
    state->title_height = metrics.tmHeight + metrics.tmExternalLeading + 2;
    const RECT top = ControlRect(dialog, IDC_TABLES_IDENTIFIER);
    int content_width = 0;
    int content_height = 0;
    static const win32ui::ListViewHooks hooks = appearance::DialogListHooks(CheckRowMenu);
    for (const records::Table& table : state->request->tables)
    {
        HWND title = win32ui::CreateControl(dialog, WC_STATICW, table.title.c_str(), SS_LEFT | SS_NOPREFIX, IDC_STATIC);
        HWND list = win32ui::CreateControl(dialog, WC_LISTVIEWW, L"", WS_TABSTOP | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SHOWSELALWAYS, 0);
        for (HWND control : {title, list})
        {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        }
        const bool checks = static_cast<int>(state->lists.size()) == state->request->check_table;
        dialog_support::SetupListView(list, &hooks, checks ? LVS_EX_CHECKBOXES : 0, {});
        SetWindowPos(list, nullptr, 0, 0, top.right - top.left, top.bottom - top.top, SWP_NOZORDER | SWP_NOACTIVATE);
        content_width = std::max(content_width, FillList(list, table));
        for (size_t row = 0; checks && row < state->request->checked.size(); ++row)
        {
            ListView_SetCheckState(list, static_cast<int>(row), state->request->checked[row]);
        }
        RECT window = {};
        RECT client = {};
        RECT header = {};
        RECT item = {};
        GetWindowRect(list, &window);
        GetClientRect(list, &client);
        GetWindowRect(ListView_GetHeader(list), &header);
        ListView_GetItemRect(list, 0, &item, LVIR_BOUNDS);
        const int chrome = (window.bottom - window.top - client.bottom) + (header.bottom - header.top);
        const int row = item.bottom - item.top;
        state->heights.push_back(chrome + static_cast<int>(table.rows.size()) * row);
        content_height += chrome + static_cast<int>(std::min(table.rows.size(), kInitialRows)) * row + state->title_height;
        state->titles.push_back(title);
        state->lists.push_back(list);
        state->sort_columns.push_back(-1);
        state->sort_ascending.push_back(true);
    }
    const RECT bottom = ControlRect(dialog, IDOK);
    RECT window = {};
    GetWindowRect(dialog, &window);
    MONITORINFO monitor = {sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    const int needed_height = content_height + Gap(dialog) * static_cast<int>(state->lists.size() + 1);
    const int extra_width = std::max(0, std::min(content_width - static_cast<int>(top.right - top.left), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left) * 9 / 10 - static_cast<int>(window.right - window.left)));
    const int extra_height = std::max(0, std::min(needed_height - static_cast<int>(bottom.top - top.bottom), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) * 85 / 100 - static_cast<int>(window.bottom - window.top)));
    // anchors are captured after WM_INITDIALOG, set opening positions at the grown size
    for (const int id : {IDC_TABLES_IDENTIFIER, IDC_TABLES_ACTION, IDOK, IDCANCEL})
    {
        RECT rect = ControlRect(dialog, id);
        if (id == IDC_TABLES_IDENTIFIER)
        {
            rect.right += extra_width;
        }
        else
        {
            OffsetRect(&rect, id == IDC_TABLES_ACTION ? 0 : extra_width, extra_height);
        }
        win32ui::Place(GetDlgItem(dialog, id), rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    }
    SetWindowPos(dialog, nullptr, 0, 0, window.right - window.left + extra_width, window.bottom - window.top + extra_height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG)
    {
        state = reinterpret_cast<State*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog, state->request->title.c_str());
        SetDlgItemTextW(dialog, IDC_TABLES_IDENTIFIER, state->request->identifier.c_str());
        SetDlgItemTextW(dialog, IDC_TABLES_ACTION, state->request->action_label.c_str());
        ShowWindow(GetDlgItem(dialog, IDC_TABLES_ACTION), state->request->action_label.empty() ? SW_HIDE : SW_SHOW);
        EnableWindow(GetDlgItem(dialog, IDC_TABLES_ACTION), static_cast<bool>(state->request->action));
        SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
        win32ui::FitDialogText(dialog);
        win32ui::AnchorDialogControls(dialog, {
                                                  {IDC_TABLES_IDENTIFIER, win32ui::kAnchorLeft | win32ui::kAnchorTop | win32ui::kAnchorRight},
                                                  {IDC_TABLES_ACTION, win32ui::kAnchorLeft | win32ui::kAnchorBottom},
                                                  {IDOK, win32ui::kAnchorRight | win32ui::kAnchorBottom},
                                                  {IDCANCEL, win32ui::kAnchorRight | win32ui::kAnchorBottom},
                                              });
        RECT client = {};
        GetClientRect(dialog, &client);
        state->layout_dpi = win32ui::DpiForWindow(dialog);
        state->right_margin_dip = win32ui::Unscale(client.right - ControlRect(dialog, IDC_TABLES_IDENTIFIER).right, state->layout_dpi);
        state->bottom_offset_dip = win32ui::Unscale(client.bottom - ControlRect(dialog, IDOK).top, state->layout_dpi);
        state->min_width_dip = win32ui::Unscale(client.right, state->layout_dpi);
        state->min_height_dip = win32ui::Unscale(client.bottom, state->layout_dpi);
        CreateLists(dialog, state);
        Layout(dialog, state);
        state->ready = true;
        if (!state->lists.empty())
        {
            SetFocus(state->lists.front());
        }
        return FALSE;
    }
    if (!state)
    {
        return FALSE;
    }
    if (message == WM_SIZE)
    {
        Layout(dialog, state);
        return FALSE;
    }
    if (message == WM_GETMINMAXINFO && state->min_width_dip > 0)
    {
        const UINT dpi = win32ui::DpiForWindow(dialog);
        RECT minimum = {0, 0, win32ui::Scale(state->min_width_dip, dpi), win32ui::Scale(state->min_height_dip, dpi)};
        win32ui::AdjustWindowRectForDpi(&minimum, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)), static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)), dpi);
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = std::max<LONG>(info->ptMinTrackSize.x, minimum.right - minimum.left);
        info->ptMinTrackSize.y = std::max<LONG>(info->ptMinTrackSize.y, minimum.bottom - minimum.top);
        return TRUE;
    }
    switch (message)
    {
    case WM_NOTIFY:
        {
            const auto* header = reinterpret_cast<NMHDR*>(lparam);
            const auto list = std::find(state->lists.begin(), state->lists.end(), header->hwndFrom);
            const auto* change = reinterpret_cast<const NMLISTVIEW*>(lparam);
            const size_t index = static_cast<size_t>(list - state->lists.begin());
            if (list != state->lists.end() && header->code == LVN_COLUMNCLICK)
            {
                bool ascending = state->sort_ascending[index];
                win32ui::SortListViewItems(*list, change->iSubItem, true, &state->sort_columns[index], &ascending, nullptr, CompareRows, const_cast<records::Table*>(&state->request->tables[index]));
                state->sort_ascending[index] = ascending;
                return TRUE;
            }
            if (list != state->lists.end() && header->code == LVN_ITEMCHANGED && state->ready && static_cast<int>(index) == state->request->check_table &&
                ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK) && change->iItem >= 0)
            {
                const bool checked = ListView_GetCheckState(*list, change->iItem) != FALSE;
                if (!state->request->on_check(dialog, static_cast<size_t>(change->lParam), checked))
                {
                    state->ready = false;
                    ListView_SetCheckState(*list, change->iItem, !checked);
                    state->ready = true;
                }
            }
            return FALSE;
        }
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDC_TABLES_ACTION:
            state->request->action(dialog);
            return TRUE;
        case IDOK:
            win32ui::CopyTextToClipboard(dialog, TablesText(*state->request));
            return TRUE;
        case IDCANCEL:
            win32ui::EndDialogWindow(dialog, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
    default:
        return FALSE;
    }
}

} // namespace

void ShowTables(HWND owner, const TablesRequest& request)
{
    State state;
    state.request = &request;
    win32ui::RunProcDialog(owner, MAKEINTRESOURCEW(IDD_TABLES), DialogProc, reinterpret_cast<LPARAM>(&state));
}

} // namespace regkit::editors
