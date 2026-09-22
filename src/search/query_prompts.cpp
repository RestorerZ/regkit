// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search/query_prompts.h"

#include <algorithm>
#include <vector>

#include <commctrl.h>
#include <windowsx.h>

#include "appearance/dialog_layout.h"
#include "appearance/dialog_metrics.h"
#include "appearance/feedback.h"
#include "appearance/theme.h"
#include "browse/key_tree.h"
#include "registry/registry_path.h"
#include "registry/registry_store.h"
#include "registry/value_format.h"
#include "win32/window_metrics.h"

namespace regkit::query_prompts
{

struct DataTypeItem
{
    DWORD type = 0;
    std::wstring label;
};

struct BaseTypeItem
{
    DWORD type = 0;
    const wchar_t* label = nullptr;
};

constexpr BaseTypeItem kBaseDataTypes[] = {
    {REG_SZ, L"REG_SZ"},
    {REG_EXPAND_SZ, L"REG_EXPAND_SZ"},
    {REG_MULTI_SZ, L"REG_MULTI_SZ"},
    {REG_DWORD, L"DWORD (32-bit)"},
    {REG_QWORD, L"QWORD (64-bit)"},
    {REG_BINARY, L"REG_BINARY"},
    {REG_NONE, L"REG_NONE"},
    {REG_DWORD_BIG_ENDIAN, L"REG_DWORD_BIG_ENDIAN"},
    {REG_LINK, L"REG_LINK"},
    {REG_RESOURCE_LIST, L"REG_RESOURCE_LIST"},
    {REG_FULL_RESOURCE_DESCRIPTOR, L"REG_FULL_RESOURCE_DESCRIPTOR"},
    {REG_RESOURCE_REQUIREMENTS_LIST, L"REG_RESOURCE_REQUIREMENTS_LIST"},
};

constexpr DWORD kExtendedTypeFlags[] = {0x20000, 0x40000};

constexpr int kDataTypesColCount = 3;
constexpr int kDataTypesColWidth = 310;

std::vector<DataTypeItem> BuildDataTypeItems()
{
    std::vector<DataTypeItem> items;
    items.reserve(_countof(kBaseDataTypes) * (1 + _countof(kExtendedTypeFlags)));
    for (const auto& entry : kBaseDataTypes)
    {
        items.push_back({entry.type, entry.label});
    }
    for (DWORD flag : kExtendedTypeFlags)
    {
        for (const auto& entry : kBaseDataTypes)
        {
            DWORD type = flag | entry.type;
            items.push_back({type, value_format::TypeName(type)});
        }
    }
    return items;
}

struct DataTypesDialogState : appearance::DialogWindow
{
    HWND select_all = nullptr;
    HWND clear_all = nullptr;
    HWND ok_button = nullptr;
    HWND cancel_button = nullptr;
    std::vector<DataTypeItem> items;
    std::vector<HWND> checks;
    std::vector<DWORD> types;
    int rows_per_col = 1;
};

void LayoutDataTypesDialog(HWND hwnd, DataTypesDialogState* state)
{
    RECT client = {};
    GetClientRect(hwnd, &client);
    using namespace appearance::metrics;
    const UINT dpi = win32::DpiForWindow(hwnd);
    const int padding = Scaled(kDialogContentMargin, dpi);
    const int col_w = Scaled(kDataTypesColWidth, dpi);
    const int col_gap = Scaled(kBlockGap, dpi);
    const int row_step = Scaled(kRowPitch, dpi);
    const int button_h = Scaled(kButtonHeight, dpi);
    const int button_gap = Scaled(kButtonGap, dpi);
    const int button_w = Scaled(kButtonMinWidth, dpi);
    const int select_all_w = Scaled(100, dpi);
    const int btn_y = client.bottom - Scaled(kDialogButtonBottomMargin, dpi) - button_h;
    const int cancel_x = client.right - Scaled(kDialogButtonRightMargin, dpi) - button_w;
    for (size_t index = 0; index < state->checks.size(); ++index)
    {
        const int col = static_cast<int>(index) / state->rows_per_col;
        const int row = static_cast<int>(index) % state->rows_per_col;
        appearance::Place(state->checks[index], padding + col * (col_w + col_gap), padding + row * row_step, col_w, Scaled(kCheckHeight, dpi));
    }
    appearance::Place(state->select_all, padding, btn_y, select_all_w, button_h);
    appearance::Place(state->clear_all, padding + select_all_w + button_gap, btn_y, Scaled(90, dpi), button_h);
    appearance::Place(state->ok_button, cancel_x - button_gap - button_w, btn_y, button_w, button_h);
    appearance::Place(state->cancel_button, cancel_x, btn_y, button_w, button_h);
}

LRESULT CALLBACK DataTypesDialogProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    auto* state = appearance::DialogWindowState<DataTypesDialogState>(hwnd);
    switch (msg)
    {
    case WM_CREATE:
        for (const auto& item : state->items)
        {
            HWND check =
                CreateWindowExW(0, L"BUTTON", item.label.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
            const bool checked = state->types.empty() ||
                                 std::find(state->types.begin(), state->types.end(), item.type) != state->types.end();
            Button_SetCheck(check, checked ? BST_CHECKED : BST_UNCHECKED);
            state->checks.push_back(check);
        }
        state->select_all =
            CreateWindowExW(0, L"BUTTON", L"Select All", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(100), nullptr, nullptr);
        state->clear_all =
            CreateWindowExW(0, L"BUTTON", L"Clear All", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(101), nullptr, nullptr);
        state->ok_button = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        state->cancel_button =
            CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        appearance::SetDialogFont(hwnd, state->font);
        LayoutDataTypesDialog(hwnd, state);
        return 0;
    case WM_SIZE:
        LayoutDataTypesDialog(hwnd, state);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case 100:
        case 101:
            for (HWND check : state->checks)
            {
                Button_SetCheck(check, LOWORD(wparam) == 100 ? BST_CHECKED : BST_UNCHECKED);
            }
            return 0;
        case IDOK:
            state->types.clear();
            for (size_t i = 0; i < state->checks.size(); ++i)
            {
                if (Button_GetCheck(state->checks[i]) == BST_CHECKED)
                {
                    state->types.push_back(state->items[i].type);
                }
            }
            if (state->types.empty())
            {
                ui::ShowWarning(hwnd, L"Select at least one data type.");
                return 0;
            }
            appearance::CloseDialogWindow(state, true);
            return 0;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return appearance::DefDialogWindowProc(hwnd, msg, wparam, lparam);
}

bool ShowDataTypes(HWND owner, std::vector<DWORD>* types)
{
    if (!types)
    {
        return false;
    }
    using namespace appearance::metrics;
    DataTypesDialogState state;
    state.owner = owner;
    state.items = BuildDataTypeItems();
    state.types = *types;
    state.rows_per_col =
        std::max(1, (static_cast<int>(state.items.size()) + kDataTypesColCount - 1) / kDataTypesColCount);
    const UINT dpi = win32::DpiForWindow(owner);
    const int content_w = kDataTypesColCount * kDataTypesColWidth + (kDataTypesColCount - 1) * kBlockGap;
    const int client_w = Scaled(kDialogContentMargin + content_w + kDialogButtonRightMargin, dpi);
    const int client_h = Scaled(kDialogContentMargin + state.rows_per_col * kRowPitch + kButtonGap + kButtonHeight + kDialogButtonBottomMargin, dpi);
    if (!appearance::RunDialogWindow(&state, L"RegKitDataTypesDialog", DataTypesDialogProc, L"Data Types", appearance::DialogWindowSize(owner, client_w, client_h)))
    {
        return false;
    }
    *types = state.types;
    return true;
}

struct BrowseDialogState : appearance::DialogWindow
{
    HWND ok_button = nullptr;
    HWND cancel_button = nullptr;
    RegistryTree tree;
    std::wstring selected_path;
};

LRESULT CALLBACK BrowseDialogProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    auto* state = appearance::DialogWindowState<BrowseDialogState>(hwnd);
    switch (msg)
    {
    case WM_CREATE:
        state->ok_button = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        state->cancel_button =
            CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        state->tree.Create(hwnd, GetModuleHandleW(nullptr), 1);
        state->tree.PopulateRoots(RegistryStore::DefaultRoots());
        state->focus = state->tree.hwnd();
        appearance::SetDialogFont(hwnd, state->font);
        return 0;
    case WM_SIZE:
        {
            using namespace appearance::metrics;
            const UINT dpi = win32::DpiForWindow(hwnd);
            const int margin = Scaled(kDialogContentMargin, dpi);
            const int button_h = Scaled(kButtonHeight, dpi);
            const int button_w = Scaled(kButtonMinWidth, dpi);
            const int width = LOWORD(lparam);
            const int bottom_y = HIWORD(lparam) - Scaled(kDialogButtonBottomMargin, dpi) - button_h;
            const int cancel_x = width - Scaled(kDialogButtonRightMargin, dpi) - button_w;
            appearance::Place(state->tree.hwnd(), margin, margin, width - margin * 2, std::max(0, bottom_y - Scaled(kBlockGap, dpi) - margin));
            appearance::Place(state->ok_button, cancel_x - Scaled(kButtonGap, dpi) - button_w, bottom_y, button_w, button_h);
            appearance::Place(state->cancel_button, cancel_x, bottom_y, button_w, button_h);
            return 0;
        }
    case WM_NOTIFY:
        {
            auto* hdr = reinterpret_cast<NMHDR*>(lparam);
            if (hdr->hwndFrom != state->tree.hwnd())
            {
                break;
            }
            switch (hdr->code)
            {
            case TVN_ITEMEXPANDINGW:
                state->tree.OnItemExpanding(reinterpret_cast<NMTREEVIEWW*>(lparam));
                return 0;
            case TVN_GETDISPINFOW:
                state->tree.OnGetDispInfo(reinterpret_cast<NMTVDISPINFOW*>(lparam));
                return 0;
            case TVN_SELCHANGEDW:
                if (RegistryNode* node = state->tree.OnSelectionChanged(reinterpret_cast<NMTREEVIEWW*>(lparam)))
                {
                    state->selected_path = registry_path::Build(*node);
                }
                return 0;
            case NM_CUSTOMDRAW:
                {
                    auto* draw = reinterpret_cast<NMTVCUSTOMDRAW*>(lparam);
                    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                    {
                        return CDRF_NOTIFYITEMDRAW;
                    }
                    if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && !(draw->nmcd.uItemState & CDIS_SELECTED))
                    {
                        draw->clrText = Theme::Current().TextColor();
                        draw->clrTextBk = Theme::Current().PanelColor();
                        return CDRF_NEWFONT;
                    }
                    return CDRF_DODEFAULT;
                }
            default:
                break;
            }
            break;
        }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK)
        {
            if (state->selected_path.empty())
            {
                ui::ShowWarning(hwnd, L"Select a key.");
                return 0;
            }
            appearance::CloseDialogWindow(state, true);
            return 0;
        }
        break;
    default:
        break;
    }
    return appearance::DefDialogWindowProc(hwnd, msg, wparam, lparam);
}

bool ShowRegistryKey(HWND owner, std::wstring* selected_path)
{
    BrowseDialogState state;
    state.owner = owner;
    const UINT dpi = win32::DpiForWindow(owner);
    if (!selected_path ||
        !appearance::RunDialogWindow(&state, L"RegKitBrowseKeyDialog", BrowseDialogProc, L"Browse Key", {appearance::metrics::Scaled(420, dpi), appearance::metrics::Scaled(420, dpi)}, WS_SIZEBOX))
    {
        return false;
    }
    *selected_path = state.selected_path;
    return true;
}

} // namespace regkit::query_prompts
