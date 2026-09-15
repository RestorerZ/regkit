// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "editors/bitfield_editor.h"

#include "appearance/dialog_layout.h"
#include "appearance/feedback.h"
#include "appearance/list_view_support.h"
#include "appearance/theme.h"
#include "editors/bitfield_definition_editor.h"
#include "editors/dialog_support.h"
#include "win32/file_dialog.h"
#include "win32/window_metrics.h"

#include "resource.h"

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

constexpr int kColumnMask = 1;
constexpr int kColumnState = 2;
constexpr int kColumnField = 3;
constexpr int kColumnValue = 4;
constexpr int kColumnMeaning = 5;

constexpr int kMenuEnable = 100;
constexpr int kMenuDisable = 101;
constexpr int kMenuEditField = 102;
constexpr int kMenuRemoveField = 103;
constexpr int kMenuCopyMask = 104;
constexpr int kMenuCopyRow = 105;
constexpr int kMenuStateBase = 200;

struct Editor {
  std::wstring value_name;
  std::wstring key_path;
  std::vector<BYTE> bytes;
  bool binary_mode = false;
  bool read_only = false;
  unsigned width = 32;
  unsigned offset = 0;
  uint64_t value = 0;
  bool updating = false;
  bool filtering = false;
  bool updating_combo = false;
  bool accepted = false;
  int sort_column = 0;
  bool sort_ascending = false;
  std::vector<Definition> choices;
  int choice = 0;
  HFONT ui_font = nullptr;
  appearance::DialogResizer resizer;

  Definition& definition() {
    return choices[static_cast<size_t>(choice)];
  }
  const Definition& definition() const {
    return choices[static_cast<size_t>(choice)];
  }
};

const std::wstring& RowMeaning(const Field& field, uint64_t value);

bool BitForRow(
    HWND list,
    int row,
    unsigned* bit
) {
  LPARAM data = 0;
  if (!bit || appearance::ListViewItemData(list, row, &data) < 0 || data < 0) {
    return false;
  }
  *bit = static_cast<unsigned>(data);
  return true;
}

int RowForBit(
    HWND list,
    unsigned bit
) {
  return appearance::FindListViewItemByData(list, static_cast<LPARAM>(bit));
}

int CompareText(
    const std::wstring& left,
    const std::wstring& right
) {
  return _wcsicmp(left.c_str(), right.c_str());
}

int CALLBACK CompareBitRows(
    LPARAM left_data,
    LPARAM right_data,
    int column,
    void* context
) {
  auto* editor = static_cast<Editor*>(context);
  const unsigned left = static_cast<unsigned>(left_data);
  const unsigned right = static_cast<unsigned>(right_data);
  if (!editor || left >= editor->width || right >= editor->width) {
    return 0;
  }
  int result = 0;
  if (column == 0 || column == kColumnMask) {
    result = left < right ? -1 : left > right ? 1
                                              : 0;
  } else if (column == kColumnState) {
    const bool left_set = ((editor->value >> left) & 1ull) != 0;
    const bool right_set = ((editor->value >> right) & 1ull) != 0;
    result = left_set == right_set ? 0 : left_set ? 1
                                                  : -1;
  } else {
    const Field* left_field = editor->definition().FieldForBit(left);
    const Field* right_field = editor->definition().FieldForBit(right);
    if (column == kColumnField) {
      result = CompareText(
          left_field ? left_field->name : std::wstring(),
          right_field ? right_field->name : std::wstring()
      );
    } else if (column == kColumnValue) {
      const uint64_t left_value = left_field ? left_field->Extract(editor->value) : 0;
      const uint64_t right_value = right_field ? right_field->Extract(editor->value) : 0;
      result = left_value < right_value ? -1 : left_value > right_value ? 1
                                                                        : 0;
    } else if (column == kColumnMeaning) {
      result = CompareText(
          left_field ? RowMeaning(*left_field, editor->value) : std::wstring(),
          right_field ? RowMeaning(*right_field, editor->value) : std::wstring()
      );
    }
  }
  return result != 0 ? result : (left < right ? -1 : left > right ? 1
                                                                  : 0);
}

void SortBitRows(
    HWND list,
    Editor* editor,
    int column,
    bool toggle
) {
  appearance::SortListViewItems(
      list,
      column,
      toggle,
      &editor->sort_column,
      &editor->sort_ascending,
      CompareBitRows,
      editor
  );
}

uint64_t ReadWindow(
    const Editor& editor
) {
  uint64_t value = 0;
  const size_t count = editor.width / 8;
  if (editor.offset + count > editor.bytes.size()) {
    return 0;
  }
  for (size_t i = 0; i < count; ++i) {
    value |= static_cast<uint64_t>(editor.bytes[editor.offset + i]) << (8 * i);
  }
  return value;
}

void WriteWindow(
    Editor* editor
) {
  const size_t count = editor->width / 8;
  if (editor->offset + count > editor->bytes.size()) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    editor->bytes[editor->offset + i] = static_cast<BYTE>((editor->value >> (8 * i)) & 0xFF);
  }
}

std::wstring MaskText(
    unsigned width,
    uint64_t mask
) {
  wchar_t buffer[32] = {};
  swprintf_s(buffer, L"0x%0*llX", static_cast<int>(width / 4), mask);
  return buffer;
}

std::wstring FieldValueText(
    const Field& field,
    uint64_t value
) {
  const uint64_t extracted = field.Extract(value);
  std::wstring text = std::to_wstring(extracted);
  if (const bitfield::State* state = field.StateFor(extracted)) {
    text.append(L" (").append(state->name).append(L")");
  }
  return text;
}

const std::wstring& RowMeaning(
    const Field& field,
    uint64_t value
) {
  if (const bitfield::State* state = field.StateFor(field.Extract(value))) {
    if (!state->meaning.empty()) {
      return state->meaning;
    }
  }
  return field.meaning;
}

std::wstring DisplayName(
    const Definition& definition
) {
  if (!definition.name.empty()) {
    return definition.name;
  }
  if (!definition.value_name.empty()) {
    return definition.value_name;
  }
  return L"Unnamed definition";
}

bool FitsWindow(
    const Editor& editor,
    const Definition& definition
) {
  if (!editor.binary_mode) {
    return definition.bit_width == editor.width && definition.byte_offset == 0;
  }
  return static_cast<size_t>(definition.byte_offset) + definition.bit_width / 8 <= editor.bytes.size();
}

void UpdateValueText(
    HWND dialog,
    const Editor& editor
) {
  std::wstring text = MaskText(editor.width, editor.value);
  if (editor.binary_mode) {
    text.append(L"  @ ").append(MaskText(16, editor.offset));
  }
  SetDlgItemTextW(dialog, IDC_VALUE_BYTES, text.c_str());
}

void SetRowText(
    HWND list,
    const Editor& editor,
    int row
) {
  unsigned bit = 0;
  if (!BitForRow(list, row, &bit) || bit >= editor.width) {
    return;
  }
  const bool set = (editor.value >> bit) & 1ull;
  ListView_SetItemText(list, row, kColumnState, const_cast<wchar_t*>(set ? L"1" : L"0"));
  const Field* field = editor.definition().FieldForBit(bit);
  if (!field) {
    ListView_SetItemText(list, row, kColumnField, const_cast<wchar_t*>(L""));
    ListView_SetItemText(list, row, kColumnValue, const_cast<wchar_t*>(L""));
    ListView_SetItemText(list, row, kColumnMeaning, const_cast<wchar_t*>(L""));
    return;
  }
  const std::wstring value = FieldValueText(*field, editor.value);
  const std::wstring meaning = dialog_support::SingleLine(RowMeaning(*field, editor.value));
  ListView_SetItemText(list, row, kColumnField, const_cast<wchar_t*>(field->name.c_str()));
  ListView_SetItemText(list, row, kColumnValue, const_cast<wchar_t*>(value.c_str()));
  ListView_SetItemText(list, row, kColumnMeaning, const_cast<wchar_t*>(meaning.c_str()));
}

void RefreshFieldRows(
    HWND dialog,
    const Editor& editor,
    const Field& field
) {
  const HWND list = GetDlgItem(dialog, IDC_BITFIELD_LIST);
  for (const unsigned bit : field.bits) {
    if (bit < editor.width) {
      const int row = RowForBit(list, bit);
      if (row >= 0) {
        SetRowText(list, editor, row);
      }
    }
  }
}

void FillList(
    HWND dialog,
    Editor* editor
) {
  const HWND list = GetDlgItem(dialog, IDC_BITFIELD_LIST);
  editor->updating = true;
  SendMessageW(list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(list);
  for (unsigned row = 0; row < editor->width; ++row) {
    const unsigned bit = editor->width - 1 - row;
    const std::wstring bit_text = std::to_wstring(bit);
    LVITEMW item = {};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = static_cast<int>(row);
    item.pszText = const_cast<wchar_t*>(bit_text.c_str());
    item.lParam = static_cast<LPARAM>(bit);
    ListView_InsertItem(list, &item);
    const std::wstring mask = MaskText(editor->width, 1ull << bit);
    ListView_SetItemText(list, static_cast<int>(row), kColumnMask, const_cast<wchar_t*>(mask.c_str()));
    SetRowText(list, *editor, static_cast<int>(row));
    if (!editor->read_only) {
      ListView_SetCheckState(list, static_cast<int>(row), (editor->value >> bit) & 1ull);
    }
  }
  SendMessageW(list, WM_SETREDRAW, TRUE, 0);
  SortBitRows(list, editor, editor->sort_column, false);
  RedrawWindow(list, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
  editor->updating = false;
  UpdateValueText(dialog, *editor);
}

void RefreshCombo(
    HWND dialog,
    Editor* editor
) {
  const HWND combo = GetDlgItem(dialog, IDC_BITFIELD_DEFINITION);
  const std::wstring filter = editor->filtering ? dialog_support::ReadText(dialog, IDC_BITFIELD_DEFINITION) : std::wstring();
  const DWORD selection = editor->filtering ? static_cast<DWORD>(SendMessageW(combo, CB_GETEDITSEL, 0, 0)) : 0;
  editor->updating_combo = true;
  SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  for (size_t i = 0; i < editor->choices.size(); ++i) {
    const std::wstring label = i == 0 ? std::wstring(L"(none)") : DisplayName(editor->choices[i]);
    if (static_cast<int>(i) != editor->choice && !dialog_support::Matches(label, filter)) {
      continue;
    }
    const int index = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
    SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
    if (static_cast<int>(i) == editor->choice && !editor->filtering) {
      SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    }
  }
  if (editor->filtering) {
    SetWindowTextW(combo, filter.c_str());
    SendMessageW(combo, CB_SETEDITSEL, 0, static_cast<LPARAM>(selection));
  }
  SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(combo, nullptr, TRUE);
  dialog_support::FitDroppedWidth(combo);
  editor->updating_combo = false;
}

void SelectRow(
    HWND dialog,
    const Editor& editor,
    unsigned bit
) {
  if (bit >= editor.width) {
    return;
  }
  const HWND list = GetDlgItem(dialog, IDC_BITFIELD_LIST);
  const int row = RowForBit(list, bit);
  if (row < 0) {
    return;
  }
  ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_EnsureVisible(list, row, FALSE);
}

void ApplyChoice(
    HWND dialog,
    Editor* editor,
    int choice
) {
  if (choice < 0 || static_cast<size_t>(choice) >= editor->choices.size()) {
    return;
  }
  if (editor->binary_mode) {
    WriteWindow(editor);
  }
  editor->choice = choice;
  RefreshCombo(dialog, editor);
  const Definition& definition = editor->definition();
  if (editor->binary_mode && !definition.value_name.empty()) {
    editor->width = definition.bit_width;
    editor->offset = definition.byte_offset;
    editor->value = ReadWindow(*editor);
  }
  SetDlgItemTextW(dialog, IDC_BITFIELD_COMMENT, dialog_support::ToDisplayText(definition.comment).c_str());
  EnableWindow(GetDlgItem(dialog, IDC_BITFIELD_EDIT_DEF), choice != 0);
  EnableWindow(GetDlgItem(dialog, IDC_BITFIELD_SAVE_AS), choice != 0);
  FillList(dialog, editor);
}

void AddChoice(
    HWND dialog,
    Editor* editor,
    Definition definition
) {
  editor->choices.push_back(std::move(definition));
  ApplyChoice(dialog, editor, static_cast<int>(editor->choices.size()) - 1);
}

bool AcceptDefinition(
    HWND dialog,
    const Editor& editor,
    const Definition& definition
) {
  if (!FitsWindow(editor, definition)) {
    ui::ShowError(
        dialog,
        editor.binary_mode
            ? L"The definition describes bytes outside this value."
            : L"The definition was made for a different value width."
    );
    return false;
  }
  if (!definition.value_name.empty() &&
      _wcsicmp(definition.value_name.c_str(), editor.value_name.c_str()) != 0) {
    std::wstring message = L"This definition was made for a different value.\r\n\r\nDefinition: ";
    message.append(definition.value_name).append(L"\r\nThis value: ").append(editor.value_name.empty() ? L"(Default)" : editor.value_name);
    message.append(L"\r\n\r\nUse it anyway?");
    if (ui::PromptChoice(dialog, message, L"Bit Definition", L"Use", L"Cancel", L"") != IDYES) {
      return false;
    }
  }
  return true;
}

void LoadFromFile(
    HWND dialog,
    Editor* editor
) {
  std::wstring path;
  const HRESULT hr = win32::ChooseFileToOpen(dialog, bitfield::FileFilter(), &path);
  if (!ui::ReportFileDialogResult(dialog, hr)) {
    return;
  }
  DefinitionFile file;
  std::wstring error;
  if (!bitfield::Load(path, &file, &error)) {
    ui::ShowError(dialog, error);
    return;
  }
  std::vector<Definition> named;
  std::vector<Definition> fitting;
  for (Definition& definition : file.definitions) {
    if (!FitsWindow(*editor, definition)) {
      continue;
    }
    if (_wcsicmp(definition.value_name.c_str(), editor->value_name.c_str()) == 0) {
      named.push_back(std::move(definition));
    } else {
      fitting.push_back(std::move(definition));
    }
  }
  std::vector<Definition>& chosen = named.empty() ? fitting : named;
  if (chosen.empty()) {
    ui::ShowError(
        dialog,
        editor->binary_mode
            ? L"No definition in that file fits this value."
            : L"No definition in that file was made for this value width."
    );
    return;
  }
  if (named.empty() && !AcceptDefinition(dialog, *editor, chosen.front())) {
    return;
  }
  const int first = static_cast<int>(editor->choices.size());
  for (Definition& definition : chosen) {
    editor->choices.push_back(std::move(definition));
  }
  ApplyChoice(dialog, editor, first);
}

void SaveCurrent(
    HWND dialog,
    Editor* editor
) {
  const Definition& definition = editor->definition();
  std::wstring path;
  const HRESULT hr = win32::ChooseFileToSave(
      dialog,
      bitfield::FileFilter(),
      bitfield::FileExtension(),
      bitfield::SuggestedFileName(definition.value_name.empty() ? editor->value_name : definition.value_name).c_str(),
      &path
  );
  if (!ui::ReportFileDialogResult(dialog, hr)) {
    return;
  }
  DefinitionFile file;
  file.name = DisplayName(definition);
  file.definitions.push_back(definition);
  std::wstring error;
  if (!bitfield::Save(path, file, &error)) {
    ui::ShowError(dialog, error);
  }
}

void EditOrCreate(
    HWND dialog,
    Editor* editor,
    bool create
) {
  Definition definition;
  if (create) {
    definition.value_name = editor->value_name;
    definition.bit_width = editor->width;
    definition.byte_offset = editor->offset;
    if (!editor->key_path.empty()) {
      definition.key_paths.push_back(editor->key_path);
    }
  } else {
    definition = editor->definition();
  }
  if (!EditBitfieldDefinition(dialog, &definition, !editor->binary_mode)) {
    return;
  }
  if (!AcceptDefinition(dialog, *editor, definition)) {
    return;
  }
  if (create) {
    AddChoice(dialog, editor, std::move(definition));
    return;
  }
  editor->choices[static_cast<size_t>(editor->choice)] = std::move(definition);
  ApplyChoice(dialog, editor, editor->choice);
}

void SetBit(
    HWND dialog,
    Editor* editor,
    unsigned bit,
    bool set
) {
  if (editor->read_only || bit >= editor->width) {
    return;
  }
  if (set) {
    editor->value |= 1ull << bit;
  } else {
    editor->value &= ~(1ull << bit);
  }
  const HWND list = GetDlgItem(dialog, IDC_BITFIELD_LIST);
  const int row = RowForBit(list, bit);
  if (row < 0) {
    return;
  }
  editor->updating = true;
  ListView_SetCheckState(list, row, set);
  editor->updating = false;
  if (const Field* field = editor->definition().FieldForBit(bit)) {
    RefreshFieldRows(dialog, *editor, *field);
  } else {
    SetRowText(list, *editor, row);
  }
  UpdateValueText(dialog, *editor);
}

void ApplyState(
    HWND dialog,
    Editor* editor,
    unsigned bit,
    size_t state_index
) {
  const int index = editor->definition().FieldIndexForBit(bit);
  if (editor->read_only || index < 0) {
    return;
  }
  const Field& field = editor->definition().fields[static_cast<size_t>(index)];
  if (state_index >= field.states.size()) {
    return;
  }
  editor->value = field.Apply(editor->value, field.states[state_index].value);
  const HWND list = GetDlgItem(dialog, IDC_BITFIELD_LIST);
  editor->updating = true;
  for (const unsigned owned : field.bits) {
    const int row = RowForBit(list, owned);
    if (row >= 0) {
      ListView_SetCheckState(list, row, (editor->value >> owned) & 1ull);
    }
  }
  editor->updating = false;
  RefreshFieldRows(dialog, *editor, field);
  UpdateValueText(dialog, *editor);
}

void EditRowField(
    HWND dialog,
    Editor* editor,
    unsigned bit
) {
  Definition& definition = editor->definition();
  if (definition.value_name.empty() && definition.bit_width != editor->width) {
    definition.bit_width = editor->width;
  }
  const int index = definition.FieldIndexForBit(bit);
  Field field;
  if (index >= 0) {
    field = definition.fields[static_cast<size_t>(index)];
  } else {
    field.bits.push_back(bit);
  }
  if (!EditBitfieldField(dialog, definition, index, &field)) {
    return;
  }
  Definition draft = definition;
  if (index >= 0) {
    draft.fields[static_cast<size_t>(index)] = std::move(field);
  } else {
    draft.fields.push_back(std::move(field));
  }
  std::wstring error;
  if (!bitfield::Validate(&draft, &error)) {
    ui::ShowError(dialog, error);
    return;
  }
  definition = std::move(draft);
  FillList(dialog, editor);
  SelectRow(dialog, *editor, bit);
}

void RemoveRowField(
    HWND dialog,
    Editor* editor,
    unsigned bit
) {
  Definition& definition = editor->definition();
  const int index = definition.FieldIndexForBit(bit);
  if (index < 0) {
    return;
  }
  definition.fields.erase(definition.fields.begin() + index);
  bitfield::BuildLookup(&definition);
  FillList(dialog, editor);
  SelectRow(dialog, *editor, bit);
}

void ShowRowMenu(
    HWND dialog,
    Editor* editor,
    int row,
    POINT screen
) {
  if (row < 0 || static_cast<unsigned>(row) >= editor->width) {
    return;
  }
  unsigned bit = 0;
  if (!BitForRow(GetDlgItem(dialog, IDC_BITFIELD_LIST), row, &bit) ||
      bit >= editor->width) {
    return;
  }
  const bool set = (editor->value >> bit) & 1ull;
  const Definition& definition = editor->definition();
  const Field* field = definition.FieldForBit(bit);
  HMENU menu = CreatePopupMenu();
  if (!menu) {
    return;
  }
  const UINT edit_flags = editor->read_only ? MF_GRAYED : 0;
  AppendMenuW(menu, MF_STRING | edit_flags | (set ? MF_GRAYED : 0), kMenuEnable, L"Enable Bit");
  AppendMenuW(menu, MF_STRING | edit_flags | (set ? 0 : MF_GRAYED), kMenuDisable, L"Disable Bit");
  if (field && !field->states.empty()) {
    HMENU states = CreatePopupMenu();
    const uint64_t current = field->Extract(editor->value);
    for (size_t i = 0; i < field->states.size(); ++i) {
      const bitfield::State& state = field->states[i];
      std::wstring label = state.name + L"   (" + std::to_wstring(state.value) + L")";
      AppendMenuW(states, MF_STRING | edit_flags | (state.value == current ? MF_CHECKED : 0), kMenuStateBase + i, label.c_str());
    }
    AppendMenuW(menu, MF_POPUP | edit_flags, reinterpret_cast<UINT_PTR>(states), L"Set Field To");
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuEditField, field ? L"Edit Field..." : L"Describe Bit...");
  AppendMenuW(menu, MF_STRING | (field ? 0 : MF_GRAYED), kMenuRemoveField, L"Remove Field");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuCopyMask, L"Copy Mask");
  AppendMenuW(menu, MF_STRING, kMenuCopyRow, L"Copy Row");

  const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0, dialog, nullptr);
  DestroyMenu(menu);
  switch (chosen) {
  case kMenuEnable:
    SetBit(dialog, editor, bit, true);
    return;
  case kMenuDisable:
    SetBit(dialog, editor, bit, false);
    return;
  case kMenuEditField:
    EditRowField(dialog, editor, bit);
    return;
  case kMenuRemoveField:
    RemoveRowField(dialog, editor, bit);
    return;
  case kMenuCopyMask:
    ui::CopyTextToClipboard(dialog, MaskText(editor->width, 1ull << bit));
    return;
  case kMenuCopyRow:
    {
      const HWND list = GetDlgItem(dialog, IDC_BITFIELD_LIST);
      std::wstring text;
      for (int column = 0; column <= kColumnMeaning; ++column) {
        if (column > 0) {
          text.push_back(L'\t');
        }
        text.append(dialog_support::ListViewText(list, row, column));
      }
      ui::CopyTextToClipboard(dialog, text);
      return;
    }
  default:
    break;
  }
  if (chosen >= kMenuStateBase) {
    ApplyState(dialog, editor, bit, static_cast<size_t>(chosen - kMenuStateBase));
  }
}

void SetAllBits(
    HWND dialog,
    Editor* editor,
    bool set
) {
  if (editor->read_only) {
    return;
  }
  editor->value = set ? bitfield::WidthMask(editor->width) : 0;
  const HWND list = GetDlgItem(dialog, IDC_BITFIELD_LIST);
  editor->updating = true;
  SendMessageW(list, WM_SETREDRAW, FALSE, 0);
  for (unsigned row = 0; row < editor->width; ++row) {
    ListView_SetCheckState(list, static_cast<int>(row), set);
    SetRowText(list, *editor, static_cast<int>(row));
  }
  SendMessageW(list, WM_SETREDRAW, TRUE, 0);
  RedrawWindow(list, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
  editor->updating = false;
  UpdateValueText(dialog, *editor);
}

void HandleItemChanged(
    HWND dialog,
    Editor* editor,
    const NMLISTVIEW* info
) {
  if (editor->updating || !(info->uChanged & LVIF_STATE)) {
    return;
  }
  const UINT old_image = info->uOldState & LVIS_STATEIMAGEMASK;
  const UINT new_image = info->uNewState & LVIS_STATEIMAGEMASK;
  if (old_image == 0 || new_image == 0 || old_image == new_image) {
    return;
  }
  unsigned bit = 0;
  const HWND list = GetDlgItem(dialog, IDC_BITFIELD_LIST);
  if (!BitForRow(list, info->iItem, &bit) || bit >= editor->width) {
    return;
  }
  const bool set = new_image == INDEXTOSTATEIMAGEMASK(2);
  if (set) {
    editor->value |= 1ull << bit;
  } else {
    editor->value &= ~(1ull << bit);
  }
  if (const Field* field = editor->definition().FieldForBit(bit)) {
    RefreshFieldRows(dialog, *editor, *field);
  } else {
    SetRowText(list, *editor, info->iItem);
  }
  UpdateValueText(dialog, *editor);
}

void PopulateChoices(
    HWND dialog,
    Editor* editor
) {
  Definition none;
  none.owner.fill(-1);
  none.bit_width = editor->width;
  editor->choices.push_back(std::move(none));
  for (Definition& definition : bitfield::Matching(editor->key_path, editor->value_name)) {
    if (FitsWindow(*editor, definition)) {
      editor->choices.push_back(std::move(definition));
    }
  }
  RefreshCombo(dialog, editor);
}

INT_PTR CALLBACK DialogProc(
    HWND dialog,
    UINT message,
    WPARAM wparam,
    LPARAM lparam
) {
  auto* editor = reinterpret_cast<Editor*>(GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) {
    editor = reinterpret_cast<Editor*>(lparam);
    SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(editor));
    SetWindowTextW(dialog, L"Edit Bits");
    SetDlgItemTextW(dialog, IDC_VALUE_NAME, editor->value_name.empty() ? L"(Default)" : editor->value_name.c_str());
    SendDlgItemMessageW(dialog, IDC_VALUE_NAME, EM_SETREADONLY, TRUE, 0);
    SendDlgItemMessageW(dialog, IDC_BITFIELD_COMMENT, EM_SETREADONLY, TRUE, 0);
    const HWND name = GetDlgItem(dialog, IDC_VALUE_NAME);
    SetWindowLongPtrW(name, GWL_STYLE, GetWindowLongPtrW(name, GWL_STYLE) & ~WS_TABSTOP);
    if (editor->read_only) {
      EnableWindow(GetDlgItem(dialog, IDC_BITFIELD_SELECT_ALL), FALSE);
      EnableWindow(GetDlgItem(dialog, IDC_BITFIELD_CLEAR_ALL), FALSE);
      EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
    }
    dialog_support::SetupListView(
        GetDlgItem(dialog, IDC_BITFIELD_LIST),
        editor->read_only ? 0u : static_cast<DWORD>(LVS_EX_CHECKBOXES),
        {{L"Bit", 44}, {L"Mask", 150}, {L"State", 62}, {L"Field", 160}, {L"Value", 110}, {L"Meaning", 420}}
    );
    PopulateChoices(dialog, editor);
    dialog_support::Initialize(
        dialog,
        &editor->ui_font,
        {IDC_VALUE_NAME, IDC_BITFIELD_COMMENT}
    );
    dialog_support::RefreshListViewTheme(GetDlgItem(dialog, IDC_BITFIELD_LIST));
    using namespace appearance;
    editor->resizer.Attach(dialog, {
                                       {IDC_VALUE_NAME, kAnchorLeft | kAnchorTop | kAnchorRight},
                                       {IDC_VALUE_BYTES_LABEL, kAnchorTop | kAnchorRight},
                                       {IDC_VALUE_BYTES, kAnchorTop | kAnchorRight},
                                       {IDC_BITFIELD_DEFINITION, kAnchorLeft | kAnchorTop | kAnchorRight},
                                       {IDC_BITFIELD_LOAD, kAnchorTop | kAnchorRight},
                                       {IDC_BITFIELD_NEW, kAnchorTop | kAnchorRight},
                                       {IDC_BITFIELD_EDIT_DEF, kAnchorTop | kAnchorRight},
                                       {IDC_BITFIELD_SAVE_AS, kAnchorTop | kAnchorRight},
                                       {IDC_BITFIELD_COMMENT, kAnchorLeft | kAnchorTop | kAnchorRight},
                                       {IDC_BITFIELD_LIST, kAnchorLeft | kAnchorTop | kAnchorRight | kAnchorBottom},
                                       {IDC_BITFIELD_SELECT_ALL, kAnchorLeft | kAnchorBottom},
                                       {IDC_BITFIELD_CLEAR_ALL, kAnchorLeft | kAnchorBottom},
                                       {IDOK, kAnchorRight | kAnchorBottom},
                                       {IDCANCEL, kAnchorRight | kAnchorBottom},
                                   });
    ApplyChoice(dialog, editor, editor->choices.size() > 1 ? 1 : 0);
    SetFocus(GetDlgItem(dialog, IDC_BITFIELD_LIST));
    return FALSE;
  }
  if (message == WM_DESTROY) {
    if (editor) {
      dialog_support::ReleaseFont(&editor->ui_font);
    }
    dialog_support::ReleaseDialogLists(dialog);
    return TRUE;
  }
  if (message == WM_SIZE && editor) {
    editor->resizer.Apply(dialog);
    dialog_support::LayoutGridToggles(dialog);
    return TRUE;
  }
  if (message == WM_GETMINMAXINFO && editor) {
    editor->resizer.ClampMinSize(reinterpret_cast<MINMAXINFO*>(lparam));
    return TRUE;
  }
  if (message == WM_CONTEXTMENU && editor) {
    const HWND target = reinterpret_cast<HWND>(wparam);
    const HWND list = GetDlgItem(dialog, IDC_BITFIELD_LIST);
    POINT screen = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    if (target == list) {
      int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
      if (screen.x != -1 || screen.y != -1) {
        POINT client = screen;
        ScreenToClient(list, &client);
        LVHITTESTINFO hit = {};
        hit.pt = client;
        const int hit_row = ListView_HitTest(list, &hit);
        if (hit_row >= 0) {
          row = hit_row;
          ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
      } else if (row >= 0) {
        RECT rect = {};
        ListView_GetItemRect(list, row, &rect, LVIR_BOUNDS);
        screen.x = rect.left;
        screen.y = rect.bottom;
        ClientToScreen(list, &screen);
      }
      ShowRowMenu(dialog, editor, row, screen);
      return TRUE;
    }
  }
  if (message == WM_NOTIFY && editor) {
    auto* header = reinterpret_cast<NMHDR*>(lparam);
    if (header->idFrom == IDC_BITFIELD_LIST &&
        header->code == LVN_COLUMNCLICK) {
      auto* info = reinterpret_cast<NMLISTVIEW*>(lparam);
      SortBitRows(
          GetDlgItem(dialog, IDC_BITFIELD_LIST),
          editor,
          info->iSubItem,
          true
      );
      return TRUE;
    }
    if (header->idFrom == IDC_BITFIELD_LIST && header->code == LVN_ITEMCHANGED) {
      HandleItemChanged(dialog, editor, reinterpret_cast<NMLISTVIEW*>(lparam));
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
      dialog_support::RefreshListViewTheme(GetDlgItem(dialog, IDC_BITFIELD_LIST));
    }
    return themed;
  }
  if (message != WM_COMMAND || !editor) {
    return FALSE;
  }
  const int id = LOWORD(wparam);
  if (dialog_support::HandleGridToggle(dialog, id)) {
    return TRUE;
  }
  if (id == IDC_BITFIELD_DEFINITION) {
    const HWND combo = GetDlgItem(dialog, IDC_BITFIELD_DEFINITION);
    if (HIWORD(wparam) == CBN_EDITCHANGE) {
      if (editor->updating_combo) {
        return TRUE;
      }
      editor->filtering = true;
      RefreshCombo(dialog, editor);
      return TRUE;
    }
    if (HIWORD(wparam) == CBN_SELCHANGE) {
      const int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
      editor->filtering = false;
      if (index >= 0) {
        ApplyChoice(dialog, editor, static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0)));
      }
      return TRUE;
    }
    if (HIWORD(wparam) == CBN_KILLFOCUS && editor->filtering) {
      editor->filtering = false;
      RefreshCombo(dialog, editor);
      return TRUE;
    }
    return FALSE;
  }
  switch (id) {
  case IDC_BITFIELD_LOAD:
    LoadFromFile(dialog, editor);
    return TRUE;
  case IDC_BITFIELD_NEW:
    EditOrCreate(dialog, editor, true);
    return TRUE;
  case IDC_BITFIELD_EDIT_DEF:
    EditOrCreate(dialog, editor, false);
    return TRUE;
  case IDC_BITFIELD_SAVE_AS:
    SaveCurrent(dialog, editor);
    return TRUE;
  case IDC_BITFIELD_SELECT_ALL:
    SetAllBits(dialog, editor, true);
    return TRUE;
  case IDC_BITFIELD_CLEAR_ALL:
    SetAllBits(dialog, editor, false);
    return TRUE;
  case IDOK:
    if (!editor->read_only) {
      WriteWindow(editor);
      editor->accepted = true;
      EndDialog(dialog, IDOK);
    }
    return TRUE;
  case IDCANCEL:
    EndDialog(dialog, IDCANCEL);
    return TRUE;
  default:
    return FALSE;
  }
}

} // namespace

bool EditBitfield(
    HWND owner,
    const BitfieldRequest& request,
    BitfieldResult* result
) {
  if (!result) {
    return false;
  }
  Editor editor;
  editor.value_name = request.value_name;
  editor.key_path = request.key_path;
  editor.read_only = request.read_only;
  editor.binary_mode = !request.data.empty();
  if (editor.binary_mode) {
    editor.bytes.assign(request.data.begin(), request.data.end());
    const size_t usable = std::min<size_t>(editor.bytes.size(), 4);
    editor.width = usable >= 4 ? 32 : (usable >= 2 ? 16 : 8);
    editor.value = ReadWindow(editor);
  } else {
    if (!bitfield::ValidWidth(request.bit_count)) {
      return false;
    }
    editor.width = request.bit_count;
    editor.value = request.value & bitfield::WidthMask(editor.width);
    editor.bytes.resize(editor.width / 8);
    WriteWindow(&editor);
  }

  const INT_PTR outcome = DialogBoxParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(IDD_BITFIELD),
      owner,
      DialogProc,
      reinterpret_cast<LPARAM>(&editor)
  );
  if (outcome != IDOK || !editor.accepted) {
    return false;
  }
  result->value = editor.value;
  result->data = std::move(editor.bytes);
  return true;
}

} // namespace regkit::editors
