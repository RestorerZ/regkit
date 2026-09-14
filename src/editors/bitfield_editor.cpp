// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "editors/bitfield_editor.h"

#include "appearance/dialog_layout.h"
#include "appearance/feedback.h"
#include "appearance/theme.h"
#include "editors/bitfield_definition_editor.h"
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

constexpr int kColumnMask = 1;
constexpr int kColumnValue = 2;
constexpr int kColumnField = 3;
constexpr int kColumnMeaning = 4;

struct State {
  std::wstring value_name;
  std::vector<BYTE> bytes;
  bool binary_mode = false;
  bool read_only = false;
  unsigned width = 32;
  unsigned offset = 0;
  uint64_t value = 0;
  bool updating = false;
  bool accepted = false;
  std::vector<Definition> choices;
  int choice = 0;
  HFONT ui_font = nullptr;
  appearance::DialogResizer resizer;

  const Definition& definition() const { return choices[static_cast<size_t>(choice)]; }
};

uint64_t ReadWindow(
    const State& state
) {
  uint64_t value = 0;
  const size_t count = state.width / 8;
  if (state.offset + count > state.bytes.size()) {
    return 0;
  }
  for (size_t i = 0; i < count; ++i) {
    value |= static_cast<uint64_t>(state.bytes[state.offset + i]) << (8 * i);
  }
  return value;
}

void WriteWindow(
    State* state
) {
  const size_t count = state->width / 8;
  if (state->offset + count > state->bytes.size()) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    state->bytes[state->offset + i] = static_cast<BYTE>((state->value >> (8 * i)) & 0xFF);
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

std::wstring DisplayName(
    const Definition& definition
) {
  if (!definition.name.empty()) {
    return definition.name;
  }
  if (!definition.path.empty()) {
    const size_t slash = definition.path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? definition.path : definition.path.substr(slash + 1);
  }
  return L"Unnamed definition";
}

bool FitsWindow(
    const State& state,
    const Definition& definition
) {
  if (!state.binary_mode) {
    return definition.bit_width == state.width && definition.byte_offset == 0;
  }
  return static_cast<size_t>(definition.byte_offset) + definition.bit_width / 8 <= state.bytes.size();
}

void UpdateValueText(
    HWND dialog,
    const State& state
) {
  std::wstring text = MaskText(state.width, state.value);
  if (state.binary_mode) {
    text.append(L"  @ ").append(MaskText(16, state.offset));
  }
  SetDlgItemTextW(dialog, IDC_VALUE_BYTES, text.c_str());
}

void UpdateRow(
    HWND list,
    const State& state,
    int row
) {
  const unsigned bit = state.width - 1 - static_cast<unsigned>(row);
  const bool set = (state.value >> bit) & 1ull;
  ListView_SetItemText(list, row, kColumnValue, const_cast<wchar_t*>(set ? L"1" : L"0"));
}

void ShowDetail(
    HWND dialog,
    const State& state,
    int row
) {
  if (row < 0) {
    SetDlgItemTextW(dialog, IDC_BITFIELD_DETAIL, L"");
    return;
  }
  const unsigned bit = state.width - 1 - static_cast<unsigned>(row);
  std::wstring text = L"Bit " + std::to_wstring(bit) + L"   " + MaskText(state.width, 1ull << bit);
  if (const Field* field = state.definition().FieldForBit(bit)) {
    text.append(L"\r\n").append(field->name).append(L"   bits ");
    for (size_t i = 0; i < field->bits.size(); ++i) {
      if (i > 0) {
        text.append(L", ");
      }
      text.append(std::to_wstring(field->bits[i]));
    }
    if (!field->meaning.empty()) {
      text.append(L"\r\n").append(field->meaning);
    }
  } else {
    text.append(L"\r\nNot described by the selected definition.");
  }
  SetDlgItemTextW(dialog, IDC_BITFIELD_DETAIL, text.c_str());
}

void FillList(
    HWND dialog,
    State* state
) {
  const HWND list = GetDlgItem(dialog, IDC_BITFIELD_LIST);
  state->updating = true;
  ListView_DeleteAllItems(list);
  const Definition& definition = state->definition();
  for (unsigned row = 0; row < state->width; ++row) {
    const unsigned bit = state->width - 1 - row;
    const std::wstring bit_text = std::to_wstring(bit);
    LVITEMW item = {};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(row);
    item.pszText = const_cast<wchar_t*>(bit_text.c_str());
    ListView_InsertItem(list, &item);
    const std::wstring mask = MaskText(state->width, 1ull << bit);
    ListView_SetItemText(list, static_cast<int>(row), kColumnMask, const_cast<wchar_t*>(mask.c_str()));
    const bool set = (state->value >> bit) & 1ull;
    ListView_SetItemText(list, static_cast<int>(row), kColumnValue, const_cast<wchar_t*>(set ? L"1" : L"0"));
    const Field* field = definition.FieldForBit(bit);
    if (field) {
      ListView_SetItemText(list, static_cast<int>(row), kColumnField, const_cast<wchar_t*>(field->name.c_str()));
      ListView_SetItemText(list, static_cast<int>(row), kColumnMeaning, const_cast<wchar_t*>(field->meaning.c_str()));
    }
    if (!state->read_only) {
      ListView_SetCheckState(list, static_cast<int>(row), set);
    }
  }
  state->updating = false;
  UpdateValueText(dialog, *state);
  ShowDetail(dialog, *state, -1);
}

void ApplyChoice(
    HWND dialog,
    State* state,
    int choice
) {
  if (choice < 0 || static_cast<size_t>(choice) >= state->choices.size()) {
    return;
  }
  if (state->binary_mode) {
    WriteWindow(state);
  }
  state->choice = choice;
  const Definition& definition = state->definition();
  if (state->binary_mode && !definition.fields.empty()) {
    state->width = definition.bit_width;
    state->offset = definition.byte_offset;
    state->value = ReadWindow(*state);
  }
  SetDlgItemTextW(dialog, IDC_BITFIELD_COMMENT, definition.comment.c_str());
  SendDlgItemMessageW(dialog, IDC_BITFIELD_DEFINITION, CB_SETCURSEL, static_cast<WPARAM>(choice), 0);
  EnableWindow(GetDlgItem(dialog, IDC_BITFIELD_EDIT_DEF), choice != 0);
  EnableWindow(GetDlgItem(dialog, IDC_BITFIELD_SAVE_AS), choice != 0);
  FillList(dialog, state);
}

void AddChoice(
    HWND dialog,
    State* state,
    Definition definition
) {
  const std::wstring label = DisplayName(definition);
  state->choices.push_back(std::move(definition));
  SendDlgItemMessageW(dialog, IDC_BITFIELD_DEFINITION, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
  ApplyChoice(dialog, state, static_cast<int>(state->choices.size()) - 1);
}

bool AcceptDefinition(
    HWND dialog,
    const State& state,
    const Definition& definition
) {
  if (!FitsWindow(state, definition)) {
    ui::ShowError(
        dialog,
        state.binary_mode
            ? L"The definition describes bytes outside this value."
            : L"The definition was made for a different value width."
    );
    return false;
  }
  if (!definition.value_name.empty() &&
      _wcsicmp(definition.value_name.c_str(), state.value_name.c_str()) != 0) {
    std::wstring message = L"This definition was made for a different value.\r\n\r\nDefinition: ";
    message.append(definition.value_name).append(L"\r\nThis value: ").append(state.value_name.empty() ? L"(Default)" : state.value_name);
    message.append(L"\r\n\r\nUse it anyway?");
    if (ui::PromptChoice(dialog, message, L"Bit Definition", L"Use", L"Cancel", L"") != IDYES) {
      return false;
    }
  }
  return true;
}

void LoadFromFile(
    HWND dialog,
    State* state
) {
  std::wstring path;
  const HRESULT hr = win32::ChooseFileToOpen(dialog, bitfield::FileFilter(), &path);
  if (!ui::ReportFileDialogResult(dialog, hr)) {
    return;
  }
  Definition definition;
  std::wstring error;
  if (!bitfield::Load(path, &definition, &error)) {
    ui::ShowError(dialog, error);
    return;
  }
  if (!AcceptDefinition(dialog, *state, definition)) {
    return;
  }
  AddChoice(dialog, state, std::move(definition));
}

void SaveCurrent(
    HWND dialog,
    State* state
) {
  const Definition& definition = state->definition();
  std::wstring path;
  const HRESULT hr = win32::ChooseFileToSave(
      dialog,
      bitfield::FileFilter(),
      bitfield::FileExtension(),
      bitfield::SuggestedFileName(definition.value_name.empty() ? state->value_name : definition.value_name).c_str(),
      &path
  );
  if (!ui::ReportFileDialogResult(dialog, hr)) {
    return;
  }
  std::wstring error;
  if (!bitfield::Save(path, definition, &error)) {
    ui::ShowError(dialog, error);
    return;
  }
  state->choices[static_cast<size_t>(state->choice)].path = path;
}

void EditOrCreate(
    HWND dialog,
    State* state,
    bool create
) {
  Definition definition;
  if (create) {
    definition.value_name = state->value_name;
    definition.bit_width = state->width;
    definition.byte_offset = state->offset;
  } else {
    definition = state->definition();
  }
  if (!EditBitfieldDefinition(dialog, &definition, !state->binary_mode)) {
    return;
  }
  if (!AcceptDefinition(dialog, *state, definition)) {
    return;
  }
  if (create) {
    AddChoice(dialog, state, std::move(definition));
    return;
  }
  const std::wstring label = DisplayName(definition);
  state->choices[static_cast<size_t>(state->choice)] = std::move(definition);
  SendDlgItemMessageW(dialog, IDC_BITFIELD_DEFINITION, CB_DELETESTRING, static_cast<WPARAM>(state->choice), 0);
  SendDlgItemMessageW(dialog, IDC_BITFIELD_DEFINITION, CB_INSERTSTRING, static_cast<WPARAM>(state->choice), reinterpret_cast<LPARAM>(label.c_str()));
  ApplyChoice(dialog, state, state->choice);
}

void SetAllBits(
    HWND dialog,
    State* state,
    bool set
) {
  if (state->read_only) {
    return;
  }
  state->value = set ? bitfield::WidthMask(state->width) : 0;
  const HWND list = GetDlgItem(dialog, IDC_BITFIELD_LIST);
  state->updating = true;
  for (unsigned row = 0; row < state->width; ++row) {
    ListView_SetCheckState(list, static_cast<int>(row), set);
    UpdateRow(list, *state, static_cast<int>(row));
  }
  state->updating = false;
  UpdateValueText(dialog, *state);
}

void SetupList(
    HWND dialog,
    const State& state
) {
  const HWND list = GetDlgItem(dialog, IDC_BITFIELD_LIST);
  DWORD extended = LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER;
  if (!state.read_only) {
    extended |= LVS_EX_CHECKBOXES;
  }
  ListView_SetExtendedListViewStyle(list, extended);
  Theme::Current().ApplyToListView(list);
  const struct {
    const wchar_t* title;
    int width;
  } columns[] = {
      {L"Bit", 44},
      {L"Mask", 150},
      {L"Value", 52},
      {L"Field", 160},
      {L"Meaning", 420},
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

void HandleItemChanged(
    HWND dialog,
    State* state,
    const NMLISTVIEW* info
) {
  if (state->updating || !(info->uChanged & LVIF_STATE)) {
    return;
  }
  if ((info->uNewState & LVIS_SELECTED) && !(info->uOldState & LVIS_SELECTED)) {
    ShowDetail(dialog, *state, info->iItem);
  }
  const UINT old_image = info->uOldState & LVIS_STATEIMAGEMASK;
  const UINT new_image = info->uNewState & LVIS_STATEIMAGEMASK;
  if (old_image == 0 || new_image == 0 || old_image == new_image) {
    return;
  }
  if (info->iItem < 0 || static_cast<unsigned>(info->iItem) >= state->width) {
    return;
  }
  const unsigned bit = state->width - 1 - static_cast<unsigned>(info->iItem);
  const bool set = new_image == INDEXTOSTATEIMAGEMASK(2);
  if (set) {
    state->value |= 1ull << bit;
  } else {
    state->value &= ~(1ull << bit);
  }
  UpdateRow(GetDlgItem(dialog, IDC_BITFIELD_LIST), *state, info->iItem);
  UpdateValueText(dialog, *state);
  ShowDetail(dialog, *state, info->iItem);
}

void PopulateChoices(
    HWND dialog,
    State* state
) {
  Definition none;
  none.owner.fill(-1);
  state->choices.push_back(std::move(none));
  SendDlgItemMessageW(dialog, IDC_BITFIELD_DEFINITION, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(none)"));
  for (Definition& definition : bitfield::BundledDefinitions()) {
    if (!FitsWindow(*state, definition) ||
        _wcsicmp(definition.value_name.c_str(), state->value_name.c_str()) != 0) {
      continue;
    }
    const std::wstring label = DisplayName(definition);
    state->choices.push_back(std::move(definition));
    SendDlgItemMessageW(dialog, IDC_BITFIELD_DEFINITION, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
  }
}

INT_PTR CALLBACK DialogProc(
    HWND dialog,
    UINT message,
    WPARAM wparam,
    LPARAM lparam
) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<State*>(lparam);
    SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    SetWindowTextW(dialog, L"Edit Bits");
    SetDlgItemTextW(dialog, IDC_VALUE_NAME, state->value_name.empty() ? L"(Default)" : state->value_name.c_str());
    SendDlgItemMessageW(dialog, IDC_VALUE_NAME, EM_SETREADONLY, TRUE, 0);
    SendDlgItemMessageW(dialog, IDC_BITFIELD_COMMENT, EM_SETREADONLY, TRUE, 0);
    SendDlgItemMessageW(dialog, IDC_BITFIELD_DETAIL, EM_SETREADONLY, TRUE, 0);
    const HWND name = GetDlgItem(dialog, IDC_VALUE_NAME);
    SetWindowLongPtrW(name, GWL_STYLE, GetWindowLongPtrW(name, GWL_STYLE) & ~WS_TABSTOP);
    if (state->read_only) {
      EnableWindow(GetDlgItem(dialog, IDC_BITFIELD_SELECT_ALL), FALSE);
      EnableWindow(GetDlgItem(dialog, IDC_BITFIELD_CLEAR_ALL), FALSE);
      EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
    }
    SetupList(dialog, *state);
    PopulateChoices(dialog, state);
    dialog_support::Initialize(
        dialog,
        &state->ui_font,
        {IDC_VALUE_NAME, IDC_BITFIELD_COMMENT, IDC_BITFIELD_DETAIL}
    );
    using namespace appearance;
    state->resizer.Attach(dialog, {
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
                                      {IDC_BITFIELD_DETAIL_LABEL, kAnchorLeft | kAnchorRight | kAnchorBottom},
                                      {IDC_BITFIELD_DETAIL, kAnchorLeft | kAnchorRight | kAnchorBottom},
                                      {IDC_BITFIELD_SELECT_ALL, kAnchorLeft | kAnchorBottom},
                                      {IDC_BITFIELD_CLEAR_ALL, kAnchorLeft | kAnchorBottom},
                                      {IDOK, kAnchorRight | kAnchorBottom},
                                      {IDCANCEL, kAnchorRight | kAnchorBottom},
                                  });
    ApplyChoice(dialog, state, state->choices.size() > 1 ? 1 : 0);
    SetFocus(GetDlgItem(dialog, IDC_BITFIELD_LIST));
    return FALSE;
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
    if (header->idFrom == IDC_BITFIELD_LIST) {
      if (header->code == LVN_ITEMCHANGED) {
        HandleItemChanged(dialog, state, reinterpret_cast<NMLISTVIEW*>(lparam));
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
  if (HIWORD(wparam) == CBN_SELCHANGE && id == IDC_BITFIELD_DEFINITION) {
    ApplyChoice(dialog, state, static_cast<int>(SendDlgItemMessageW(dialog, IDC_BITFIELD_DEFINITION, CB_GETCURSEL, 0, 0)));
    return TRUE;
  }
  switch (id) {
  case IDC_BITFIELD_LOAD:
    LoadFromFile(dialog, state);
    return TRUE;
  case IDC_BITFIELD_NEW:
    EditOrCreate(dialog, state, true);
    return TRUE;
  case IDC_BITFIELD_EDIT_DEF:
    EditOrCreate(dialog, state, false);
    return TRUE;
  case IDC_BITFIELD_SAVE_AS:
    SaveCurrent(dialog, state);
    return TRUE;
  case IDC_BITFIELD_SELECT_ALL:
    SetAllBits(dialog, state, true);
    return TRUE;
  case IDC_BITFIELD_CLEAR_ALL:
    SetAllBits(dialog, state, false);
    return TRUE;
  case IDOK:
    if (!state->read_only) {
      WriteWindow(state);
      state->accepted = true;
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
  State state;
  state.value_name = request.value_name;
  state.read_only = request.read_only;
  state.binary_mode = !request.data.empty();
  if (state.binary_mode) {
    state.bytes.assign(request.data.begin(), request.data.end());
    const size_t usable = std::min<size_t>(state.bytes.size(), 4);
    state.width = usable >= 4 ? 32 : (usable >= 2 ? 16 : 8);
    state.value = ReadWindow(state);
  } else {
    if (!bitfield::ValidWidth(request.bit_count)) {
      return false;
    }
    state.width = request.bit_count;
    state.value = request.value & bitfield::WidthMask(state.width);
    state.bytes.resize(state.width / 8);
    WriteWindow(&state);
  }

  const INT_PTR outcome = DialogBoxParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(IDD_BITFIELD),
      owner,
      DialogProc,
      reinterpret_cast<LPARAM>(&state)
  );
  if (outcome != IDOK || !state.accepted) {
    return false;
  }
  result->value = state.value;
  result->data = std::move(state.bytes);
  return true;
}

} // namespace regkit::editors
