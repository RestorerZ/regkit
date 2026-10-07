// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "dialogs/table_dialog.h"

#include "ui/dialog_layout.h"
#include "ui/dialog_metrics.h"
#include "ui/dialog_support.h"
#include "ui/feedback.h"
#include "ui/list_view_support.h"
#include "win32/text_transform.h"
#include "win32/window_metrics.h"

#include "resource.h"

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
    HFONT font = nullptr;
    appearance::DialogResizer resizer;
    std::vector<HWND> titles;
    std::vector<HWND> lists;
    std::vector<int> heights;
    std::vector<int> sort_columns;
    std::vector<bool> sort_ascending;
    int title_height = 0;
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

size_t RowOf(HWND list, int item)
{
    LPARAM row = 0;
    appearance::ListViewItemData(list, item, &row);
    return static_cast<size_t>(row);
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

void CopySelectedRows(HWND dialog, HWND list, const records::Table& table)
{
    std::wstring text;
    for (int item = ListView_GetNextItem(list, -1, LVNI_SELECTED); item >= 0; item = ListView_GetNextItem(list, item, LVNI_SELECTED))
    {
        text.append(Line(table.rows[RowOf(list, item)]));
    }
    if (!text.empty())
    {
        ui::CopyTextToClipboard(dialog, text);
    }
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
    return appearance::metrics::Scaled(appearance::metrics::kBlockGap, win32::DpiForWindow(dialog)) / 2;
}

void Layout(HWND dialog, State* state)
{
    const RECT top = ControlRect(dialog, IDC_TABLES_IDENTIFIER);
    const RECT bottom = ControlRect(dialog, IDOK);
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
        appearance::Place(state->titles[i], top.left, y, width, state->title_height);
        y += state->title_height;
        appearance::Place(state->lists[i], top.left, y, width, heights[i]);
        y += heights[i] + gap;
    }
    appearance::LayoutListViews(dialog);
}

int FillList(HWND list, const records::Table& table)
{
    int width = 0;
    for (size_t column = 0; column < table.columns.size(); ++column)
    {
        LVCOLUMNW item = {};
        item.mask = LVCF_TEXT | LVCF_SUBITEM;
        item.pszText = const_cast<wchar_t*>(table.columns[column].c_str());
        item.iSubItem = static_cast<int>(column);
        ListView_InsertColumn(list, static_cast<int>(column), &item);
    }
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
        width += appearance::FitListColumn(list, static_cast<int>(column));
    }
    return width + GetSystemMetrics(SM_CXVSCROLL) + 2 * GetSystemMetrics(SM_CXEDGE);
}

void CreateLists(HWND dialog, State* state)
{
    TEXTMETRICW metrics = {};
    HDC dc = GetDC(dialog);
    const HGDIOBJ previous = SelectObject(dc, state->font);
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previous);
    ReleaseDC(dialog, dc);
    state->title_height = metrics.tmHeight + metrics.tmExternalLeading + 2;
    const RECT top = ControlRect(dialog, IDC_TABLES_IDENTIFIER);
    int content_width = 0;
    int content_height = 0;
    for (const records::Table& table : state->request->tables)
    {
        HWND title = appearance::CreateControl(dialog, WC_STATICW, table.title.c_str(), SS_LEFT | SS_NOPREFIX, IDC_STATIC);
        HWND list = appearance::CreateControl(dialog, WC_LISTVIEWW, L"", WS_TABSTOP | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SHOWSELALWAYS, 0);
        for (HWND control : {title, list})
        {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), FALSE);
        }
        const bool checks = static_cast<int>(state->lists.size()) == state->request->check_table;
        dialog_support::SetupListView(list, checks ? LVS_EX_CHECKBOXES : 0, {});
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
    SetWindowPos(dialog, nullptr, 0, 0, window.right - window.left + extra_width, window.bottom - window.top + extra_height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    appearance::CenterWindow(dialog, GetWindow(dialog, GW_OWNER));
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
        dialog_support::Initialize(dialog, &state->font, {IDC_TABLES_IDENTIFIER});
        using namespace appearance;
        state->resizer.Attach(dialog, {
                                          {IDC_TABLES_IDENTIFIER, kAnchorLeft | kAnchorTop | kAnchorRight},
                                          {IDC_TABLES_ACTION, kAnchorLeft | kAnchorBottom},
                                          {IDOK, kAnchorRight | kAnchorBottom},
                                          {IDCANCEL, kAnchorRight | kAnchorBottom},
                                      });
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
        state->resizer.Apply(dialog);
        Layout(dialog, state);
        return TRUE;
    }
    INT_PTR themed = 0;
    if (dialog_support::HandleThemeMessage(dialog, message, wparam, lparam, &themed, &state->resizer))
    {
        return themed;
    }
    switch (message)
    {
    case WM_NOTIFY:
        {
            const auto* header = reinterpret_cast<NMHDR*>(lparam);
            INT_PTR result = 0;
            if (dialog_support::HandleListViewNotify(dialog, header, &result))
            {
                return result;
            }
            const auto list = std::find(state->lists.begin(), state->lists.end(), header->hwndFrom);
            const auto* change = reinterpret_cast<const NMLISTVIEW*>(lparam);
            const size_t index = static_cast<size_t>(list - state->lists.begin());
            if (list != state->lists.end() && header->code == LVN_COLUMNCLICK)
            {
                bool ascending = state->sort_ascending[index];
                appearance::SortListViewItems(*list, change->iSubItem, true, &state->sort_columns[index], &ascending, CompareRows, const_cast<records::Table*>(&state->request->tables[index]));
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
            if (list != state->lists.end() && header->code == LVN_KEYDOWN && reinterpret_cast<const NMLVKEYDOWN*>(lparam)->wVKey == 'C' && GetKeyState(VK_CONTROL) < 0)
            {
                CopySelectedRows(dialog, *list, state->request->tables[index]);
            }
            return FALSE;
        }
    case WM_COMMAND:
        if (appearance::HandleListViewCommand(dialog, LOWORD(wparam)))
        {
            return TRUE;
        }
        switch (LOWORD(wparam))
        {
        case IDC_TABLES_ACTION:
            state->request->action(dialog);
            return TRUE;
        case IDOK:
            ui::CopyTextToClipboard(dialog, TablesText(*state->request));
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
    case WM_DESTROY:
        appearance::ReleaseListViews(dialog);
        dialog_support::ReleaseFont(&state->font);
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

void ShowTables(HWND owner, const TablesRequest& request)
{
    State state;
    state.request = &request;
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_TABLES), owner, DialogProc, reinterpret_cast<LPARAM>(&state));
}

} // namespace regkit::editors
