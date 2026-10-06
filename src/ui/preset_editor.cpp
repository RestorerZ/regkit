// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/preset_editor.h"

#include <algorithm>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <windowsx.h>

#include "resource.h"
#include "ui/dialog_layout.h"
#include "ui/dialog_support.h"
#include "ui/feedback.h"
#include "ui/list_view_support.h"
#include "win32/file_dialog.h"
#include "win32/text_transform.h"
#include "win32/translation.h"

namespace regkit
{

namespace
{

constexpr UINT_PTR kThemePresetListViewSubclassId = 2;
constexpr int kColorGridId = 5014;

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

struct ThemePresetWindowState
{
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    HWND preset_list = nullptr;
    HWND color_list = nullptr;
    HWND dark_check = nullptr;
    HWND template_combo = nullptr;
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
        EndDialog(state->hwnd, IDOK);
    }
}

INT_PTR CALLBACK ThemePresetDialogProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<ThemePresetWindowState*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (msg == WM_INITDIALOG)
    {
        state = reinterpret_cast<ThemePresetWindowState*>(lparam);
        SetWindowLongPtrW(hwnd, DWLP_USER, lparam);
        state->hwnd = hwnd;
        state->preset_list = GetDlgItem(hwnd, IDC_PRESET_LIST);
        state->color_list = GetDlgItem(hwnd, IDC_PRESET_COLORS);
        state->dark_check = GetDlgItem(hwnd, IDC_PRESET_DARK);
        state->template_combo = GetDlgItem(hwnd, IDC_PRESET_TEMPLATE);
        editors::dialog_support::Initialize(hwnd, &state->font, {});
        SetupPresetListView(state->preset_list);
        SetupColorListView(state->color_list);
        appearance::LayoutListViews(hwnd);
        PopulateTemplates(state);
        PopulatePresets(state);
        return TRUE;
    }
    if (!state)
    {
        return FALSE;
    }
    if (msg == WM_SETTINGCHANGE)
    {
        if (Theme::UpdateFromSystem())
        {
            appearance::ApplyDialogTheme(hwnd);
            RefreshThemeRendering(state);
        }
        return TRUE;
    }
    INT_PTR themed = 0;
    if (editors::dialog_support::HandleThemeMessage(hwnd, msg, wparam, lparam, &themed))
    {
        return themed;
    }
    switch (msg)
    {
    case WM_COMMAND:
        {
            int id = LOWORD(wparam);
            if (appearance::HandleListViewCommand(hwnd, id))
            {
                return TRUE;
            }
            switch (id)
            {
            case IDC_PRESET_NEW:
                {
                    std::wstring name;
                    if (!PromptPresetName(state, hwnd, util::Tr(L"New Preset"), L"", &name))
                    {
                        return TRUE;
                    }
                    int sel =
                        state->template_combo ? static_cast<int>(SendMessageW(state->template_combo, CB_GETCURSEL, 0, 0)) : -1;
                    ThemePreset preset = BuildPresetFromTemplate(state, sel);
                    preset.name = MakeUniquePresetName(state->presets, name);
                    state->presets.push_back(preset);
                    state->selected_index = static_cast<int>(state->presets.size() - 1);
                    state->selected_index = RefreshPresetList(state->preset_list, state->presets, state->selected_index);
                    SyncSelection(state);
                    return TRUE;
                }
            case IDC_PRESET_DUPLICATE:
                {
                    ThemePreset* preset = CurrentPreset(state);
                    if (!preset)
                    {
                        return TRUE;
                    }
                    ThemePreset copy = *preset;
                    copy.name = MakeUniquePresetName(state->presets, preset->name + L" Copy");
                    state->presets.push_back(copy);
                    state->selected_index = static_cast<int>(state->presets.size() - 1);
                    state->selected_index = RefreshPresetList(state->preset_list, state->presets, state->selected_index);
                    SyncSelection(state);
                    return TRUE;
                }
            case IDC_PRESET_RENAME:
                {
                    ThemePreset* preset = CurrentPreset(state);
                    if (!preset)
                    {
                        return TRUE;
                    }
                    std::wstring name;
                    if (!PromptPresetName(state, hwnd, util::Tr(L"Rename Preset"), preset->name, &name))
                    {
                        return TRUE;
                    }
                    preset->name = MakeUniquePresetName(state->presets, name, preset);
                    state->selected_index = RefreshPresetList(state->preset_list, state->presets, state->selected_index);
                    SyncSelection(state);
                    return TRUE;
                }
            case IDC_PRESET_DELETE:
                {
                    if (state->presets.size() <= 1)
                    {
                        ui::ShowWarning(hwnd, util::Tr(L"At least one preset must remain."));
                        return TRUE;
                    }
                    ThemePreset* preset = CurrentPreset(state);
                    if (!preset)
                    {
                        return TRUE;
                    }
                    if (!ui::ConfirmDelete(hwnd, util::Tr(L"Delete Preset"), preset->name))
                    {
                        return TRUE;
                    }
                    state->presets.erase(state->presets.begin() + state->selected_index);
                    if (state->selected_index >= static_cast<int>(state->presets.size()))
                    {
                        state->selected_index = static_cast<int>(state->presets.size() - 1);
                    }
                    state->selected_index = RefreshPresetList(state->preset_list, state->presets, state->selected_index);
                    SyncSelection(state);
                    return TRUE;
                }
            case IDC_PRESET_IMPORT:
                {
                    std::wstring path;
                    if (!ui::PromptOpenFile(hwnd, kThemeFilter, &path))
                    {
                        return TRUE;
                    }
                    std::vector<ThemePreset> imported;
                    std::wstring error;
                    if (!ThemePresetStore::ImportFromFile(path, &imported, &error))
                    {
                        ui::ShowError(hwnd, error.empty() ? util::Tr(L"Failed to import theme presets.") : error);
                        return TRUE;
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
                    return TRUE;
                }
            case IDC_PRESET_EXPORT:
                {
                    std::wstring path;
                    win32::OpenAfter open_after = win32::OpenAfter::kNone;
                    if (!ui::ReportFileDialogResult(hwnd, win32::ChooseFileToSave(hwnd, kThemeFilter, nullptr, &path, &open_after)))
                    {
                        return TRUE;
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
                    return TRUE;
                }
            case IDC_PRESET_EDIT_COLOR:
                {
                    ThemePreset* preset = CurrentPreset(state);
                    if (!preset || !state->color_list)
                    {
                        return TRUE;
                    }
                    const int field_index = appearance::SelectedListViewData(state->color_list);
                    if (field_index < 0 || field_index >= static_cast<int>(std::size(kColorFields)))
                    {
                        return TRUE;
                    }
                    COLORREF* color = &(preset->colors.*(kColorFields[field_index].member));
                    if (ChooseColorFor(hwnd, color, state->custom_colors))
                    {
                        FillColorList(state, preset);
                    }
                    return TRUE;
                }
            case IDC_PRESET_DARK:
                {
                    ThemePreset* preset = CurrentPreset(state);
                    if (!preset || !state->dark_check)
                    {
                        return TRUE;
                    }
                    preset->is_dark = Button_GetCheck(state->dark_check) == BST_CHECKED;
                    return TRUE;
                }
            case IDC_PRESET_APPLY_TEMPLATE:
                {
                    ThemePreset* preset = CurrentPreset(state);
                    if (!preset)
                    {
                        return TRUE;
                    }
                    int sel =
                        state->template_combo ? static_cast<int>(SendMessageW(state->template_combo, CB_GETCURSEL, 0, 0)) : -1;
                    ThemePreset tmpl = BuildPresetFromTemplate(state, sel);
                    preset->colors = tmpl.colors;
                    preset->is_dark = tmpl.is_dark;
                    SyncSelection(state);
                    return TRUE;
                }
            case IDC_PRESET_APPLY:
                ApplySelectedPreset(state, false);
                return TRUE;
            case IDOK:
                ApplySelectedPreset(state, true);
                return TRUE;
            case IDCANCEL:
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            default:
                break;
            }
            break;
        }
    case WM_NOTIFY:
        {
            auto* hdr = reinterpret_cast<NMHDR*>(lparam);
            INT_PTR result = 0;
            if (editors::dialog_support::HandleListViewNotify(hwnd, hdr, &result))
            {
                return result;
            }
            if (hdr->hwndFrom == state->preset_list && hdr->code == LVN_ITEMCHANGED)
            {
                auto* info = reinterpret_cast<NMLISTVIEW*>(lparam);
                if ((info->uNewState & LVIS_SELECTED) && info->iItem >= 0)
                {
                    SyncSelection(state);
                }
            }
            else if (hdr->hwndFrom == state->color_list && hdr->code == NM_DBLCLK)
            {
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_PRESET_EDIT_COLOR, 0), 0);
            }
            else if (hdr->hwndFrom == state->color_list && hdr->code == LVN_COLUMNCLICK)
            {
                auto* info = reinterpret_cast<NMLISTVIEW*>(lparam);
                appearance::SortListViewItems(state->color_list, info->iSubItem, true, &state->color_sort_column, &state->color_sort_ascending, CompareColorListItems, CurrentPreset(state));
            }
            return FALSE;
        }
    case WM_DESTROY:
        appearance::ReleaseListViews(hwnd);
        editors::dialog_support::ReleaseFont(&state->font);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

} // namespace

void appearance::ShowThemePresetEditor(HWND owner, const std::vector<ThemePreset>& presets, const std::wstring& active_name, appearance::ThemePresetApply apply, appearance::ThemePresetNamePrompt prompt_name, void* context)
{
    ThemePresetWindowState state;
    state.apply = apply;
    state.prompt_name = prompt_name;
    state.apply_context = context;
    state.presets = presets;
    state.templates = ThemePresetStore::BuiltInPresets();
    state.active_name = active_name;
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_THEME_PRESETS), owner, ThemePresetDialogProc, reinterpret_cast<LPARAM>(&state));
}

} // namespace regkit
