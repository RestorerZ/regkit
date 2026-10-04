// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "appearance/preset_editor.h"

#include <algorithm>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <windowsx.h>

#include "appearance/default_font.h"
#include "appearance/dialog_fit.h"
#include "appearance/dialog_layout.h"
#include "appearance/dialog_metrics.h"
#include "appearance/feedback.h"
#include "appearance/list_view_support.h"
#include "win32/file_dialog.h"
#include "win32/text_transform.h"
#include "win32/translation.h"
#include "win32/window_metrics.h"

namespace regkit
{

namespace
{

constexpr wchar_t kThemePresetClass[] = L"RegKitThemePresetsWindow";

constexpr int kWindowWidth = 580;
constexpr int kWindowHeight = 360;
constexpr int kPadding = appearance::metrics::kDialogContentMargin;
constexpr int kGap = 8;
constexpr int kButtonGap = appearance::metrics::kButtonGap;
constexpr int kButtonHeight = appearance::metrics::kButtonHeight;
constexpr int kButtonWidth = appearance::metrics::kButtonMinWidth;
constexpr int kWideButtonWidth = 90;
constexpr int kLeftPanelWidth = 210;
constexpr int kGroupBoxCaptionHeight = 18;
constexpr int kGroupBoxPadding = 10;
constexpr int kEditColorButtonWidth = 120;
constexpr int kTemplateButtonWidth = 130;
constexpr UINT_PTR kThemePresetListViewSubclassId = 2;

enum ControlId
{
    kPresetListId = 5001,
    kColorListId = 5002,
    kNewPresetId = 5003,
    kDuplicatePresetId = 5004,
    kRenamePresetId = 5005,
    kDeletePresetId = 5006,
    kImportPresetId = 5007,
    kExportPresetId = 5008,
    kEditColorId = 5009,
    kDarkCheckId = 5010,
    kTemplateComboId = 5011,
    kApplyTemplateId = 5012,
    kApplyId = 5013,
    kColorGridId = 5014,
};

struct ColorField
{
    const wchar_t* label = nullptr;
    COLORREF ThemeColors::* member = nullptr;
};

constexpr ColorField kColorFields[] = {
    {util::TrNoop(L"Background"), &ThemeColors::background},
    {util::TrNoop(L"Panel"), &ThemeColors::panel},
    {util::TrNoop(L"Surface"), &ThemeColors::surface},
    {util::TrNoop(L"Field"), &ThemeColors::field},
    {util::TrNoop(L"Header"), &ThemeColors::header},
    {util::TrNoop(L"Border"), &ThemeColors::border},
    {util::TrNoop(L"Text"), &ThemeColors::text},
    {util::TrNoop(L"Muted Text"), &ThemeColors::muted_text},
    {util::TrNoop(L"Accent"), &ThemeColors::accent},
    {util::TrNoop(L"Selection"), &ThemeColors::selection},
    {util::TrNoop(L"Selection Text"), &ThemeColors::selection_text},
    {util::TrNoop(L"Hover"), &ThemeColors::hover},
    {util::TrNoop(L"Focus"), &ThemeColors::focus},
};

struct ThemePresetWindowState : appearance::DialogWindow
{
    HWND presets_group = nullptr;
    HWND preset_list = nullptr;
    HWND colors_group = nullptr;
    HWND color_list = nullptr;
    HWND templates_group = nullptr;
    HWND new_btn = nullptr;
    HWND duplicate_btn = nullptr;
    HWND rename_btn = nullptr;
    HWND delete_btn = nullptr;
    HWND import_btn = nullptr;
    HWND export_btn = nullptr;
    HWND edit_color_btn = nullptr;
    HWND dark_check = nullptr;
    HWND template_combo = nullptr;
    HWND template_btn = nullptr;
    HWND apply_btn = nullptr;
    HWND ok_btn = nullptr;
    HWND cancel_btn = nullptr;
    appearance::ThemePresetApply apply = nullptr;
    appearance::ThemePresetNamePrompt prompt_name = nullptr;
    void* apply_context = nullptr;
    std::vector<ThemePreset> presets;
    std::vector<ThemePreset> templates;
    std::wstring active_name;
    int selected_index = -1;
    int color_sort_column = -1;
    bool color_sort_ascending = true;
    COLORREF custom_colors[16] = {};
};

ThemePreset* CurrentPreset(ThemePresetWindowState* state)
{
    if (!state)
    {
        return nullptr;
    }
    if (state->selected_index < 0 || static_cast<size_t>(state->selected_index) >= state->presets.size())
    {
        return nullptr;
    }
    return &state->presets[static_cast<size_t>(state->selected_index)];
}

int FindPresetIndexByName(const std::vector<ThemePreset>& presets, const std::wstring& name)
{
    for (size_t i = 0; i < presets.size(); ++i)
    {
        if (util::EqualsInsensitive(presets[i].name, name))
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::wstring MakeUniquePresetName(const std::vector<ThemePreset>& presets, const std::wstring& base_name, const ThemePreset* ignored = nullptr)
{
    std::wstring base = base_name.empty() ? L"Preset" : base_name;
    auto exists = [&](const std::wstring& name) -> bool {
        return std::any_of(presets.begin(), presets.end(), [&](const ThemePreset& preset) {
            return &preset != ignored && util::EqualsInsensitive(preset.name, name);
        });
    };
    if (!exists(base))
    {
        return base;
    }
    for (int i = 2; i < 1000; ++i)
    {
        std::wstring candidate = base + L" " + std::to_wstring(i);
        if (!exists(candidate))
        {
            return candidate;
        }
    }
    return base + L" Copy";
}

bool PromptPresetName(ThemePresetWindowState* state, HWND owner, const wchar_t* title, const std::wstring& initial, std::wstring* out_name)
{
    if (!state || !state->prompt_name || !out_name)
    {
        return false;
    }
    std::wstring name;
    if (!state->prompt_name(state->apply_context, owner, title, initial, &name))
    {
        return false;
    }
    if (name.empty())
    {
        ui::ShowError(owner, util::Tr(L"Preset name can't be empty."));
        return false;
    }
    *out_name = name;
    return true;
}

constexpr wchar_t kThemeFilter[] = L"RegKit Theme Presets (*.rktheme)\0*.rktheme\0All Files (*.*)\0*.*\0";

bool ChooseColorFor(HWND owner, COLORREF* color, COLORREF* custom_colors)
{
    if (!color)
    {
        return false;
    }
    CHOOSECOLORW cc = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = owner;
    cc.rgbResult = *color;
    cc.lpCustColors = custom_colors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc))
    {
        return false;
    }
    *color = cc.rgbResult;
    return true;
}

int CompareColorValue(COLORREF left, COLORREF right)
{
    if (left < right)
    {
        return -1;
    }
    if (left > right)
    {
        return 1;
    }
    return 0;
}

LRESULT CALLBACK ThemePresetListViewProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
    {
        SendMessageW(hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    }
    if (message == WM_UPDATEUISTATE)
    {
        LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
        SendMessageW(hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
        return result;
    }
    if (message == WM_THEMECHANGED)
    {
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void SetupPresetListView(HWND list)
{
    if (!list)
    {
        return;
    }
    appearance::ConfigureListView(list);
    LVCOLUMNW col = {};
    col.mask = LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_LEFT;
    col.cx = 120;
    ListView_InsertColumn(list, 0, &col);
    EnsureSubclass(list, ThemePresetListViewProc, kThemePresetListViewSubclassId);
    Theme::Current().ApplyToListView(list);
}

void SetupColorListView(HWND list)
{
    if (!list)
    {
        return;
    }
    appearance::ConfigureListView(list);
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.cx = 150;
    col.pszText = const_cast<wchar_t*>(util::Tr(L"Color"));
    col.iSubItem = 0;
    ListView_InsertColumn(list, 0, &col);
    col.cx = 90;
    col.pszText = const_cast<wchar_t*>(util::Tr(L"Hex"));
    col.iSubItem = 1;
    ListView_InsertColumn(list, 1, &col);
    EnsureSubclass(list, ThemePresetListViewProc, kThemePresetListViewSubclassId);
    Theme::Current().ApplyToListView(list);
    appearance::RegisterListView(GetParent(list), list, kColorGridId);
}

int CALLBACK CompareColorListItems(LPARAM left_param, LPARAM right_param, int column, void* context)
{
    auto* preset = static_cast<ThemePreset*>(context);
    if (!preset)
    {
        return 0;
    }
    int left_index = static_cast<int>(left_param);
    int right_index = static_cast<int>(right_param);
    if (left_index < 0 || left_index >= static_cast<int>(std::size(kColorFields)) || right_index < 0 ||
        right_index >= static_cast<int>(std::size(kColorFields)))
    {
        return 0;
    }
    int result = 0;
    if (column == 0)
    {
        result = util::CompareListText(util::Tr(kColorFields[left_index].label), util::Tr(kColorFields[right_index].label));
    }
    else if (column == 1)
    {
        COLORREF left = preset->colors.*(kColorFields[left_index].member);
        COLORREF right = preset->colors.*(kColorFields[right_index].member);
        result = CompareColorValue(left, right);
    }
    return result;
}

void ReselectColorField(HWND list, int field_index)
{
    if (!list || field_index < 0)
    {
        return;
    }
    LVFINDINFOW find = {};
    find.flags = LVFI_PARAM;
    find.lParam = static_cast<LPARAM>(field_index);
    int row = ListView_FindItem(list, -1, &find);
    if (row < 0)
    {
        return;
    }
    ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, row, FALSE);
}

void FillColorList(ThemePresetWindowState* state, const ThemePreset* preset)
{
    if (!state || !state->color_list)
    {
        return;
    }
    HWND list = state->color_list;
    int selected_field = appearance::SelectedListViewData(list);
    ListView_DeleteAllItems(list);
    if (!preset)
    {
        appearance::UpdateListViewSort(list, state->color_sort_column, state->color_sort_ascending);
        return;
    }
    for (size_t i = 0; i < std::size(kColorFields); ++i)
    {
        const auto& field = kColorFields[i];
        COLORREF color = preset->colors.*(field.member);
        std::wstring hex = FormatColorHex(color);
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(util::Tr(field.label));
        item.lParam = static_cast<LPARAM>(i);
        int index = ListView_InsertItem(list, &item);
        if (index >= 0)
        {
            ListView_SetItemText(list, index, 1, const_cast<wchar_t*>(hex.c_str()));
        }
    }
    if (state->color_sort_column >= 0)
    {
        appearance::SortListViewItems(list, state->color_sort_column, false, &state->color_sort_column, &state->color_sort_ascending, CompareColorListItems, const_cast<ThemePreset*>(preset));
    }
    if (selected_field >= 0)
    {
        ReselectColorField(list, selected_field);
    }
}

int RefreshPresetList(HWND list, const std::vector<ThemePreset>& presets, int selected_index)
{
    if (!list)
    {
        return -1;
    }
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < presets.size(); ++i)
    {
        const auto& preset = presets[i];
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(preset.name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        ListView_InsertItem(list, &item);
    }
    int index = selected_index;
    if (index < 0 || index >= static_cast<int>(presets.size()))
    {
        index = presets.empty() ? -1 : 0;
    }
    if (index >= 0)
    {
        ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, index, FALSE);
    }
    ListView_SetColumnWidth(list, 0, LVSCW_AUTOSIZE_USEHEADER);
    return index;
}

void SyncSelection(ThemePresetWindowState* state)
{
    if (!state)
    {
        return;
    }
    int preset_index = state->preset_list ? appearance::SelectedListViewData(state->preset_list) : -1;
    if (preset_index >= 0 && preset_index < static_cast<int>(state->presets.size()))
    {
        state->selected_index = preset_index;
    }
    ThemePreset* preset = CurrentPreset(state);
    FillColorList(state, preset);
    if (state->dark_check)
    {
        SendMessageW(state->dark_check, BM_SETCHECK, (preset && preset->is_dark) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

ThemePreset BuildPresetFromTemplate(const ThemePresetWindowState* state, int template_index)
{
    ThemePreset preset;
    if (!state || state->templates.empty())
    {
        return preset;
    }
    if (template_index < 0 || template_index >= static_cast<int>(state->templates.size()))
    {
        return state->templates.front();
    }
    return state->templates[static_cast<size_t>(template_index)];
}

void RefreshThemeRendering(ThemePresetWindowState* state)
{
    if (!state)
    {
        return;
    }
    if (state->preset_list)
    {
        Theme::Current().ApplyToListView(state->preset_list);
        state->selected_index = RefreshPresetList(state->preset_list, state->presets, state->selected_index);
        RedrawWindow(state->preset_list, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    }
    if (state->color_list)
    {
        appearance::RefreshListView(state->color_list);
        RedrawWindow(state->color_list, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    }
    RedrawWindow(state->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void LayoutControls(ThemePresetWindowState* state)
{
    if (!state || !state->hwnd)
    {
        return;
    }
    using appearance::metrics::Scaled;
    RECT rc = {};
    GetClientRect(state->hwnd, &rc);
    const UINT dpi = win32::DpiForWindow(state->hwnd);
    const int padding = Scaled(kPadding, dpi);
    const int gap = Scaled(kGap, dpi);
    const int button_gap = Scaled(kButtonGap, dpi);
    const int button_h = Scaled(kButtonHeight, dpi);
    const int button_w = Scaled(kButtonWidth, dpi);
    const int wide_button_w = Scaled(kWideButtonWidth, dpi);
    const int caption_h = Scaled(kGroupBoxCaptionHeight, dpi);
    const int box_padding = Scaled(kGroupBoxPadding, dpi);
    const int right_margin = Scaled(appearance::metrics::kDialogButtonRightMargin, dpi);
    const int bottom_margin = Scaled(appearance::metrics::kDialogButtonBottomMargin, dpi);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    const int content_top = padding;
    const int content_h = std::max(Scaled(100, dpi), height - content_top - padding - button_h - gap);
    const int button_rows_h = button_h * 3 + gap * 2;

    auto fit = [](HWND button, int minimum) { return std::max(minimum, appearance::TextFitWidth(button)); };
    const int new_w = fit(state->new_btn, button_w);
    const int duplicate_w = fit(state->duplicate_btn, wide_button_w);
    const int rename_w = fit(state->rename_btn, wide_button_w);
    const int delete_w = fit(state->delete_btn, button_w);
    const int import_w = fit(state->import_btn, wide_button_w);
    const int export_w = fit(state->export_btn, wide_button_w);
    const int left_x = padding;
    const int left_w = std::max(Scaled(kLeftPanelWidth, dpi), std::max({new_w + duplicate_w, rename_w + delete_w, import_w + export_w}) + button_gap + box_padding * 2);
    const int right_x = left_x + left_w + gap;
    const int edit_btn_w = fit(state->edit_color_btn, Scaled(kEditColorButtonWidth, dpi));
    const int template_btn_w = fit(state->template_btn, Scaled(kTemplateButtonWidth, dpi));
    const int right_min = std::max({Scaled(180, dpi), edit_btn_w + gap + appearance::TextFitWidth(state->dark_check), Scaled(120, dpi) + gap + template_btn_w}) + box_padding * 2;
    const int right_w = width - right_x - padding;

    int template_group_h = std::max(Scaled(60, dpi), caption_h + box_padding * 2 + button_h);
    int colors_group_h = content_h - template_group_h - gap;
    if (colors_group_h < Scaled(100, dpi))
    {
        colors_group_h = Scaled(100, dpi);
        template_group_h = std::max(Scaled(60, dpi), content_h - colors_group_h - gap);
    }

    const int left_inner_x = left_x + box_padding;
    const int left_inner_w = left_w - box_padding * 2;
    const int left_inner_h = content_h - caption_h - box_padding;
    const int list_y = content_top + caption_h;
    const int list_h = std::max(Scaled(80, dpi), left_inner_h - button_rows_h - gap);
    const int row1_y = list_y + list_h + gap;
    const int row2_y = row1_y + button_h + gap;
    const int row3_y = row2_y + button_h + gap;

    appearance::Place(state->presets_group, left_x, content_top, left_w, content_h);
    appearance::Place(state->preset_list, left_inner_x, list_y, left_inner_w, list_h);
    if (state->preset_list)
    {
        ListView_SetColumnWidth(state->preset_list, 0, LVSCW_AUTOSIZE_USEHEADER);
    }
    appearance::Place(state->new_btn, left_inner_x, row1_y, new_w, button_h);
    appearance::Place(state->duplicate_btn, left_inner_x + new_w + button_gap, row1_y, duplicate_w, button_h);
    appearance::Place(state->rename_btn, left_inner_x, row2_y, rename_w, button_h);
    appearance::Place(state->delete_btn, left_inner_x + rename_w + button_gap, row2_y, delete_w, button_h);
    appearance::Place(state->import_btn, left_inner_x, row3_y, import_w, button_h);
    appearance::Place(state->export_btn, left_inner_x + import_w + button_gap, row3_y, export_w, button_h);

    appearance::Place(state->colors_group, right_x, content_top, right_w, colors_group_h);
    const int colors_inner_x = right_x + box_padding;
    const int colors_inner_w = right_w - box_padding * 2;
    const int colors_inner_h = colors_group_h - caption_h - box_padding;
    const int color_list_y = content_top + caption_h;
    const int color_list_h = std::max(Scaled(80, dpi), colors_inner_h - button_h - gap);
    appearance::Place(state->color_list, colors_inner_x, color_list_y, colors_inner_w, color_list_h);

    const int edit_row_y = color_list_y + color_list_h + gap;
    appearance::Place(state->edit_color_btn, colors_inner_x, edit_row_y, edit_btn_w, button_h);
    appearance::Place(state->dark_check, colors_inner_x + edit_btn_w + gap, edit_row_y, colors_inner_w - edit_btn_w - gap, button_h);

    const int templates_group_y = content_top + colors_group_h + gap;
    appearance::Place(state->templates_group, right_x, templates_group_y, right_w, template_group_h);
    const int templates_inner_x = right_x + box_padding;
    const int templates_inner_w = right_w - box_padding * 2;
    const int template_row_y = templates_group_y + caption_h;
    const int combo_w = std::max(Scaled(120, dpi), templates_inner_w - template_btn_w - gap);
    appearance::Place(state->template_combo, templates_inner_x, template_row_y, combo_w, button_h);
    appearance::Place(state->template_btn, templates_inner_x + combo_w + gap, template_row_y, template_btn_w, button_h);

    const int bottom_y = height - bottom_margin - button_h;
    const int buttons_w = appearance::PlaceButtonRow({state->apply_btn, state->ok_btn, state->cancel_btn}, width - right_margin, bottom_y, button_w, button_h, button_gap);
    appearance::GrowDialogWidth(state->hwnd, std::max(right_x + right_min + padding, padding + buttons_w + right_margin));
}

void CreateControls(ThemePresetWindowState* state)
{
    if (!state || !state->hwnd)
    {
        return;
    }
    HWND hwnd = state->hwnd;

    state->presets_group =
        CreateWindowExW(0, L"BUTTON", util::Tr(L"Presets"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

    state->preset_list = appearance::CreateControl(hwnd, WC_LISTVIEWW, L"", WS_TABSTOP | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | LVS_NOSORTHEADER, kPresetListId);

    state->new_btn =
        appearance::CreateControl(hwnd, L"BUTTON", util::Tr(L"New..."), WS_TABSTOP | BS_PUSHBUTTON, kNewPresetId);
    state->duplicate_btn =
        appearance::CreateControl(hwnd, L"BUTTON", util::Tr(L"Duplicate"), WS_TABSTOP | BS_PUSHBUTTON, kDuplicatePresetId);
    state->rename_btn =
        appearance::CreateControl(hwnd, L"BUTTON", util::Tr(L"Rename..."), WS_TABSTOP | BS_PUSHBUTTON, kRenamePresetId);
    state->delete_btn =
        appearance::CreateControl(hwnd, L"BUTTON", util::Tr(L"Delete"), WS_TABSTOP | BS_PUSHBUTTON, kDeletePresetId);
    state->import_btn =
        appearance::CreateControl(hwnd, L"BUTTON", util::Tr(L"Import..."), WS_TABSTOP | BS_PUSHBUTTON, kImportPresetId);
    state->export_btn =
        appearance::CreateControl(hwnd, L"BUTTON", util::Tr(L"Export..."), WS_TABSTOP | BS_PUSHBUTTON, kExportPresetId);

    state->colors_group =
        CreateWindowExW(0, L"BUTTON", util::Tr(L"Colors"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

    state->color_list = appearance::CreateControl(hwnd, WC_LISTVIEWW, L"", WS_TABSTOP | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, kColorListId);

    state->edit_color_btn =
        appearance::CreateControl(hwnd, L"BUTTON", util::Tr(L"Edit Color..."), WS_TABSTOP | BS_PUSHBUTTON, kEditColorId);

    state->dark_check =
        appearance::CreateControl(hwnd, L"BUTTON", util::Tr(L"Treat as dark theme"), WS_TABSTOP | BS_AUTOCHECKBOX, kDarkCheckId);

    state->templates_group =
        CreateWindowExW(0, L"BUTTON", util::Tr(L"Templates"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

    state->template_combo =
        appearance::CreateControl(hwnd, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kTemplateComboId);

    state->template_btn =
        appearance::CreateControl(hwnd, L"BUTTON", util::Tr(L"Apply Template"), WS_TABSTOP | BS_PUSHBUTTON, kApplyTemplateId);

    state->apply_btn =
        appearance::CreateControl(hwnd, L"BUTTON", util::Tr(L"Apply"), WS_TABSTOP | BS_PUSHBUTTON, kApplyId);
    state->ok_btn = appearance::CreateControl(hwnd, L"BUTTON", util::Tr(L"OK"), WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    state->cancel_btn = appearance::CreateControl(hwnd, L"BUTTON", util::Tr(L"Cancel"), WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);

    for (HWND group : {state->presets_group, state->colors_group, state->templates_group})
    {
        SetWindowPos(group, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    SetupPresetListView(state->preset_list);
    SetupColorListView(state->color_list);
}

void PopulateTemplates(ThemePresetWindowState* state)
{
    if (!state || !state->template_combo)
    {
        return;
    }
    SendMessageW(state->template_combo, CB_RESETCONTENT, 0, 0);
    for (const auto& preset : state->templates)
    {
        SendMessageW(state->template_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset.name.c_str()));
    }
    SendMessageW(state->template_combo, CB_SETCURSEL, 0, 0);
}

void PopulatePresets(ThemePresetWindowState* state)
{
    if (!state || !state->preset_list)
    {
        return;
    }
    int active_index = FindPresetIndexByName(state->presets, state->active_name);
    state->selected_index = RefreshPresetList(state->preset_list, state->presets, active_index);
    SyncSelection(state);
}

void ApplySelectedPreset(ThemePresetWindowState* state, bool close_dialog)
{
    if (!state || !state->apply)
    {
        return;
    }
    SyncSelection(state);
    ThemePreset* preset = CurrentPreset(state);
    if (!preset)
    {
        return;
    }
    state->active_name = preset->name;
    state->apply(state->apply_context, state->presets, state->active_name);
    appearance::ApplyDialogTheme(state->hwnd);
    RefreshThemeRendering(state);
    if (close_dialog)
    {
        appearance::CloseDialogWindow(state, true);
    }
}

LRESULT CALLBACK ThemePresetWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    auto* state = appearance::DialogWindowState<ThemePresetWindowState>(hwnd);
    switch (msg)
    {
    case WM_CREATE:
        CreateControls(state);
        appearance::SetDialogFont(hwnd, state->font);
        PopulateTemplates(state);
        PopulatePresets(state);
        LayoutControls(state);
        return 0;
    case WM_SIZE:
        LayoutControls(state);
        appearance::LayoutListViews(hwnd);
        return 0;
    case WM_SETTINGCHANGE:
        if (Theme::UpdateFromSystem())
        {
            appearance::ApplyDialogTheme(hwnd);
            RefreshThemeRendering(state);
        }
        return 0;
    case WM_COMMAND:
        {
            int id = LOWORD(wparam);
            if (appearance::HandleListViewCommand(hwnd, id))
            {
                return 0;
            }
            switch (id)
            {
            case kNewPresetId:
                {
                    std::wstring name;
                    if (!PromptPresetName(state, hwnd, util::Tr(L"New Preset"), L"", &name))
                    {
                        return 0;
                    }
                    int sel =
                        state->template_combo ? static_cast<int>(SendMessageW(state->template_combo, CB_GETCURSEL, 0, 0)) : -1;
                    ThemePreset preset = BuildPresetFromTemplate(state, sel);
                    preset.name = MakeUniquePresetName(state->presets, name);
                    state->presets.push_back(preset);
                    state->selected_index = static_cast<int>(state->presets.size() - 1);
                    state->selected_index = RefreshPresetList(state->preset_list, state->presets, state->selected_index);
                    SyncSelection(state);
                    return 0;
                }
            case kDuplicatePresetId:
                {
                    ThemePreset* preset = CurrentPreset(state);
                    if (!preset)
                    {
                        return 0;
                    }
                    ThemePreset copy = *preset;
                    copy.name = MakeUniquePresetName(state->presets, preset->name + L" Copy");
                    state->presets.push_back(copy);
                    state->selected_index = static_cast<int>(state->presets.size() - 1);
                    state->selected_index = RefreshPresetList(state->preset_list, state->presets, state->selected_index);
                    SyncSelection(state);
                    return 0;
                }
            case kRenamePresetId:
                {
                    ThemePreset* preset = CurrentPreset(state);
                    if (!preset)
                    {
                        return 0;
                    }
                    std::wstring name;
                    if (!PromptPresetName(state, hwnd, util::Tr(L"Rename Preset"), preset->name, &name))
                    {
                        return 0;
                    }
                    preset->name = MakeUniquePresetName(state->presets, name, preset);
                    state->selected_index = RefreshPresetList(state->preset_list, state->presets, state->selected_index);
                    SyncSelection(state);
                    return 0;
                }
            case kDeletePresetId:
                {
                    if (state->presets.size() <= 1)
                    {
                        ui::ShowWarning(hwnd, util::Tr(L"At least one preset must remain."));
                        return 0;
                    }
                    ThemePreset* preset = CurrentPreset(state);
                    if (!preset)
                    {
                        return 0;
                    }
                    if (!ui::ConfirmDelete(hwnd, util::Tr(L"Delete Preset"), preset->name))
                    {
                        return 0;
                    }
                    state->presets.erase(state->presets.begin() + state->selected_index);
                    if (state->selected_index >= static_cast<int>(state->presets.size()))
                    {
                        state->selected_index = static_cast<int>(state->presets.size() - 1);
                    }
                    state->selected_index = RefreshPresetList(state->preset_list, state->presets, state->selected_index);
                    SyncSelection(state);
                    return 0;
                }
            case kImportPresetId:
                {
                    std::wstring path;
                    if (!ui::PromptOpenFile(hwnd, kThemeFilter, &path))
                    {
                        return 0;
                    }
                    std::vector<ThemePreset> imported;
                    std::wstring error;
                    if (!ThemePresetStore::ImportFromFile(path, &imported, &error))
                    {
                        ui::ShowError(hwnd, error.empty() ? util::Tr(L"Failed to import theme presets.") : error);
                        return 0;
                    }
                    if (!imported.empty())
                    {
                        for (auto& preset : imported)
                        {
                            preset.name = MakeUniquePresetName(state->presets, preset.name);
                            state->presets.push_back(preset);
                        }
                        state->selected_index = static_cast<int>(state->presets.size() - 1);
                        state->selected_index = RefreshPresetList(state->preset_list, state->presets, state->selected_index);
                        SyncSelection(state);
                    }
                    return 0;
                }
            case kExportPresetId:
                {
                    std::wstring path;
                    win32::OpenAfter open_after = win32::OpenAfter::kNone;
                    if (!ui::ReportFileDialogResult(hwnd, win32::ChooseFileToSave(hwnd, kThemeFilter, nullptr, &path, &open_after)))
                    {
                        return 0;
                    }
                    std::wstring error;
                    if (!ThemePresetStore::ExportToFile(path, state->presets, &error))
                    {
                        ui::ShowError(hwnd, error.empty() ? util::Tr(L"Failed to export theme presets.") : error);
                    }
                    else if (open_after == win32::OpenAfter::kEditor)
                    {
                        ui::ReportFileDialogResult(hwnd, win32::OpenInTextEditor(hwnd, path));
                    }
                    return 0;
                }
            case kEditColorId:
                {
                    ThemePreset* preset = CurrentPreset(state);
                    if (!preset || !state->color_list)
                    {
                        return 0;
                    }
                    const int field_index = appearance::SelectedListViewData(state->color_list);
                    if (field_index < 0 || field_index >= static_cast<int>(std::size(kColorFields)))
                    {
                        return 0;
                    }
                    COLORREF* color = &(preset->colors.*(kColorFields[field_index].member));
                    if (ChooseColorFor(hwnd, color, state->custom_colors))
                    {
                        FillColorList(state, preset);
                    }
                    return 0;
                }
            case kDarkCheckId:
                {
                    ThemePreset* preset = CurrentPreset(state);
                    if (!preset || !state->dark_check)
                    {
                        return 0;
                    }
                    preset->is_dark = Button_GetCheck(state->dark_check) == BST_CHECKED;
                    return 0;
                }
            case kApplyTemplateId:
                {
                    ThemePreset* preset = CurrentPreset(state);
                    if (!preset)
                    {
                        return 0;
                    }
                    int sel =
                        state->template_combo ? static_cast<int>(SendMessageW(state->template_combo, CB_GETCURSEL, 0, 0)) : -1;
                    ThemePreset tmpl = BuildPresetFromTemplate(state, sel);
                    preset->colors = tmpl.colors;
                    preset->is_dark = tmpl.is_dark;
                    SyncSelection(state);
                    return 0;
                }
            case kApplyId:
                ApplySelectedPreset(state, false);
                return 0;
            case IDOK:
                ApplySelectedPreset(state, true);
                return 0;
            default:
                break;
            }
            break;
        }
    case WM_NOTIFY:
        {
            auto* hdr = reinterpret_cast<NMHDR*>(lparam);
            LRESULT feature_result = 0;
            if (appearance::HandleListViewNotify(hwnd, hdr, &feature_result))
            {
                return feature_result;
            }
            if (hdr->hwndFrom == state->preset_list && hdr->code == LVN_ITEMCHANGED)
            {
                auto* info = reinterpret_cast<NMLISTVIEW*>(lparam);
                if (info && (info->uNewState & LVIS_SELECTED) && info->iItem >= 0)
                {
                    SyncSelection(state);
                }
                return 0;
            }
            if (hdr->hwndFrom == state->preset_list && hdr->code == NM_CUSTOMDRAW)
            {
                return ui::HandleThemedListViewCustomDraw(state->preset_list, reinterpret_cast<NMLVCUSTOMDRAW*>(lparam));
            }
            if (hdr->hwndFrom == state->color_list && hdr->code == NM_DBLCLK)
            {
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(kEditColorId, 0), 0);
                return 0;
            }
            if (hdr->hwndFrom == state->color_list && hdr->code == LVN_COLUMNCLICK)
            {
                auto* info = reinterpret_cast<NMLISTVIEW*>(lparam);
                if (info)
                {
                    ThemePreset* preset = CurrentPreset(state);
                    appearance::SortListViewItems(state->color_list, info->iSubItem, true, &state->color_sort_column, &state->color_sort_ascending, CompareColorListItems, preset);
                }
                return 0;
            }
            if (hdr->hwndFrom == state->color_list && hdr->code == NM_CUSTOMDRAW)
            {
                auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
                return appearance::HandleListGridCustomDraw(state->color_list, draw, ui::HandleThemedListViewCustomDraw(state->color_list, draw));
            }
            break;
        }
    case WM_NCDESTROY:
        appearance::ReleaseListViews(hwnd);
        break;
    default:
        break;
    }
    return appearance::DefDialogWindowProc(hwnd, msg, wparam, lparam);
}

} // namespace

void appearance::ShowThemePresetEditor(HWND owner, const std::vector<ThemePreset>& presets, const std::wstring& active_name, appearance::ThemePresetApply apply, appearance::ThemePresetNamePrompt prompt_name, void* context)
{
    ThemePresetWindowState state;
    state.apply = apply;
    state.prompt_name = prompt_name;
    state.apply_context = context;
    state.owner = owner;
    state.presets = presets;
    state.templates = ThemePresetStore::BuiltInPresets();
    state.active_name = active_name;
    const UINT dpi = win32::DpiForWindow(owner);
    const SIZE size = appearance::DialogWindowSize(owner, appearance::metrics::Scaled(kWindowWidth, dpi), appearance::metrics::Scaled(kWindowHeight, dpi), WS_CLIPCHILDREN);
    appearance::RunDialogWindow(&state, kThemePresetClass, ThemePresetWindowProc, util::Tr(L"Theme Presets"), size, WS_CLIPCHILDREN);
}

} // namespace regkit
