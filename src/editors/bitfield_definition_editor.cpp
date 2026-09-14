// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "editors/bitfield_definition_editor.h"

#include "appearance/dialog_layout.h"
#include "appearance/feedback.h"
#include "appearance/theme.h"
#include "editors/dialog_support.h"
#include "win32/file_dialog.h"
#include "win32/window_metrics.h"

#include "resource.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace regkit::editors {

namespace {

using bitfield::Definition;
using bitfield::Field;

constexpr unsigned kWidths[] = {8, 16, 32, 64};

struct FieldState {
  Field field;
  const Definition* parent = nullptr;
  int editing = -1;
  std::vector<unsigned> bits;
  bool accepted = false;
  HFONT ui_font = nullptr;
};

struct DefinitionState {
  Definition working;
  bool lock_width = false;
  bool dirty = false;
  bool saved = false;
  bool accepted = false;
  HFONT ui_font = nullptr;
  appearance::DialogResizer resizer;
};

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

INT_PTR CALLBACK FieldDialogProc(
    HWND dialog,
    UINT message,
    WPARAM wparam,
    LPARAM lparam
) {
  auto* state = reinterpret_cast<FieldState*>(GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<FieldState*>(lparam);
    SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    SetWindowTextW(dialog, state->editing < 0 ? L"Add Field" : L"Edit Field");
    SetDlgItemTextW(dialog, IDC_FIELD_NAME, state->field.name.c_str());
    SetDlgItemTextW(dialog, IDC_FIELD_MEANING, state->field.meaning.c_str());
    const HWND list = GetDlgItem(dialog, IDC_FIELD_BITS);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    Theme::Current().ApplyToListView(list);
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<wchar_t*>(L"Bit");
    column.cx = MulDiv(160, static_cast<int>(win32::DpiForWindow(dialog)), 96);
    ListView_InsertColumn(list, 0, &column);
    ListView_SetColumnWidth(list, 0, LVSCW_AUTOSIZE_USEHEADER);
    for (unsigned bit = 0; bit < state->parent->bit_width; ++bit) {
      const signed char owner = state->parent->owner[bit];
      if (owner >= 0 && owner != state->editing) {
        continue;
      }
      const std::wstring text = std::to_wstring(bit);
      LVITEMW item = {};
      item.mask = LVIF_TEXT;
      item.iItem = static_cast<int>(state->bits.size());
      item.pszText = const_cast<wchar_t*>(text.c_str());
      ListView_InsertItem(list, &item);
      const bool checked = std::find(state->field.bits.begin(), state->field.bits.end(), bit) != state->field.bits.end();
      ListView_SetCheckState(list, item.iItem, checked);
      state->bits.push_back(bit);
    }
    dialog_support::Initialize(dialog, &state->ui_font, {IDC_FIELD_NAME, IDC_FIELD_MEANING});
    return TRUE;
  }
  if (message == WM_DESTROY) {
    if (state) {
      dialog_support::ReleaseFont(&state->ui_font);
    }
    return TRUE;
  }
  if (message == WM_NOTIFY && state) {
    auto* header = reinterpret_cast<NMHDR*>(lparam);
    if (header->idFrom == IDC_FIELD_BITS && header->code == NM_CUSTOMDRAW) {
      SetWindowLongPtrW(dialog, DWLP_MSGRESULT, ui::HandleThemedListViewCustomDraw(header->hwndFrom, reinterpret_cast<NMLVCUSTOMDRAW*>(lparam)));
      return TRUE;
    }
  }
  INT_PTR themed = 0;
  if (dialog_support::HandleThemeMessage(dialog, message, wparam, lparam, &themed)) {
    return themed;
  }
  if (message != WM_COMMAND || !state) {
    return FALSE;
  }
  switch (LOWORD(wparam)) {
  case IDOK:
    {
      Field result;
      result.name = dialog_support::ReadText(dialog, IDC_FIELD_NAME);
      result.meaning = dialog_support::ReadText(dialog, IDC_FIELD_MEANING);
      if (result.name.empty()) {
        ui::ShowError(dialog, L"Enter a field name.");
        return TRUE;
      }
      const HWND list = GetDlgItem(dialog, IDC_FIELD_BITS);
      for (size_t row = 0; row < state->bits.size(); ++row) {
        if (ListView_GetCheckState(list, static_cast<int>(row))) {
          result.bits.push_back(state->bits[row]);
        }
      }
      if (result.bits.empty()) {
        ui::ShowError(dialog, L"Select at least one bit.");
        return TRUE;
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

bool RunFieldDialog(
    HWND owner,
    const Definition& parent,
    int editing,
    Field* field
) {
  FieldState state;
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

void RefreshFieldList(
    HWND dialog,
    const Definition& definition
) {
  const HWND list = GetDlgItem(dialog, IDC_DEF_LIST);
  ListView_DeleteAllItems(list);
  for (size_t i = 0; i < definition.fields.size(); ++i) {
    const Field& field = definition.fields[i];
    LVITEMW item = {};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(i);
    item.pszText = const_cast<wchar_t*>(field.name.c_str());
    ListView_InsertItem(list, &item);
    const std::wstring bits = BitsText(field);
    ListView_SetItemText(list, item.iItem, 1, const_cast<wchar_t*>(bits.c_str()));
    ListView_SetItemText(list, item.iItem, 2, const_cast<wchar_t*>(field.meaning.c_str()));
  }
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

bool CollectDefinition(
    HWND dialog,
    DefinitionState* state,
    Definition* out
) {
  Definition draft = state->working;
  draft.name = dialog_support::ReadText(dialog, IDC_DEF_NAME);
  draft.value_name = dialog_support::ReadText(dialog, IDC_DEF_VALUE_NAME);
  draft.comment = dialog_support::ReadText(dialog, IDC_DEF_COMMENT);
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

bool SaveToFile(
    HWND owner,
    Definition* draft
) {
  std::wstring path;
  const HRESULT hr = win32::ChooseFileToSave(
      owner,
      bitfield::FileFilter(),
      bitfield::FileExtension(),
      bitfield::SuggestedFileName(draft->value_name).c_str(),
      &path
  );
  if (!ui::ReportFileDialogResult(owner, hr)) {
    return false;
  }
  std::wstring error;
  if (!bitfield::Save(path, *draft, &error)) {
    ui::ShowError(owner, error);
    return false;
  }
  draft->path = path;
  return true;
}

bool SaveDefinition(
    HWND dialog,
    DefinitionState* state
) {
  Definition draft;
  if (!CollectDefinition(dialog, state, &draft)) {
    return false;
  }
  if (!SaveToFile(dialog, &draft)) {
    return false;
  }
  state->working = std::move(draft);
  state->dirty = false;
  state->saved = true;
  return true;
}

bool DiscardChanges(
    HWND dialog,
    DefinitionState* state
) {
  if (!state->dirty) {
    return true;
  }
  const int choice = ui::PromptChoice(
      dialog,
      L"This definition has unsaved changes.",
      L"Bit Definition",
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
    DefinitionState* state
) {
  if (!DiscardChanges(dialog, state)) {
    return;
  }
  std::wstring path;
  const HRESULT hr = win32::ChooseFileToOpen(dialog, bitfield::FileFilter(), &path);
  if (!ui::ReportFileDialogResult(dialog, hr)) {
    return;
  }
  Definition loaded;
  std::wstring error;
  if (!bitfield::Load(path, &loaded, &error)) {
    ui::ShowError(dialog, error);
    return;
  }
  if (state->lock_width && loaded.bit_width != state->working.bit_width) {
    ui::ShowError(dialog, L"The definition was made for a different value width.");
    return;
  }
  state->working = std::move(loaded);
  state->dirty = false;
  state->saved = true;
  SetDlgItemTextW(dialog, IDC_DEF_NAME, state->working.name.c_str());
  SetDlgItemTextW(dialog, IDC_DEF_VALUE_NAME, state->working.value_name.c_str());
  SetDlgItemTextW(dialog, IDC_DEF_COMMENT, state->working.comment.c_str());
  SetDlgItemInt(dialog, IDC_DEF_OFFSET, state->working.byte_offset, FALSE);
  SelectWidth(dialog, state->working.bit_width);
  RefreshFieldList(dialog, state->working);
}

void ChangeWidth(
    HWND dialog,
    DefinitionState* state
) {
  const unsigned width = SelectedWidth(dialog);
  bool affected = false;
  for (const Field& field : state->working.fields) {
    if (!field.bits.empty() && field.bits.back() >= width) {
      affected = true;
      break;
    }
  }
  if (affected) {
    const int choice = ui::PromptChoice(
        dialog,
        L"Some fields use bits outside the new width. Remove those bits?",
        L"Bit Definition",
        L"Remove",
        L"Cancel",
        L"",
        {80, 70, 70}
    );
    if (choice != IDYES) {
      SelectWidth(dialog, state->working.bit_width);
      return;
    }
    for (Field& field : state->working.fields) {
      field.bits.erase(
          std::remove_if(field.bits.begin(), field.bits.end(), [width](unsigned bit) { return bit >= width; }),
          field.bits.end()
      );
    }
    state->working.fields.erase(
        std::remove_if(
            state->working.fields.begin(),
            state->working.fields.end(),
            [](const Field& field) { return field.bits.empty(); }
        ),
        state->working.fields.end()
    );
    RefreshFieldList(dialog, state->working);
  }
  state->working.bit_width = width;
  state->working.owner.fill(-1);
  for (size_t i = 0; i < state->working.fields.size(); ++i) {
    for (const unsigned bit : state->working.fields[i].bits) {
      state->working.owner[bit] = static_cast<signed char>(i);
    }
  }
  state->dirty = true;
}

void AddOrEditField(
    HWND dialog,
    DefinitionState* state,
    bool create
) {
  const HWND list = GetDlgItem(dialog, IDC_DEF_LIST);
  const int selected = create ? -1 : ListView_GetNextItem(list, -1, LVNI_SELECTED);
  if (!create && selected < 0) {
    return;
  }
  Field field;
  if (!create) {
    field = state->working.fields[static_cast<size_t>(selected)];
  }
  if (!RunFieldDialog(dialog, state->working, selected, &field)) {
    return;
  }
  Definition draft = state->working;
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
  state->working = std::move(draft);
  state->dirty = true;
  RefreshFieldList(dialog, state->working);
}

void RemoveField(
    HWND dialog,
    DefinitionState* state
) {
  const HWND list = GetDlgItem(dialog, IDC_DEF_LIST);
  const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
  if (selected < 0) {
    return;
  }
  state->working.fields.erase(state->working.fields.begin() + selected);
  state->working.owner.fill(-1);
  for (size_t i = 0; i < state->working.fields.size(); ++i) {
    for (const unsigned bit : state->working.fields[i].bits) {
      state->working.owner[bit] = static_cast<signed char>(i);
    }
  }
  state->dirty = true;
  RefreshFieldList(dialog, state->working);
}

void SetupDefinitionList(
    HWND dialog
) {
  const HWND list = GetDlgItem(dialog, IDC_DEF_LIST);
  ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  Theme::Current().ApplyToListView(list);
  const struct {
    const wchar_t* title;
    int width;
  } columns[] = {
      {L"Field", 160},
      {L"Bits", 110},
      {L"Meaning", 360},
  };
  const UINT dpi = win32::DpiForWindow(dialog);
  for (int i = 0; i < static_cast<int>(_countof(columns)); ++i) {
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<wchar_t*>(columns[i].title);
    column.cx = MulDiv(columns[i].width, static_cast<int>(dpi), 96);
    ListView_InsertColumn(list, i, &column);
  }
}

INT_PTR CALLBACK DefinitionDialogProc(
    HWND dialog,
    UINT message,
    WPARAM wparam,
    LPARAM lparam
) {
  auto* state = reinterpret_cast<DefinitionState*>(GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<DefinitionState*>(lparam);
    SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    SetWindowTextW(dialog, L"Bit Definition");
    SetDlgItemTextW(dialog, IDC_DEF_NAME, state->working.name.c_str());
    SetDlgItemTextW(dialog, IDC_DEF_VALUE_NAME, state->working.value_name.c_str());
    SetDlgItemTextW(dialog, IDC_DEF_COMMENT, state->working.comment.c_str());
    SetDlgItemInt(dialog, IDC_DEF_OFFSET, state->working.byte_offset, FALSE);
    for (const unsigned width : kWidths) {
      SendDlgItemMessageW(dialog, IDC_DEF_WIDTH, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(width).c_str()));
    }
    SelectWidth(dialog, state->working.bit_width);
    EnableWindow(GetDlgItem(dialog, IDC_DEF_WIDTH), !state->lock_width);
    SetupDefinitionList(dialog);
    RefreshFieldList(dialog, state->working);
    dialog_support::Initialize(
        dialog,
        &state->ui_font,
        {IDC_DEF_NAME, IDC_DEF_VALUE_NAME, IDC_DEF_OFFSET, IDC_DEF_COMMENT}
    );
    using namespace appearance;
    state->resizer.Attach(dialog, {
                                      {IDC_DEF_NAME, kAnchorLeft | kAnchorTop | kAnchorRight},
                                      {IDC_DEF_VALUE_NAME, kAnchorLeft | kAnchorTop | kAnchorRight},
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
    return TRUE;
  }
  if (message == WM_SIZE && state) {
    state->resizer.Apply(dialog);
    return TRUE;
  }
  if (message == WM_GETMINMAXINFO && state) {
    state->resizer.ClampMinSize(reinterpret_cast<MINMAXINFO*>(lparam));
    return TRUE;
  }
  if (message == WM_NOTIFY && state) {
    auto* header = reinterpret_cast<NMHDR*>(lparam);
    if (header->idFrom == IDC_DEF_LIST) {
      if (header->code == NM_DBLCLK) {
        AddOrEditField(dialog, state, false);
        return TRUE;
      }
      if (header->code == NM_CUSTOMDRAW) {
        SetWindowLongPtrW(dialog, DWLP_MSGRESULT, ui::HandleThemedListViewCustomDraw(header->hwndFrom, reinterpret_cast<NMLVCUSTOMDRAW*>(lparam)));
        return TRUE;
      }
    }
  }
  INT_PTR themed = 0;
  if (dialog_support::HandleThemeMessage(dialog, message, wparam, lparam, &themed)) {
    return themed;
  }
  if (message != WM_COMMAND || !state) {
    return FALSE;
  }
  const int id = LOWORD(wparam);
  const int code = HIWORD(wparam);
  if (code == EN_CHANGE) {
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
  case IDC_DEF_OPEN:
    LoadIntoEditor(dialog, state);
    return TRUE;
  case IDC_DEF_SAVE_AS:
    SaveDefinition(dialog, state);
    return TRUE;
  case IDOK:
    {
      Definition draft;
      if (!CollectDefinition(dialog, state, &draft)) {
        return TRUE;
      }
      state->working = std::move(draft);
      state->accepted = true;
      EndDialog(dialog, IDOK);
      return TRUE;
    }
  case IDCANCEL:
    if (DiscardChanges(dialog, state)) {
      EndDialog(dialog, IDCANCEL);
    }
    return TRUE;
  default:
    return FALSE;
  }
}

} // namespace

bool EditBitfieldDefinition(
    HWND owner,
    Definition* definition,
    bool lock_width
) {
  if (!definition) {
    return false;
  }
  DefinitionState state;
  state.working = *definition;
  state.lock_width = lock_width;
  if (!bitfield::ValidWidth(state.working.bit_width)) {
    state.working.bit_width = 32;
  }
  state.working.owner.fill(-1);
  for (size_t i = 0; i < state.working.fields.size(); ++i) {
    for (const unsigned bit : state.working.fields[i].bits) {
      if (bit < 64) {
        state.working.owner[bit] = static_cast<signed char>(i);
      }
    }
  }
  const INT_PTR outcome = DialogBoxParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(IDD_BITFIELD_DEFINITION),
      owner,
      DefinitionDialogProc,
      reinterpret_cast<LPARAM>(&state)
  );
  if (outcome != IDOK || !state.accepted) {
    return false;
  }
  *definition = std::move(state.working);
  return true;
}

void ShowBitfieldDefinitionEditor(
    HWND owner
) {
  Definition definition;
  definition.owner.fill(-1);
  DefinitionState state;
  state.working = std::move(definition);
  const INT_PTR outcome = DialogBoxParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(IDD_BITFIELD_DEFINITION),
      owner,
      DefinitionDialogProc,
      reinterpret_cast<LPARAM>(&state)
  );
  if (outcome == IDOK && state.accepted && !state.saved) {
    SaveToFile(owner, &state.working);
  }
}

} // namespace regkit::editors
