// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "editors/bitfield_definition_editor.h"

#include "appearance/dialog_layout.h"
#include "appearance/feedback.h"
#include "appearance/list_view_support.h"
#include "appearance/theme.h"
#include "editors/dialog_support.h"
#include "win32/file_dialog.h"
#include "win32/window_metrics.h"

#include "resource.h"
#include "win32/text_transform.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace regkit::editors {

namespace {

using bitfield::Definition;
using bitfield::DefinitionFile;
using bitfield::Field;

constexpr unsigned kWidths[] = {8, 16, 32, 64};

struct FieldEditor {
  Field field;
  const Definition* parent = nullptr;
  int editing = -1;
  bool accepted = false;
  int sort_column = 0;
  bool sort_ascending = true;
  HFONT ui_font = nullptr;
  appearance::DialogResizer resizer;
};

struct Editor {
  DefinitionFile file;
  int selected = 0;
  bool single = false;
  bool lock_width = false;
  bool dirty = false;
  bool saved = false;
  bool accepted = false;
  bool loading = false;
  bool filtering = false;
  bool updating_combo = false;
  int field_sort_column = 0;
  bool field_sort_ascending = true;
  HFONT ui_font = nullptr;
  appearance::DialogResizer resizer;

  Definition& definition() {
    return file.definitions[static_cast<size_t>(selected)];
  }
};

std::wstring StatesText(const Field& field);

int CALLBACK CompareFieldBits(
    LPARAM left_data,
    LPARAM right_data,
    int column,
    void* context
) {
  auto* state = static_cast<FieldEditor*>(context);
  const unsigned left = static_cast<unsigned>(left_data);
  const unsigned right = static_cast<unsigned>(right_data);
  int result = 0;
  if (column == 0 || column == 1) {
    result = left < right ? -1 : left > right ? 1
                                              : 0;
  } else if (column == 2 && state && state->parent) {
    const int left_owner = state->parent->FieldIndexForBit(left);
    const int right_owner = state->parent->FieldIndexForBit(right);
    const std::wstring left_name = left_owner >= 0
                                       ? state->parent->fields[static_cast<size_t>(left_owner)].name
                                       : std::wstring();
    const std::wstring right_name = right_owner >= 0
                                        ? state->parent->fields[static_cast<size_t>(right_owner)].name
                                        : std::wstring();
    result = util::CompareListText(left_name, right_name);
  }
  return result != 0 ? result : (left < right ? -1 : left > right ? 1
                                                                  : 0);
}

int CALLBACK CompareDefinitionFields(
    LPARAM left_data,
    LPARAM right_data,
    int column,
    void* context
) {
  auto* state = static_cast<Editor*>(context);
  if (!state || left_data < 0 || right_data < 0) {
    return 0;
  }
  const Definition& definition = state->definition();
  const size_t left_index = static_cast<size_t>(left_data);
  const size_t right_index = static_cast<size_t>(right_data);
  if (left_index >= definition.fields.size() || right_index >= definition.fields.size()) {
    return 0;
  }
  const Field& left = definition.fields[left_index];
  const Field& right = definition.fields[right_index];
  int result = 0;
  switch (column) {
  case 0:
    result = util::CompareListText(left.name, right.name);
    break;
  case 1:
    result = std::lexicographical_compare(left.bits.begin(), left.bits.end(), right.bits.begin(), right.bits.end())
                 ? -1
             : std::lexicographical_compare(right.bits.begin(), right.bits.end(), left.bits.begin(), left.bits.end()) ? 1
                                                                                                                      : 0;
    break;
  case 2:
    result = util::CompareListText(StatesText(left), StatesText(right));
    break;
  case 3:
    result = util::CompareListText(left.meaning, right.meaning);
    break;
  default:
    break;
  }
  return result != 0 ? result : (left_index < right_index ? -1 : left_index > right_index ? 1
                                                                                          : 0);
}

int SelectedFieldIndex(
    HWND list
) {
  LPARAM data = -1;
  const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
  return appearance::ListViewItemData(list, row, &data) >= 0
             ? static_cast<int>(data)
             : -1;
}

std::wstring BitsText(
    const Field& field
) {
  std::wstring text;
  for (size_t i = 0; i < field.bits.size(); ++i) {
    if (i > 0) {
      text.append(L", ");
    }
    text.append(std::to_wstring(field.bits[i]));
  }
  return text;
}

std::wstring StatesText(
    const Field& field
) {
  std::wstring text;
  for (size_t i = 0; i < field.states.size(); ++i) {
    if (i > 0) {
      text.append(L", ");
    }
    text.append(std::to_wstring(field.states[i].value)).append(L"=").append(field.states[i].name);
  }
  return text;
}

INT_PTR CALLBACK FieldDialogProc(
    HWND dialog,
    UINT message,
    WPARAM wparam,
    LPARAM lparam
) {
  auto* state = reinterpret_cast<FieldEditor*>(GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<FieldEditor*>(lparam);
    SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    SetWindowTextW(dialog, state->editing < 0 ? L"Add Field" : L"Edit Field");
    SetDlgItemTextW(dialog, IDC_FIELD_NAME, state->field.name.c_str());
    SetDlgItemTextW(dialog, IDC_FIELD_MEANING, dialog_support::ToDisplayText(state->field.meaning).c_str());
    const HWND list = GetDlgItem(dialog, IDC_FIELD_BITS);
    dialog_support::SetupListView(list, LVS_EX_CHECKBOXES, {{L"Bit", 60}, {L"Mask", 150}, {L"Field", 160}});
    for (unsigned bit = 0; bit < state->parent->bit_width; ++bit) {
      const int owner = state->parent->FieldIndexForBit(bit);
      if (owner >= 0 && owner != state->editing) {
        continue;
      }
      const std::wstring text = std::to_wstring(bit);
      LVITEMW item = {};
      item.mask = LVIF_TEXT | LVIF_PARAM;
      item.iItem = ListView_GetItemCount(list);
      item.pszText = const_cast<wchar_t*>(text.c_str());
      item.lParam = static_cast<LPARAM>(bit);
      ListView_InsertItem(list, &item);
      wchar_t mask[32] = {};
      swprintf_s(mask, L"0x%0*llX", static_cast<int>(state->parent->bit_width / 4), 1ull << bit);
      ListView_SetItemText(list, item.iItem, 1, mask);
      if (owner == state->editing && state->editing >= 0) {
        ListView_SetItemText(list, item.iItem, 2, const_cast<wchar_t*>(state->field.name.c_str()));
      }
      const bool checked = std::find(state->field.bits.begin(), state->field.bits.end(), bit) != state->field.bits.end();
      ListView_SetCheckState(list, item.iItem, checked);
    }
    appearance::SortListViewItems(
        list,
        state->sort_column,
        false,
        &state->sort_column,
        &state->sort_ascending,
        CompareFieldBits,
        state
    );
    if (!state->field.states.empty()) {
      SetDlgItemTextW(dialog, IDC_FIELD_STATES, dialog_support::ToDisplayText(StatesText(state->field)).c_str());
      SendDlgItemMessageW(dialog, IDC_FIELD_STATES, EM_SETREADONLY, TRUE, 0);
    }
    dialog_support::Initialize(dialog, &state->ui_font, {IDC_FIELD_NAME, IDC_FIELD_MEANING, IDC_FIELD_STATES});
    dialog_support::RefreshListViewTheme(list);
    using namespace appearance;
    state->resizer.Attach(dialog, {
                                      {IDC_FIELD_NAME, kAnchorLeft | kAnchorTop | kAnchorRight},
                                      {IDC_FIELD_MEANING, kAnchorLeft | kAnchorTop | kAnchorRight},
                                      {IDC_FIELD_STATES, kAnchorLeft | kAnchorTop | kAnchorRight},
                                      {IDC_FIELD_BITS, kAnchorLeft | kAnchorTop | kAnchorRight | kAnchorBottom},
                                      {IDOK, kAnchorRight | kAnchorBottom},
                                      {IDCANCEL, kAnchorRight | kAnchorBottom},
                                  });
    return TRUE;
  }
  if (message == WM_DESTROY) {
    if (state) {
      dialog_support::ReleaseFont(&state->ui_font);
    }
    dialog_support::ReleaseDialogLists(dialog);
    return TRUE;
  }
  if (message == WM_SIZE && state) {
    state->resizer.Apply(dialog);
    dialog_support::LayoutGridToggles(dialog);
    return TRUE;
  }
  if (message == WM_GETMINMAXINFO && state) {
    state->resizer.ClampMinSize(reinterpret_cast<MINMAXINFO*>(lparam));
    return TRUE;
  }
  if (message == WM_NOTIFY && state) {
    auto* header = reinterpret_cast<NMHDR*>(lparam);
    if (header->idFrom == IDC_FIELD_BITS && header->code == LVN_COLUMNCLICK) {
      auto* info = reinterpret_cast<NMLISTVIEW*>(lparam);
      appearance::SortListViewItems(
          GetDlgItem(dialog, IDC_FIELD_BITS),
          info->iSubItem,
          true,
          &state->sort_column,
          &state->sort_ascending,
          CompareFieldBits,
          state
      );
      return TRUE;
    }
    INT_PTR drawn = 0;
    if (dialog_support::HandleListViewNotify(dialog, header, &drawn)) {
      return drawn;
    }
  }
  INT_PTR themed = 0;
  if (dialog_support::HandleThemeMessage(dialog, message, wparam, lparam, &themed)) {
    if (message == WM_SETTINGCHANGE) {
      dialog_support::RefreshListViewTheme(GetDlgItem(dialog, IDC_FIELD_BITS));
    }
    return themed;
  }
  if (message != WM_COMMAND || !state) {
    return FALSE;
  }
  if (dialog_support::HandleGridToggle(dialog, LOWORD(wparam))) {
    return TRUE;
  }
  switch (LOWORD(wparam)) {
  case IDOK:
    {
      Field result;
      result.name = dialog_support::ReadText(dialog, IDC_FIELD_NAME);
      result.meaning = dialog_support::FromDisplayText(dialog_support::ReadText(dialog, IDC_FIELD_MEANING));
      if (result.name.empty()) {
        ui::ShowError(dialog, L"Enter a field name.");
        return TRUE;
      }
      for (size_t i = 0; i < state->parent->fields.size(); ++i) {
        if (static_cast<int>(i) != state->editing &&
            util::EqualsInsensitive(state->parent->fields[i].name, result.name)) {
          ui::ShowError(dialog, L"Another field already uses that name.");
          return TRUE;
        }
      }
      const HWND list = GetDlgItem(dialog, IDC_FIELD_BITS);
      const int count = ListView_GetItemCount(list);
      for (int row = 0; row < count; ++row) {
        LPARAM bit = 0;
        if (ListView_GetCheckState(list, row) &&
            appearance::ListViewItemData(list, row, &bit) >= 0) {
          result.bits.push_back(static_cast<unsigned>(bit));
        }
      }
      std::sort(result.bits.begin(), result.bits.end());
      if (result.bits.empty()) {
        ui::ShowError(dialog, L"Select at least one bit.");
        return TRUE;
      }
      const uint64_t limit = bitfield::WidthMask(static_cast<unsigned>(result.bits.size()));
      for (const bitfield::State& value : state->field.states) {
        if (value.value <= limit) {
          result.states.push_back(value);
        }
      }
      state->field = std::move(result);
      state->accepted = true;
      EndDialog(dialog, IDOK);
      return TRUE;
    }
  case IDCANCEL:
    EndDialog(dialog, IDCANCEL);
    return TRUE;
  default:
    return FALSE;
  }
}

void RefreshFieldList(
    HWND dialog,
    Editor* state
) {
  const Definition& definition = state->definition();
  const HWND list = GetDlgItem(dialog, IDC_DEF_LIST);
  SendMessageW(list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(list);
  for (size_t i = 0; i < definition.fields.size(); ++i) {
    const Field& field = definition.fields[i];
    LVITEMW item = {};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = static_cast<int>(i);
    item.pszText = const_cast<wchar_t*>(field.name.c_str());
    item.lParam = static_cast<LPARAM>(i);
    ListView_InsertItem(list, &item);
    const std::wstring bits = BitsText(field);
    ListView_SetItemText(list, item.iItem, 1, const_cast<wchar_t*>(bits.c_str()));
    const std::wstring states = dialog_support::SingleLine(StatesText(field));
    ListView_SetItemText(list, item.iItem, 2, const_cast<wchar_t*>(states.c_str()));
    const std::wstring meaning = dialog_support::SingleLine(field.meaning);
    ListView_SetItemText(list, item.iItem, 3, const_cast<wchar_t*>(meaning.c_str()));
  }
  appearance::SortListViewItems(
      list,
      state->field_sort_column,
      false,
      &state->field_sort_column,
      &state->field_sort_ascending,
      CompareDefinitionFields,
      state
  );
  SendMessageW(list, WM_SETREDRAW, TRUE, 0);
  RedrawWindow(list, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
}

unsigned SelectedWidth(
    HWND dialog
) {
  const int index = static_cast<int>(SendDlgItemMessageW(dialog, IDC_DEF_WIDTH, CB_GETCURSEL, 0, 0));
  if (index < 0 || index >= static_cast<int>(_countof(kWidths))) {
    return 32;
  }
  return kWidths[index];
}

void SelectWidth(
    HWND dialog,
    unsigned width
) {
  for (int i = 0; i < static_cast<int>(_countof(kWidths)); ++i) {
    if (kWidths[i] == width) {
      SendDlgItemMessageW(dialog, IDC_DEF_WIDTH, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
      return;
    }
  }
}

unsigned ReadOffset(
    HWND dialog
) {
  const std::wstring text = dialog_support::ReadText(dialog, IDC_DEF_OFFSET);
  unsigned long long value = wcstoull(text.c_str(), nullptr, 10);
  if (value > bitfield::kMaxByteOffset) {
    value = bitfield::kMaxByteOffset;
  }
  return static_cast<unsigned>(value);
}

void ShowDefinition(
    HWND dialog,
    Editor* state
) {
  state->loading = true;
  const Definition& definition = state->definition();
  SetDlgItemTextW(dialog, IDC_DEF_VALUE_NAME, definition.value_name.c_str());
  SetDlgItemTextW(dialog, IDC_DEF_KEY_PATHS, util::JoinLines(definition.key_paths).c_str());
  SetDlgItemTextW(dialog, IDC_DEF_COMMENT, dialog_support::ToDisplayText(definition.comment).c_str());
  SetDlgItemInt(dialog, IDC_DEF_OFFSET, definition.byte_offset, FALSE);
  SelectWidth(dialog, definition.bit_width);
  RefreshFieldList(dialog, state);
  state->loading = false;
}

void RefreshDefinitionCombo(
    HWND dialog,
    Editor* state
) {
  const HWND combo = GetDlgItem(dialog, IDC_DEF_SELECT);
  const std::wstring filter = state->filtering ? dialog_support::ReadText(dialog, IDC_DEF_SELECT) : std::wstring();
  const DWORD selection = state->filtering ? static_cast<DWORD>(SendMessageW(combo, CB_GETEDITSEL, 0, 0)) : 0;
  state->updating_combo = true;
  SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  bool shown = false;
  for (size_t i = 0; i < state->file.definitions.size(); ++i) {
    const std::wstring label = bitfield::DisplayName(state->file.definitions[i]);
    if (static_cast<int>(i) != state->selected && !dialog_support::Matches(label, filter)) {
      continue;
    }
    const int index = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
    SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
    if (static_cast<int>(i) == state->selected && !state->filtering) {
      SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
      shown = true;
    }
  }
  if (!shown && !state->filtering) {
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
  }
  if (!state->filtering) {
    SendMessageW(combo, CB_SETEDITSEL, 0, MAKELPARAM(-1, 0));
  }
  if (state->filtering) {
    SetWindowTextW(combo, filter.c_str());
    SendMessageW(combo, CB_SETEDITSEL, 0, static_cast<LPARAM>(selection));
  }
  SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(combo, nullptr, TRUE);
  dialog_support::FitDroppedWidth(combo);
  EnableWindow(GetDlgItem(dialog, IDC_DEF_REMOVE_DEF), !state->single && state->file.definitions.size() > 1);
  state->updating_combo = false;
}

bool CollectDefinition(
    HWND dialog,
    Editor* state,
    Definition* out
) {
  Definition draft = state->definition();
  draft.value_name = dialog_support::ReadText(dialog, IDC_DEF_VALUE_NAME);
  draft.key_paths = util::SplitLines(dialog_support::ReadText(dialog, IDC_DEF_KEY_PATHS));
  draft.comment = dialog_support::FromDisplayText(dialog_support::ReadText(dialog, IDC_DEF_COMMENT));
  draft.bit_width = SelectedWidth(dialog);
  draft.byte_offset = ReadOffset(dialog);
  std::wstring error;
  if (!bitfield::Validate(&draft, &error)) {
    ui::ShowError(dialog, error);
    return false;
  }
  *out = std::move(draft);
  return true;
}

bool CommitDefinition(
    HWND dialog,
    Editor* state
) {
  Definition draft;
  if (!CollectDefinition(dialog, state, &draft)) {
    return false;
  }
  state->definition() = std::move(draft);
  return true;
}

bool SaveToFile(
    HWND owner,
    DefinitionFile* file
) {
  std::wstring path;
  const std::wstring suggested =
      file->definitions.size() == 1
          ? bitfield::SuggestedFileName(file->definitions.front().value_name)
          : bitfield::SuggestedFileName(file->name);
  const HRESULT hr = win32::ChooseFileToSave(
      owner,
      bitfield::FileFilter(),
      bitfield::FileExtension(),
      suggested.c_str(),
      &path
  );
  if (!ui::ReportFileDialogResult(owner, hr)) {
    return false;
  }
  std::wstring error;
  if (!bitfield::Save(path, *file, &error)) {
    ui::ShowError(owner, error);
    return false;
  }
  file->path = path;
  return true;
}

bool SaveDefinition(
    HWND dialog,
    Editor* state
) {
  if (!CommitDefinition(dialog, state)) {
    return false;
  }
  if (!SaveToFile(dialog, &state->file)) {
    return false;
  }
  state->dirty = false;
  state->saved = true;
  return true;
}

bool DiscardChanges(
    HWND dialog,
    Editor* state
) {
  if (!state->dirty) {
    return true;
  }
  const int choice = ui::PromptChoice(
      dialog,
      L"This definition file has unsaved changes.",
      L"Bit Definitions",
      L"Save",
      L"Discard",
      L"Cancel",
      {70, 80, 70}
  );
  if (choice == IDCANCEL) {
    return false;
  }
  if (choice == IDYES) {
    return SaveDefinition(dialog, state);
  }
  return true;
}

void LoadIntoEditor(
    HWND dialog,
    Editor* state,
    const std::wstring& preset
) {
  if (!DiscardChanges(dialog, state)) {
    return;
  }
  std::wstring path = preset;
  if (path.empty()) {
    const HRESULT hr = win32::ChooseFileToOpen(dialog, bitfield::FileFilter(), &path);
    if (!ui::ReportFileDialogResult(dialog, hr)) {
      return;
    }
  }
  DefinitionFile loaded;
  std::wstring error;
  if (!bitfield::Load(path, &loaded, &error)) {
    ui::ShowError(dialog, error);
    return;
  }
  if (state->lock_width && loaded.definitions.front().bit_width != state->definition().bit_width) {
    ui::ShowError(dialog, L"The definition was made for a different value width.");
    return;
  }
  if (state->single) {
    loaded.definitions.resize(1);
  }
  state->file = std::move(loaded);
  state->selected = 0;
  state->dirty = false;
  state->saved = true;
  RefreshDefinitionCombo(dialog, state);
  ShowDefinition(dialog, state);
}

void ChangeWidth(
    HWND dialog,
    Editor* state
) {
  const unsigned width = SelectedWidth(dialog);
  Definition& definition = state->definition();
  bool affected = false;
  for (const Field& field : definition.fields) {
    if (!field.bits.empty() && field.bits.back() >= width) {
      affected = true;
      break;
    }
  }
  if (affected) {
    const int choice = ui::PromptChoice(
        dialog,
        L"Some fields use bits outside the new width. Remove those bits?",
        L"Bit Definitions",
        L"Remove",
        L"Cancel",
        L"",
        {80, 70, 70}
    );
    if (choice != IDYES) {
      SelectWidth(dialog, definition.bit_width);
      return;
    }
    for (Field& field : definition.fields) {
      field.bits.erase(
          std::remove_if(field.bits.begin(), field.bits.end(), [width](unsigned bit) { return bit >= width; }),
          field.bits.end()
      );
    }
    definition.fields.erase(
        std::remove_if(
            definition.fields.begin(),
            definition.fields.end(),
            [](const Field& field) { return field.bits.empty(); }
        ),
        definition.fields.end()
    );
    RefreshFieldList(dialog, state);
  }
  definition.bit_width = width;
  bitfield::BuildLookup(&definition);
  state->dirty = true;
}

void AddOrEditField(
    HWND dialog,
    Editor* state,
    bool create
) {
  const HWND list = GetDlgItem(dialog, IDC_DEF_LIST);
  const int selected = create ? -1 : SelectedFieldIndex(list);
  if (!create && selected < 0) {
    return;
  }
  Definition& definition = state->definition();
  Field field;
  if (!create) {
    field = definition.fields[static_cast<size_t>(selected)];
  }
  if (!EditBitfieldField(dialog, definition, selected, &field)) {
    return;
  }
  Definition draft = definition;
  if (create) {
    draft.fields.push_back(std::move(field));
  } else {
    draft.fields[static_cast<size_t>(selected)] = std::move(field);
  }
  std::wstring error;
  if (!bitfield::Validate(&draft, &error)) {
    ui::ShowError(dialog, error);
    return;
  }
  definition = std::move(draft);
  state->dirty = true;
  RefreshFieldList(dialog, state);
}

void RemoveField(
    HWND dialog,
    Editor* state
) {
  const HWND list = GetDlgItem(dialog, IDC_DEF_LIST);
  const int selected = SelectedFieldIndex(list);
  if (selected < 0) {
    return;
  }
  Definition& definition = state->definition();
  definition.fields.erase(definition.fields.begin() + selected);
  bitfield::BuildLookup(&definition);
  state->dirty = true;
  RefreshFieldList(dialog, state);
}

void AddDefinition(
    HWND dialog,
    Editor* state
) {
  if (!CommitDefinition(dialog, state)) {
    return;
  }
  Definition fresh;
  fresh.owner.fill(-1);
  fresh.bit_width = state->definition().bit_width;
  fresh.name = L"New definition";
  state->file.definitions.push_back(std::move(fresh));
  state->selected = static_cast<int>(state->file.definitions.size()) - 1;
  state->dirty = true;
  RefreshDefinitionCombo(dialog, state);
  ShowDefinition(dialog, state);
}

void RemoveDefinition(
    HWND dialog,
    Editor* state
) {
  if (state->file.definitions.size() <= 1) {
    return;
  }
  state->file.definitions.erase(state->file.definitions.begin() + state->selected);
  state->selected = std::min<int>(state->selected, static_cast<int>(state->file.definitions.size()) - 1);
  state->dirty = true;
  RefreshDefinitionCombo(dialog, state);
  ShowDefinition(dialog, state);
}

void RenameFromCombo(
    HWND dialog,
    Editor* state
) {
  std::wstring text = dialog_support::ReadText(dialog, IDC_DEF_SELECT);
  state->filtering = false;
  if (text.empty() || text == state->definition().name) {
    RefreshDefinitionCombo(dialog, state);
    return;
  }
  for (const Definition& other : state->file.definitions) {
    if (&other != &state->definition() && other.name == text) {
      RefreshDefinitionCombo(dialog, state);
      return;
    }
  }
  state->definition().name = std::move(text);
  state->dirty = true;
  RefreshDefinitionCombo(dialog, state);
}

constexpr int kFieldMenuAdd = 1;
constexpr int kFieldMenuEdit = 2;
constexpr int kFieldMenuRemove = 3;
constexpr int kFieldMenuCopy = 4;

void ShowFieldMenu(
    HWND dialog,
    Editor* state,
    POINT screen
) {
  const HWND list = GetDlgItem(dialog, IDC_DEF_LIST);
  const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
  HMENU menu = CreatePopupMenu();
  if (!menu) {
    return;
  }
  const UINT row_flags = MF_STRING | (selected >= 0 ? 0 : MF_GRAYED);
  AppendMenuW(menu, MF_STRING, kFieldMenuAdd, L"Add Field...");
  AppendMenuW(menu, row_flags, kFieldMenuEdit, L"Edit Field...");
  AppendMenuW(menu, row_flags, kFieldMenuRemove, L"Remove Field");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, row_flags, kFieldMenuCopy, L"Copy Row");
  const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0, dialog, nullptr);
  DestroyMenu(menu);
  switch (chosen) {
  case kFieldMenuAdd:
    AddOrEditField(dialog, state, true);
    return;
  case kFieldMenuEdit:
    AddOrEditField(dialog, state, false);
    return;
  case kFieldMenuRemove:
    RemoveField(dialog, state);
    return;
  case kFieldMenuCopy:
    {
      std::wstring text;
      for (int column = 0; column < 4; ++column) {
        if (column > 0) {
          text.push_back(L'\t');
        }
        text.append(dialog_support::ListViewText(list, selected, column));
      }
      ui::CopyTextToClipboard(dialog, text);
      return;
    }
  default:
    break;
  }
}

void SelectDefinition(
    HWND dialog,
    Editor* state
) {
  const HWND combo = GetDlgItem(dialog, IDC_DEF_SELECT);
  const int entry = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
  state->filtering = false;
  if (entry < 0) {
    return;
  }
  const int index = static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(entry), 0));
  if (index == state->selected || index < 0 || static_cast<size_t>(index) >= state->file.definitions.size()) {
    return;
  }
  if (!CommitDefinition(dialog, state)) {
    RefreshDefinitionCombo(dialog, state);
    return;
  }
  state->selected = index;
  ShowDefinition(dialog, state);
}

INT_PTR CALLBACK DefinitionDialogProc(
    HWND dialog,
    UINT message,
    WPARAM wparam,
    LPARAM lparam
) {
  auto* state = reinterpret_cast<Editor*>(GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<Editor*>(lparam);
    SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    SetWindowTextW(dialog, L"Bit Definitions");
    for (const unsigned width : kWidths) {
      SendDlgItemMessageW(dialog, IDC_DEF_WIDTH, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(width).c_str()));
    }
    EnableWindow(GetDlgItem(dialog, IDC_DEF_WIDTH), !state->lock_width);
    EnableWindow(GetDlgItem(dialog, IDC_DEF_ADD_DEF), !state->single);
    dialog_support::SetupListView(
        GetDlgItem(dialog, IDC_DEF_LIST),
        0,
        {{L"Field", 150}, {L"Bits", 100}, {L"States", 180}, {L"Meaning", 320}}
    );
    RefreshDefinitionCombo(dialog, state);
    ShowDefinition(dialog, state);
    dialog_support::Initialize(
        dialog,
        &state->ui_font,
        {IDC_DEF_VALUE_NAME, IDC_DEF_KEY_PATHS, IDC_DEF_OFFSET, IDC_DEF_COMMENT}
    );
    dialog_support::AllowNewlines(dialog, IDC_DEF_KEY_PATHS);
    dialog_support::RefreshListViewTheme(GetDlgItem(dialog, IDC_DEF_LIST));
    using namespace appearance;
    state->resizer.Attach(dialog, {
                                      {IDC_DEF_SELECT, kAnchorLeft | kAnchorTop | kAnchorRight},
                                      {IDC_DEF_ADD_DEF, kAnchorTop | kAnchorRight},
                                      {IDC_DEF_REMOVE_DEF, kAnchorTop | kAnchorRight},
                                      {IDC_DEF_VALUE_NAME, kAnchorLeft | kAnchorTop | kAnchorRight},
                                      {IDC_DEF_KEY_PATHS, kAnchorLeft | kAnchorTop | kAnchorRight},
                                      {IDC_DEF_COMMENT, kAnchorLeft | kAnchorTop | kAnchorRight},
                                      {IDC_DEF_LIST, kAnchorLeft | kAnchorTop | kAnchorRight | kAnchorBottom},
                                      {IDC_DEF_ADD, kAnchorLeft | kAnchorBottom},
                                      {IDC_DEF_EDIT, kAnchorLeft | kAnchorBottom},
                                      {IDC_DEF_REMOVE, kAnchorLeft | kAnchorBottom},
                                      {IDC_DEF_OPEN, kAnchorRight | kAnchorBottom},
                                      {IDC_DEF_SAVE_AS, kAnchorRight | kAnchorBottom},
                                      {IDOK, kAnchorRight | kAnchorBottom},
                                      {IDCANCEL, kAnchorRight | kAnchorBottom},
                                  });
    state->dirty = false;
    return TRUE;
  }
  if (message == WM_DESTROY) {
    if (state) {
      dialog_support::ReleaseFont(&state->ui_font);
    }
    dialog_support::ReleaseDialogLists(dialog);
    return TRUE;
  }
  if (message == WM_SIZE && state) {
    state->resizer.Apply(dialog);
    dialog_support::LayoutGridToggles(dialog);
    return TRUE;
  }
  if (message == WM_GETMINMAXINFO && state) {
    state->resizer.ClampMinSize(reinterpret_cast<MINMAXINFO*>(lparam));
    return TRUE;
  }
  if (message == WM_CONTEXTMENU && state) {
    const HWND list = GetDlgItem(dialog, IDC_DEF_LIST);
    if (reinterpret_cast<HWND>(wparam) == list) {
      POINT screen = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (screen.x != -1 || screen.y != -1) {
        POINT client = screen;
        ScreenToClient(list, &client);
        LVHITTESTINFO hit = {};
        hit.pt = client;
        const int row = ListView_HitTest(list, &hit);
        if (row >= 0) {
          ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
      } else {
        RECT rect = {};
        GetWindowRect(list, &rect);
        screen.x = rect.left + 24;
        screen.y = rect.top + 24;
      }
      ShowFieldMenu(dialog, state, screen);
      return TRUE;
    }
  }
  if (message == WM_NOTIFY && state) {
    auto* header = reinterpret_cast<NMHDR*>(lparam);
    if (header->idFrom == IDC_DEF_LIST && header->code == LVN_COLUMNCLICK) {
      auto* info = reinterpret_cast<NMLISTVIEW*>(lparam);
      appearance::SortListViewItems(
          GetDlgItem(dialog, IDC_DEF_LIST),
          info->iSubItem,
          true,
          &state->field_sort_column,
          &state->field_sort_ascending,
          CompareDefinitionFields,
          state
      );
      return TRUE;
    }
    if (header->idFrom == IDC_DEF_LIST && header->code == NM_DBLCLK) {
      AddOrEditField(dialog, state, false);
      return TRUE;
    }
    INT_PTR drawn = 0;
    if (dialog_support::HandleListViewNotify(dialog, header, &drawn)) {
      return drawn;
    }
  }
  INT_PTR themed = 0;
  if (dialog_support::HandleThemeMessage(dialog, message, wparam, lparam, &themed)) {
    if (message == WM_SETTINGCHANGE) {
      dialog_support::RefreshListViewTheme(GetDlgItem(dialog, IDC_DEF_LIST));
    }
    return themed;
  }
  if (message != WM_COMMAND || !state) {
    return FALSE;
  }
  const int id = LOWORD(wparam);
  const int code = HIWORD(wparam);
  if (dialog_support::HandleGridToggle(dialog, id)) {
    return TRUE;
  }
  if (id == IDC_DEF_SELECT) {
    if (code == CBN_EDITCHANGE) {
      if (state->updating_combo) {
        return TRUE;
      }
      state->filtering = true;
      RefreshDefinitionCombo(dialog, state);
      return TRUE;
    }
    if (code == CBN_SELCHANGE) {
      SelectDefinition(dialog, state);
      return TRUE;
    }
    if (code == CBN_KILLFOCUS && state->filtering) {
      RenameFromCombo(dialog, state);
      return TRUE;
    }
    return FALSE;
  }
  if (code == EN_CHANGE && !state->loading) {
    state->dirty = true;
    return TRUE;
  }
  if (code == CBN_SELCHANGE && id == IDC_DEF_WIDTH) {
    ChangeWidth(dialog, state);
    return TRUE;
  }
  switch (id) {
  case IDC_DEF_ADD:
    AddOrEditField(dialog, state, true);
    return TRUE;
  case IDC_DEF_EDIT:
    AddOrEditField(dialog, state, false);
    return TRUE;
  case IDC_DEF_REMOVE:
    RemoveField(dialog, state);
    return TRUE;
  case IDC_DEF_ADD_DEF:
    AddDefinition(dialog, state);
    return TRUE;
  case IDC_DEF_REMOVE_DEF:
    RemoveDefinition(dialog, state);
    return TRUE;
  case IDC_DEF_OPEN:
    LoadIntoEditor(dialog, state, std::wstring());
    return TRUE;
  case IDC_DEF_SAVE_AS:
    if (state->filtering) {
      RenameFromCombo(dialog, state);
    }
    SaveDefinition(dialog, state);
    return TRUE;
  case IDOK:
    if (state->filtering) {
      RenameFromCombo(dialog, state);
    }
    if (!CommitDefinition(dialog, state)) {
      return TRUE;
    }
    state->accepted = true;
    EndDialog(dialog, IDOK);
    return TRUE;
  case IDCANCEL:
    if (DiscardChanges(dialog, state)) {
      EndDialog(dialog, IDCANCEL);
    }
    return TRUE;
  default:
    return FALSE;
  }
}

INT_PTR RunDefinitionDialog(
    HWND owner,
    Editor* state
) {
  return DialogBoxParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(IDD_BITFIELD_DEFINITION),
      owner,
      DefinitionDialogProc,
      reinterpret_cast<LPARAM>(state)
  );
}

} // namespace

bool EditBitfieldField(
    HWND owner,
    const Definition& parent,
    int editing,
    Field* field
) {
  FieldEditor state;
  state.parent = &parent;
  state.editing = editing;
  state.field = *field;
  const INT_PTR outcome = DialogBoxParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(IDD_BITFIELD_FIELD),
      owner,
      FieldDialogProc,
      reinterpret_cast<LPARAM>(&state)
  );
  if (outcome != IDOK || !state.accepted) {
    return false;
  }
  *field = std::move(state.field);
  return true;
}

bool EditBitfieldDefinition(
    HWND owner,
    Definition* definition,
    bool lock_width
) {
  if (!definition) {
    return false;
  }
  Editor state;
  state.single = true;
  state.lock_width = lock_width;
  state.file.definitions.push_back(*definition);
  Definition& working = state.file.definitions.front();
  if (!bitfield::ValidWidth(working.bit_width)) {
    working.bit_width = 32;
  }
  bitfield::BuildLookup(&working);
  if (RunDefinitionDialog(owner, &state) != IDOK || !state.accepted) {
    return false;
  }
  *definition = std::move(state.file.definitions.front());
  return true;
}

void ShowBitfieldDefinitionEditor(
    HWND owner,
    const std::wstring& path
) {
  Editor state;
  if (!path.empty()) {
    std::wstring error;
    if (!bitfield::Load(path, &state.file, &error)) {
      ui::ShowError(owner, error);
      return;
    }
    state.saved = true;
  } else {
    Definition fresh;
    fresh.owner.fill(-1);
    state.file.definitions.push_back(std::move(fresh));
  }
  if (RunDefinitionDialog(owner, &state) == IDOK && state.accepted && !state.saved) {
    SaveToFile(owner, &state.file);
  }
}

} // namespace regkit::editors
