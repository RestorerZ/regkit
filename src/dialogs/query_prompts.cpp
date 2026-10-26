// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "dialogs/query_prompts.h"

#include <win32ui/dialog/dialog_layout.h>
#include <win32ui/dialog/dialog_metrics.h>
#include <win32ui/dialog/dialog_window.h>

#include <algorithm>
#include <vector>

#include <commctrl.h>
#include <windowsx.h>

#include "browse/key_tree.h"
#include "registry/registry_path.h"
#include "registry/registry_store.h"
#include "registry/value_format.h"
#include "resource.h"
#include "ui/feedback.h"
#include "win32/translation.h"

namespace regkit::query_prompts
{

struct DataTypeItem
{
    DWORD type = 0;
    std::wstring label;
};

constexpr DWORD kExtendedTypeFlags[] = {0x20000, 0x40000};

constexpr int kDataTypesColCount = 3;
constexpr int kDataTypesColWidth = 310;

std::vector<DataTypeItem> BuildDataTypeItems()
{
    std::vector<DataTypeItem> items;
    const auto base_types = value_format::TypeLabels();
    items.reserve(base_types.size() * (1 + _countof(kExtendedTypeFlags)));
    for (const auto& entry : base_types)
    {
        items.push_back({entry.type, entry.name});
    }
    for (DWORD flag : kExtendedTypeFlags)
    {
        for (const auto& entry : base_types)
        {
            DWORD type = flag | entry.type;
            items.push_back({type, value_format::TypeName(type)});
        }
    }
    return items;
}

// registry types supply the runtime check boxes
class DataTypesDialog final : public win32ui::DialogWindow
{
  public:
    DataTypesDialog(std::vector<DataTypeItem> items, std::vector<DWORD> types)
        : items_(std::move(items)), types_(std::move(types))
    {
    }

    const std::vector<DWORD>& types() const noexcept
    {
        return types_;
    }

  protected:
    HWND initial_focus() const override
    {
        return checks_.empty() ? nullptr : checks_.front();
    }

    void OnCreate() override
    {
        // check boxes precede template buttons in tab order
        HWND previous = HWND_TOP;
        for (const auto& item : items_)
        {
            HWND check = CreateWindowExW(0, L"BUTTON", item.label.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd(), nullptr, nullptr, nullptr);
            SetWindowPos(check, previous, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            previous = check;
            const bool checked = types_.empty() || std::find(types_.begin(), types_.end(), item.type) != types_.end();
            Button_SetCheck(check, checked ? BST_CHECKED : BST_UNCHECKED);
            // set font for measurement, dialog reapplies it after OnCreate
            SendMessageW(check, WM_SETFONT, reinterpret_cast<WPARAM>(fonts().ui()), FALSE);
            checks_.push_back(check);
        }
        select_all_ = GetDlgItem(hwnd(), IDC_DATA_TYPES_SELECT_ALL);
        clear_all_ = GetDlgItem(hwnd(), IDC_DATA_TYPES_CLEAR_ALL);
        ok_button_ = GetDlgItem(hwnd(), IDOK);
        cancel_button_ = GetDlgItem(hwnd(), IDCANCEL);
        // fit columns within monitor work area, larger fonts use fewer, taller columns
        MONITORINFO monitor = {sizeof(monitor)};
        RECT window = {};
        RECT client = {};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd(), MONITOR_DEFAULTTONEAREST), &monitor) && GetWindowRect(hwnd(), &window) && GetClientRect(hwnd(), &client))
        {
            const int frame = window.right - window.left - client.right;
            const int room = monitor.rcWork.right - monitor.rcWork.left - frame;
            while (columns_ > 1 && ContentWidth(columns_) > room)
            {
                --columns_;
            }
            // template scales with font, beyond the work area use the width needed by columns
            if (client.right > room)
            {
                SetWindowPos(hwnd(), nullptr, 0, 0, ContentWidth(columns_) + frame, window.bottom - window.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        rows_per_col_ = std::max(1, (static_cast<int>(items_.size()) + columns_ - 1) / columns_);
        win32ui::FitDialogHeight(hwnd(), Px(win32ui::metrics::kDialogContentMargin) + rows_per_col_ * TextPx(win32ui::metrics::kRowPitch) + Px(win32ui::metrics::kButtonGap) + TextPx(win32ui::metrics::kButtonHeight) + Px(win32ui::metrics::kDialogButtonBottomMargin));
    }

    void OnLayout(int width, int height) override
    {
        const int padding = Px(win32ui::metrics::kDialogContentMargin);
        const int col_gap = Px(win32ui::metrics::kBlockGap);
        const int row_step = TextPx(win32ui::metrics::kRowPitch);
        const int button_h = TextPx(win32ui::metrics::kButtonHeight);
        const int button_gap = Px(win32ui::metrics::kButtonGap);
        const int button_w = Px(win32ui::metrics::kButtonMinWidth);
        const int select_all_w = std::max(Px(100), win32ui::TextFitWidth(select_all_));
        const int clear_all_w = std::max(Px(90), win32ui::TextFitWidth(clear_all_));
        const int btn_y = height - Px(win32ui::metrics::kDialogButtonBottomMargin) - button_h;
        const int right_margin = Px(win32ui::metrics::kDialogButtonRightMargin);
        win32ui::PlaceButtonRow({ok_button_, cancel_button_}, width - right_margin, btn_y, button_w, button_h, button_gap);
        if (win32ui::GrowDialogWidth(hwnd(), ContentWidth(columns_)))
        {
            return;
        }
        int x = padding;
        for (int col = 0; col < columns_; ++col)
        {
            const int col_w = ColumnWidth(col, columns_);
            for (int row = 0; row < rows_per_col_ && col * rows_per_col_ + row < static_cast<int>(checks_.size()); ++row)
            {
                win32ui::Place(checks_[static_cast<size_t>(col * rows_per_col_ + row)], x, padding + row * row_step, col_w, TextPx(win32ui::metrics::kCheckHeight));
            }
            x += col_w + col_gap;
        }
        win32ui::Place(select_all_, padding, btn_y, select_all_w, button_h);
        win32ui::Place(clear_all_, padding + select_all_w + button_gap, btn_y, clear_all_w, button_h);
    }

    bool OnCommand(int id, int, HWND) override
    {
        switch (id)
        {
        case IDC_DATA_TYPES_SELECT_ALL:
        case IDC_DATA_TYPES_CLEAR_ALL:
            for (HWND check : checks_)
            {
                Button_SetCheck(check, id == IDC_DATA_TYPES_SELECT_ALL ? BST_CHECKED : BST_UNCHECKED);
            }
            return true;
        case IDOK:
            types_.clear();
            for (size_t i = 0; i < checks_.size(); ++i)
            {
                if (Button_GetCheck(checks_[i]) == BST_CHECKED)
                {
                    types_.push_back(items_[i].type);
                }
            }
            if (types_.empty())
            {
                ui::ShowWarning(hwnd(), util::Tr(L"Select at least one data type."));
                return true;
            }
            End(IDOK);
            return true;
        default:
            return false;
        }
    }

  private:
    // widest label in each column, at least kDataTypesColWidth
    int ColumnWidth(int col, int columns) const
    {
        const int count = static_cast<int>(checks_.size());
        const int rows = std::max(1, (count + columns - 1) / columns);
        int width = Px(kDataTypesColWidth);
        for (int index = col * rows; index < std::min(count, (col + 1) * rows); ++index)
        {
            width = std::max(width, win32ui::TextFitWidth(checks_[static_cast<size_t>(index)]));
        }
        return width;
    }

    int ContentWidth(int columns) const
    {
        const int gap = Px(win32ui::metrics::kBlockGap);
        const int button_gap = Px(win32ui::metrics::kButtonGap);
        int checks_w = 0;
        for (int col = 0; col < columns; ++col)
        {
            checks_w += ColumnWidth(col, columns) + (col ? gap : 0);
        }
        const int buttons_w = std::max(Px(100), win32ui::TextFitWidth(select_all_)) + std::max(Px(90), win32ui::TextFitWidth(clear_all_)) + 2 * std::max(Px(win32ui::metrics::kButtonMinWidth), std::max(win32ui::TextFitWidth(ok_button_), win32ui::TextFitWidth(cancel_button_))) + 3 * button_gap;
        return Px(win32ui::metrics::kDialogContentMargin) + std::max(checks_w, buttons_w) + Px(win32ui::metrics::kDialogButtonRightMargin);
    }

    HWND select_all_ = nullptr;
    HWND clear_all_ = nullptr;
    HWND ok_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    std::vector<DataTypeItem> items_;
    std::vector<HWND> checks_;
    std::vector<DWORD> types_;
    int columns_ = kDataTypesColCount;
    int rows_per_col_ = 1;
};

bool ShowDataTypes(HWND owner, std::vector<DWORD>* types)
{
    if (!types)
    {
        return false;
    }
    DataTypesDialog dialog(BuildDataTypeItems(), *types);
    if (dialog.RunModal(owner, MAKEINTRESOURCEW(IDD_DATA_TYPES)) != IDOK)
    {
        return false;
    }
    *types = dialog.types();
    return true;
}

// registry tree is created at runtime
class BrowseKeyDialog final : public win32ui::DialogWindow
{
  public:
    const std::wstring& selected_path() const noexcept
    {
        return selected_path_;
    }

  protected:
    void OnCreate() override
    {
        // keep outer size at 420 by 420 DIPs regardless of frame metrics, opening size is the resize minimum
        SetWindowPos(hwnd(), nullptr, 0, 0, Px(420), Px(420), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        ok_button_ = GetDlgItem(hwnd(), IDOK);
        cancel_button_ = GetDlgItem(hwnd(), IDCANCEL);
        tree_.Create(hwnd(), GetModuleHandleW(nullptr), IDC_BROWSE_KEY_TREE);
        tree_.PopulateRoots(RegistryStore::DefaultRoots());
        // tree precedes template buttons in tab order
        SetWindowLongPtrW(tree_.hwnd(), GWL_STYLE, GetWindowLongPtrW(tree_.hwnd(), GWL_STYLE) | WS_TABSTOP);
        SetWindowPos(tree_.hwnd(), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(tree_.hwnd(), SW_SHOW);
    }

    HWND initial_focus() const override
    {
        return tree_.hwnd();
    }

    void OnLayout(int width, int height) override
    {
        const int margin = Px(win32ui::metrics::kDialogContentMargin);
        const int button_h = TextPx(win32ui::metrics::kButtonHeight);
        const int right_margin = Px(win32ui::metrics::kDialogButtonRightMargin);
        const int bottom_y = height - Px(win32ui::metrics::kDialogButtonBottomMargin) - button_h;
        win32ui::Place(tree_.hwnd(), margin, margin, width - margin * 2, std::max(0, bottom_y - Px(win32ui::metrics::kBlockGap) - margin));
        const int buttons_w = win32ui::PlaceButtonRow({ok_button_, cancel_button_}, width - right_margin, bottom_y, Px(win32ui::metrics::kButtonMinWidth), button_h, Px(win32ui::metrics::kButtonGap));
        win32ui::GrowDialogWidth(hwnd(), margin + buttons_w + right_margin);
    }

    LRESULT OnNotify(NMHDR* header, bool* handled) override
    {
        *handled = false;
        if (header->hwndFrom != tree_.hwnd())
        {
            return 0;
        }
        switch (header->code)
        {
        case TVN_ITEMEXPANDINGW:
            *handled = true;
            tree_.OnItemExpanding(reinterpret_cast<NMTREEVIEWW*>(header));
            return 0;
        case TVN_GETDISPINFOW:
            *handled = true;
            tree_.OnGetDispInfo(reinterpret_cast<NMTVDISPINFOW*>(header));
            return 0;
        case TVN_SELCHANGEDW:
            *handled = true;
            if (RegistryNode* node = tree_.OnSelectionChanged(reinterpret_cast<NMTREEVIEWW*>(header)))
            {
                selected_path_ = registry_path::Build(*node);
            }
            return 0;
        default:
            return 0;
        }
    }

    bool OnCommand(int id, int, HWND) override
    {
        if (id != IDOK)
        {
            return false;
        }
        if (selected_path_.empty())
        {
            ui::ShowWarning(hwnd(), util::Tr(L"Select a key."));
            return true;
        }
        End(IDOK);
        return true;
    }

  private:
    HWND ok_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    RegistryTree tree_;
    std::wstring selected_path_;
};

bool ShowRegistryKey(HWND owner, std::wstring* selected_path)
{
    if (!selected_path)
    {
        return false;
    }
    BrowseKeyDialog dialog;
    if (dialog.RunModal(owner, MAKEINTRESOURCEW(IDD_BROWSE_KEY)) != IDOK)
    {
        return false;
    }
    *selected_path = dialog.selected_path();
    return true;
}

} // namespace regkit::query_prompts
