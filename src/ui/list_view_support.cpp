// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/list_view_support.h"

#include "ui/dialog_layout.h"
#include "ui/theme.h"
#include "win32/translation.h"

#include "resource.h"

#include <win32ui/dialog/message_dialog.h>
#include <win32ui/gdi/icon_loader.h>
#include <win32ui/menu/popup_menu.h>

namespace regkit::appearance
{

namespace
{

constexpr int kMenuRowCopy = 0x7F00;
constexpr int kMenuRowSelectAll = 0x7F01;

ListGridChangedCallback grid_changed = nullptr;
void* grid_changed_context = nullptr;

} // namespace

void SetListGridChangedCallback(ListGridChangedCallback callback, void* context)
{
    grid_changed = callback;
    grid_changed_context = context;
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
    // returned length only counts text that fits, grow buffer while full
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

int SelectedItemIndex(HWND list)
{
    LPARAM data = -1;
    return win32ui::SelectedListViewData(list, &data) ? static_cast<int>(data) : -1;
}

win32ui::ListViewHooks DialogListHooks(bool (*row_menu)(void* context, HWND list, int row, POINT screen))
{
    win32ui::ListViewHooks hooks = {
        .label = [](win32ui::ListLabel label) -> const wchar_t* {
            switch (label)
            {
            case win32ui::ListLabel::fit_column:
                return util::Tr(L"Size column to fit");
            case win32ui::ListLabel::fit_all:
                return util::Tr(L"Size all columns to fit");
            case win32ui::ListLabel::reset_columns:
                return util::Tr(L"Reset columns");
            case win32ui::ListLabel::copy:
                return util::Tr(L"Copy");
            case win32ui::ListLabel::select_all:
                return util::Tr(L"Select All");
            case win32ui::ListLabel::grid_lines:
                return util::Tr(L"Grid Lines");
            case win32ui::ListLabel::more_text:
                return L"...";
            }
            return L"";
        },
        .frame = DialogThemeHooks(),
        .grid_icon = [](UINT dpi) { return win32ui::LoadIconResource(win32ui::ThemeLightGlyphs() ? IDI_ICON_LIGHT_GRID : IDI_ICON_DARK_GRID, dpi); },
        .grid_changed = [](bool shown) {
            // main window saves grid setting and updates main lists too
            if (grid_changed)
            {
                grid_changed(grid_changed_context, shown);
            }
            else
            {
                win32ui::SetAllListGridLines(shown);
            } },
        .row_menu = row_menu,
    };
    return hooks;
}

void AppendRowMenuCommands(HMENU menu, HWND list)
{
    const bool selected = ListView_GetSelectedCount(list) > 0;
    const bool can_select_all = ListView_GetItemCount(list) > 0 && !(GetWindowLongPtrW(list, GWL_STYLE) & LVS_SINGLESEL);
    win32ui::AppendMenuItem(menu, MF_STRING | (selected ? 0 : MF_GRAYED), kMenuRowCopy, util::Tr(L"&Copy"), L"Ctrl+C");
    win32ui::AppendMenuItem(menu, MF_STRING | (can_select_all ? 0 : MF_GRAYED), kMenuRowSelectAll, util::Tr(L"Select &All"), L"Ctrl+A");
}

bool RunRowMenuCommand(HWND list, int command)
{
    if (command == kMenuRowCopy)
    {
        // no visible columns means nothing to copy, leave clipboard unchanged
        if (const std::wstring text = win32ui::SelectedRowsText(list); !text.empty())
        {
            win32ui::CopyTextToClipboard(GetAncestor(list, GA_ROOT), text);
        }
        return true;
    }
    if (command == kMenuRowSelectAll)
    {
        win32ui::SelectAllListItems(list);
        return true;
    }
    return false;
}

} // namespace regkit::appearance
