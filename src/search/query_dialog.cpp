// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search/query_dialog.h"

#include "win32/text_transform.h"
#include "search/query_prompts.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <vector>

#include <commctrl.h>
#include <windowsx.h>

#include "appearance/dialog_layout.h"
#include "appearance/dialog_metrics.h"
#include "win32/window_metrics.h"
#include "appearance/feedback.h"
#include "registry/registry_store.h"
#include "editors/value_editor.h"
#include "records/escaped_fields.h"
#include "win32/file_text.h"
#include "win32/shell_paths.h"

namespace regkit {

namespace {

constexpr wchar_t kDialogClass[] = L"RegKitSearchDialog";

enum ControlId {
  kFindLabel = 100,
  kFindCombo = 101,
  kWhereGroup = 110,
  kScopeTop = 111,
  kScopeKey = 112,
  kScopeRecursive = 113,
  kScopeCombo = 114,
  kScopeEdit = 116,
  kScopeBrowse = 117,
  kOptionsGroup = 120,
  kOptKeys = 121,
  kOptValues = 122,
  kOptData = 123,
  kOptDataTypes = 124,
  kOptMatchCase = 125,
  kOptMatchWhole = 126,
  kOptUseRegex = 127,
  kOptSkipLinks = 128,
  kOptMinSize = 129,
  kOptMinSizeEdit = 130,
  kOptMaxSize = 131,
  kOptMaxSizeEdit = 132,
  kOptStandardHives = 133,
  kOptRegistryRoot = 134,
  kOptTraceValues = 135,
  kOptOfflineHives = 136,
  kOptRegFiles = 137,
  kOptRemoteRegistry = 138,
  kModifiedLabel = 140,
  kModifiedFrom = 141,
  kModifiedDash = 142,
  kModifiedTo = 143,
  kExcludeGroup = 150,
  kExcludeEnable = 151,
  kExcludeEdit = 152,
  kExcludeButton = 153,
  kResultGroup = 160,
  kResultReuse = 161,
  kResultNew = 162,
  kResultOpenNewTab = 163,
  kResultLimitEnable = 164,
  kResultLimitEdit = 165,
  kFindButton = IDOK,
  kCancelButton = IDCANCEL,
};

struct SearchDialogState : appearance::DialogWindow {
  HWND find_combo = nullptr;
  HWND scope_top = nullptr;
  HWND scope_key = nullptr;
  HWND scope_recursive = nullptr;
  HWND scope_combo = nullptr;
  HWND scope_edit = nullptr;
  HWND scope_browse = nullptr;
  HWND options_keys = nullptr;
  HWND options_values = nullptr;
  HWND options_data = nullptr;
  HWND options_data_types = nullptr;
  HWND options_standard = nullptr;
  HWND options_registry = nullptr;
  HWND options_trace = nullptr;
  HWND options_offline = nullptr;
  HWND options_reg_files = nullptr;
  HWND options_remote = nullptr;
  HWND match_case = nullptr;
  HWND match_whole = nullptr;
  HWND use_regex = nullptr;
  HWND skip_links = nullptr;
  HWND min_size = nullptr;
  HWND min_size_edit = nullptr;
  HWND max_size = nullptr;
  HWND max_size_edit = nullptr;
  HWND modified_from = nullptr;
  HWND modified_to = nullptr;
  HWND exclude_enable = nullptr;
  HWND exclude_edit = nullptr;
  HWND exclude_button = nullptr;
  HWND result_reuse = nullptr;
  HWND result_new = nullptr;
  HWND result_open_new_tab = nullptr;
  HWND result_limit_enable = nullptr;
  HWND result_limit_edit = nullptr;
  HWND find_button = nullptr;
  HWND cancel_button = nullptr;
  SearchDialogResult* out = nullptr;
  SearchSources sources;
  bool recursive = true;
  std::vector<std::wstring> history;
  std::vector<std::wstring> root_names;
  std::vector<bool> root_selected;
  std::vector<DWORD> data_types;
};

std::wstring SearchHistoryPath() {
  std::wstring folder = util::GetCacheFolder();
  if (folder.empty()) {
    return L"";
  }
  return util::JoinPath(folder, L"search_history.txt");
}

std::vector<std::wstring> LoadSearchHistory() {
  std::vector<std::wstring> items;
  std::wstring content;
  const std::wstring path = SearchHistoryPath();
  if (!path.empty() && util::ReadTextFile(path, &content, nullptr, util::kMaxStateFileBytes)) {
    for (std::wstring& line : record_fields::Lines(content)) {
      if (!line.empty()) {
        items.push_back(std::move(line));
      }
    }
  }
  return items;
}

void SaveSearchHistory(
    const std::vector<std::wstring>& items
) {
  std::wstring content;
  for (const auto& item : items) {
    content.append(item).append(L"\n");
  }
  const std::wstring path = SearchHistoryPath();
  if (!path.empty() && !content.empty()) {
    util::WriteTextFile(path, content, false);
  }
}

void UpdateHistoryList(
    std::vector<std::wstring>* items,
    const std::wstring& entry
) {
  if (!items || entry.empty()) {
    return;
  }
  std::erase_if(*items, [&](const std::wstring& item) { return util::EqualsInsensitive(item, entry); });
  items->insert(items->begin(), entry);
  const size_t max_items = 20;
  if (items->size() > max_items) {
    items->resize(max_items);
  }
}

void PopulateHistoryCombo(
    HWND combo,
    const std::vector<std::wstring>& items
) {
  if (!combo) {
    return;
  }
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  for (const auto& item : items) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
  }
}

void UpdateScopeComboText(
    SearchDialogState* state
) {
  if (!state || !state->scope_combo) {
    return;
  }
  size_t total = state->root_selected.size();
  size_t selected = 0;
  std::wstring first;
  for (size_t i = 0; i < state->root_selected.size(); ++i) {
    if (state->root_selected[i]) {
      ++selected;
      if (first.empty() && i < state->root_names.size()) {
        first = state->root_names[i];
      }
    }
  }
  std::wstring text;
  if (selected == 0) {
    text = L"No top level keys";
  } else if (selected == total) {
    text = L"All top level keys";
  } else if (selected == 1) {
    text = first;
  } else {
    text = L"Multiple keys";
  }
  SendMessageW(state->scope_combo, CB_RESETCONTENT, 0, 0);
  SendMessageW(state->scope_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
  SendMessageW(state->scope_combo, CB_SETCURSEL, 0, 0);
}

void ShowRootSelectionMenu(
    HWND owner,
    SearchDialogState* state
) {
  if (!owner || !state || !state->scope_combo) {
    return;
  }
  RECT rect = {};
  GetWindowRect(state->scope_combo, &rect);
  HMENU menu = CreatePopupMenu();
  for (size_t i = 0; i < state->root_names.size(); ++i) {
    UINT flags = MF_STRING;
    if (i < state->root_selected.size() && state->root_selected[i]) {
      flags |= MF_CHECKED;
    }
    AppendMenuW(menu, flags, static_cast<UINT>(1000 + i), state->root_names[i].c_str());
  }
  int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rect.left, rect.bottom, 0, owner, nullptr);
  DestroyMenu(menu);
  if (cmd >= 1000) {
    size_t index = static_cast<size_t>(cmd - 1000);
    if (index < state->root_selected.size()) {
      state->root_selected[index] = !state->root_selected[index];
      UpdateScopeComboText(state);
    }
  }
}

bool ParseUint64(
    const std::wstring& text,
    uint64_t* out
) {
  if (!out) {
    return false;
  }
  *out = 0;
  if (text.empty()) {
    return false;
  }
  if (text.find_first_not_of(L"0123456789") != std::wstring::npos) {
    return false;
  }
  errno = 0;
  wchar_t* end = nullptr;
  unsigned long long value = wcstoull(text.c_str(), &end, 10);
  if (!end || end == text.c_str() || *end != L'\0' || errno == ERANGE) {
    return false;
  }
  *out = static_cast<uint64_t>(value);
  return true;
}

bool GetDateTimeValue(
    HWND control,
    FILETIME* out
) {
  if (!control || !out) {
    return false;
  }
  SYSTEMTIME local = {};
  DWORD result = static_cast<DWORD>(SendMessageW(control, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&local)));
  if (result != GDT_VALID) {
    return false;
  }
  SYSTEMTIME utc = {};
  if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) {
    return false;
  }
  return SystemTimeToFileTime(&utc, out) != 0;
}

void SetDateTimeValue(
    HWND control,
    const FILETIME& value
) {
  if (!control) {
    return;
  }
  SYSTEMTIME utc = {};
  SYSTEMTIME local = {};
  if (!FileTimeToSystemTime(&value, &utc)) {
    return;
  }
  if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
    return;
  }
  SendMessageW(control, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&local));
}

std::vector<std::wstring> SplitExcludePaths(
    const std::wstring& text
) {
  std::vector<std::wstring> items;
  std::wstring current;
  for (wchar_t ch : text) {
    if (ch == L'\r' || ch == L'\n' || ch == L';' || ch == L',') {
      if (!current.empty()) {
        items.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    items.push_back(current);
  }
  for (auto& item : items) {
    item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](wchar_t c) { return c != L' '; }));
    while (!item.empty() && item.back() == L' ') {
      item.pop_back();
    }
  }
  items.erase(std::remove_if(items.begin(), items.end(), [](const std::wstring& value) { return value.empty(); }), items.end());
  return items;
}

std::wstring JoinExcludePaths(
    const std::vector<std::wstring>& items
) {
  std::wstring out;
  for (const auto& item : items) {
    if (item.empty()) {
      continue;
    }
    if (!out.empty()) {
      out.append(L", ");
    }
    out.append(item);
  }
  return out;
}

bool IsChecked(
    HWND button
) {
  return Button_GetCheck(button) == BST_CHECKED;
}

void SetChecked(
    HWND button,
    bool checked
) {
  Button_SetCheck(button, checked ? BST_CHECKED : BST_UNCHECKED);
}

void UpdateDialogEnableState(
    SearchDialogState* state
) {
  if (!state) {
    return;
  }

  bool scope_top = state->scope_top && IsChecked(state->scope_top);
  bool scope_key = state->scope_key && IsChecked(state->scope_key);
  bool standard_roots = state->options_standard && IsChecked(state->options_standard);
  bool enable_roots = scope_top;
  EnableWindow(state->scope_combo, enable_roots && standard_roots);
  EnableWindow(state->scope_edit, scope_key);
  EnableWindow(state->scope_browse, scope_key);
  EnableWindow(state->scope_recursive, scope_key);

  bool search_data = state->options_data && IsChecked(state->options_data);
  EnableWindow(state->options_data_types, search_data);
  EnableWindow(state->min_size, search_data);
  EnableWindow(state->max_size, search_data);

  bool min_checked = state->min_size && IsChecked(state->min_size);
  bool max_checked = state->max_size && IsChecked(state->max_size);
  EnableWindow(state->min_size_edit, search_data && min_checked);
  EnableWindow(state->max_size_edit, search_data && max_checked);

  bool exclude_checked = state->exclude_enable && IsChecked(state->exclude_enable);
  EnableWindow(state->exclude_edit, exclude_checked);

  bool limit_checked = state->result_limit_enable && IsChecked(state->result_limit_enable);
  EnableWindow(state->result_limit_edit, limit_checked);
  EnableWindow(state->exclude_button, exclude_checked);

  EnableWindow(state->options_trace, state->sources.traces);
  EnableWindow(state->options_registry, state->sources.registry_root);
  EnableWindow(state->options_offline, state->sources.offline);
  EnableWindow(state->options_reg_files, state->sources.reg_files);
  EnableWindow(state->options_remote, state->sources.remote);
}

void LayoutDialog(
    HWND hwnd,
    SearchDialogState* state,
    HFONT font
) {
  if (!hwnd || !state) {
    return;
  }
  appearance::SetControlFont(hwnd, font);
  RECT client = {};
  GetClientRect(hwnd, &client);
  using namespace appearance::metrics;
  const UINT dpi = win32::DpiForWindow(hwnd);
  const int margin = Scaled(kDialogContentMargin, dpi);
  const int block_gap = Scaled(kBlockGap, dpi);
  const int label_gap = Scaled(kLabelGap, dpi);
  const int label_inset = Scaled(kLabelInset, dpi);
  const int label_h = Scaled(kLabelHeight, dpi);
  const int check_inset = Scaled(kCheckInset, dpi);
  const int check_h = Scaled(kCheckHeight, dpi);
  const int line_h = Scaled(kControlHeight, dpi);
  const int control_pitch = Scaled(kControlPitch, dpi);
  const int row_pitch = Scaled(kRowPitch, dpi);
  const int group_top = Scaled(kGroupTop, dpi);
  const int group_bottom = Scaled(kGroupBottom, dpi);
  const int group_inset = Scaled(kGroupInset, dpi);
  const int button_h = Scaled(kButtonHeight, dpi);
  const int button_w = Scaled(kButtonMinWidth, dpi);
  const int button_gap = Scaled(kButtonGap, dpi);
  const int right_margin = Scaled(kDialogButtonRightMargin, dpi);
  const int bottom_margin = Scaled(kDialogButtonBottomMargin, dpi);
  const int width = client.right - client.left;
  const int x = margin;
  int y = margin;

  const int label_w = Scaled(94, dpi);
  HWND find_label = GetDlgItem(hwnd, kFindLabel);
  appearance::Place(find_label, x, y + label_inset, label_w, label_h);
  appearance::Place(state->find_combo, x + label_w + label_gap, y, width - x * 2 - label_w - label_gap, line_h);
  y += line_h + block_gap;

  const int group_w = width - x * 2;
  const int where_h = group_top + control_pitch * 2 + line_h + group_bottom;
  appearance::Place(GetDlgItem(hwnd, kWhereGroup), x, y, group_w, where_h);
  const int gx = x + group_inset;
  int gy = y + group_top;
  const int scope_label_w = Scaled(150, dpi);
  const int browse_w = Scaled(90, dpi);
  appearance::Place(state->scope_top, gx, gy + check_inset, scope_label_w, check_h);
  const int combo_x = gx + scope_label_w + label_gap;
  const int combo_w = width - combo_x - x - group_inset;
  appearance::Place(state->scope_combo, combo_x, gy, combo_w, line_h);
  appearance::Place(state->scope_key, gx, gy + control_pitch + check_inset, scope_label_w, check_h);
  const int scope_edit_y = gy + control_pitch;
  appearance::Place(state->scope_edit, combo_x, scope_edit_y, combo_w - browse_w - label_gap, line_h);
  appearance::Place(state->scope_browse, combo_x + combo_w - browse_w, scope_edit_y, browse_w, line_h);
  appearance::Place(state->scope_recursive, combo_x, gy + control_pitch * 2 + check_inset, Scaled(140, dpi), check_h);
  y += where_h + block_gap;

  const int options_h = group_top + row_pitch * 8 + check_h + group_bottom;
  appearance::Place(GetDlgItem(hwnd, kOptionsGroup), x, y, group_w, options_h);
  gy = y + group_top;
  const int left_x = x + group_inset;
  const int right_x = x + group_w / 2 + label_gap;
  auto option_row = [&](int row) { return gy + row_pitch * row; };
  const int scope_col_w = Scaled(170, dpi);
  const int hive_col_w = Scaled(200, dpi);
  appearance::Place(state->options_keys, left_x, option_row(0), scope_col_w, check_h);
  appearance::Place(state->options_values, left_x, option_row(1), scope_col_w, check_h);
  appearance::Place(state->options_data, left_x, option_row(2), scope_col_w, check_h);
  appearance::Place(state->options_standard, left_x, option_row(3), hive_col_w, check_h);
  appearance::Place(state->options_registry, left_x, option_row(4), hive_col_w, check_h);
  appearance::Place(state->options_trace, left_x, option_row(5), hive_col_w, check_h);
  appearance::Place(state->options_offline, left_x, option_row(6), hive_col_w, check_h);
  appearance::Place(state->options_reg_files, left_x, option_row(7), hive_col_w, check_h);
  appearance::Place(state->options_remote, left_x, option_row(8), hive_col_w, check_h);

  const int size_label_w = Scaled(180, dpi);
  const int size_edit_x = right_x + Scaled(188, dpi);
  const int size_edit_w = Scaled(76, dpi);
  appearance::Place(state->min_size, right_x, option_row(0), size_label_w, check_h);
  appearance::Place(state->min_size_edit, size_edit_x, option_row(0) - check_inset, size_edit_w, line_h);
  appearance::Place(state->max_size, right_x, option_row(1), size_label_w, check_h);
  appearance::Place(state->max_size_edit, size_edit_x, option_row(1) - check_inset, size_edit_w, line_h);
  appearance::Place(state->match_case, right_x, option_row(2), Scaled(140, dpi), check_h);
  appearance::Place(state->match_whole, right_x, option_row(3), Scaled(160, dpi), check_h);
  appearance::Place(state->use_regex, right_x, option_row(4), Scaled(190, dpi), check_h);
  appearance::Place(state->skip_links, right_x, option_row(5), Scaled(190, dpi), check_h);
  appearance::Place(state->options_data_types, right_x, option_row(6) - check_inset + Scaled(5, dpi), Scaled(120, dpi), line_h);
  y += options_h + block_gap;

  const int modified_label_w = Scaled(150, dpi);
  const int modified_w = Scaled(150, dpi);
  const int modified_gap = Scaled(6, dpi);
  const int modified_x = x + modified_label_w + modified_gap;
  const int dash_x = modified_x + modified_w + modified_gap;
  appearance::Place(GetDlgItem(hwnd, kModifiedLabel), x, y + label_inset, modified_label_w, label_h);
  appearance::Place(state->modified_from, modified_x, y, modified_w, line_h);
  appearance::Place(GetDlgItem(hwnd, kModifiedDash), dash_x, y + label_inset, Scaled(12, dpi), label_h);
  appearance::Place(state->modified_to, dash_x + Scaled(18, dpi), y, modified_w, line_h);
  y += line_h + block_gap;

  const int exclude_h = group_top + row_pitch + line_h + group_bottom;
  const int exclude_button_w = Scaled(80, dpi);
  appearance::Place(GetDlgItem(hwnd, kExcludeGroup), x, y, group_w, exclude_h);
  appearance::Place(state->exclude_enable, x + group_inset, y + group_top, Scaled(120, dpi), check_h);
  const int exclude_row = y + group_top + row_pitch;
  appearance::Place(state->exclude_edit, x + group_inset, exclude_row, group_w - group_inset * 2 - exclude_button_w - label_gap, line_h);
  appearance::Place(state->exclude_button, x + group_w - group_inset - exclude_button_w, exclude_row, exclude_button_w, line_h);
  y += exclude_h + block_gap;

  const int result_h = group_top + row_pitch * 2 + control_pitch + line_h + group_bottom;
  appearance::Place(GetDlgItem(hwnd, kResultGroup), x, y, group_w, result_h);
  const int result_gy = y + group_top;
  appearance::Place(state->result_reuse, x + group_inset, result_gy, Scaled(220, dpi), check_h);
  appearance::Place(state->result_new, x + group_inset, result_gy + row_pitch, Scaled(240, dpi), check_h);
  appearance::Place(state->result_open_new_tab, x + group_inset, result_gy + row_pitch * 2, Scaled(200, dpi), check_h);
  const int limit_row = result_gy + row_pitch * 2 + control_pitch;
  appearance::Place(state->result_limit_enable, x + group_inset, limit_row + check_inset, Scaled(140, dpi), check_h);
  appearance::Place(state->result_limit_edit, x + Scaled(160, dpi), limit_row, button_w, line_h);
  y += result_h + block_gap;

  const int cancel_x = width - right_margin - button_w;
  appearance::Place(state->find_button, cancel_x - button_gap - button_w, y, button_w, button_h);
  appearance::Place(state->cancel_button, cancel_x, y, button_w, button_h);
  appearance::FitDialogHeight(hwnd, y + button_h + bottom_margin);

  for (HWND edit : {state->scope_edit, state->min_size_edit, state->max_size_edit, state->exclude_edit, state->result_limit_edit}) {
    appearance::CenterEditText(edit, font, 2, 2);
  }

  appearance::SetControlFont(find_label, font);
  appearance::SetControlFont(state->find_combo, font);
  appearance::SetControlFont(state->scope_combo, font);
}

LRESULT CALLBACK SearchDialogProc(
    HWND hwnd,
    UINT msg,
    WPARAM wparam,
    LPARAM lparam
) {
  auto* state = appearance::DialogWindowState<SearchDialogState>(hwnd);
  switch (msg) {
  case WM_CREATE:
    {
      HFONT font = state->font;

      CreateWindowExW(0, L"STATIC", L"Find what:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kFindLabel), nullptr, nullptr);
      state->find_combo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kFindCombo), nullptr, nullptr);

      CreateWindowExW(0, L"BUTTON", L"Where to search", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kWhereGroup), nullptr, nullptr);
      state->scope_top = CreateWindowExW(0, L"BUTTON", L"Top level keys", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kScopeTop), nullptr, nullptr);
      state->scope_key = CreateWindowExW(0, L"BUTTON", L"Specific key", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kScopeKey), nullptr, nullptr);
      state->scope_combo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kScopeCombo), nullptr, nullptr);
      state->scope_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_MULTILINE | WS_BORDER, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kScopeEdit), nullptr, nullptr);
      state->scope_browse = CreateWindowExW(0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kScopeBrowse), nullptr, nullptr);
      state->scope_recursive = CreateWindowExW(0, L"BUTTON", L"Recursive", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kScopeRecursive), nullptr, nullptr);

      CreateWindowExW(0, L"BUTTON", L"Search options", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptionsGroup), nullptr, nullptr);
      state->options_keys = CreateWindowExW(0, L"BUTTON", L"Search keys", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptKeys), nullptr, nullptr);
      state->options_values = CreateWindowExW(0, L"BUTTON", L"Search values", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptValues), nullptr, nullptr);
      state->options_data = CreateWindowExW(0, L"BUTTON", L"Search data", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptData), nullptr, nullptr);
      state->options_data_types = CreateWindowExW(0, L"BUTTON", L"Data Types...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptDataTypes), nullptr, nullptr);
      state->match_case = CreateWindowExW(0, L"BUTTON", L"Match case", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptMatchCase), nullptr, nullptr);
      state->match_whole = CreateWindowExW(0, L"BUTTON", L"Match whole string", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptMatchWhole), nullptr, nullptr);
      state->use_regex = CreateWindowExW(0, L"BUTTON", L"Regular expressions", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptUseRegex), nullptr, nullptr);
      ui::AddTooltip(
          hwnd,
          state->use_regex,
          L"PCRE syntax: ^ $ anchors, character classes, greedy, lazy (*?) and possessive (*+) quantifiers,\n"
          L"(?<name>...) groups, lookaround (?=...) (?<=...), backreferences \\1 and Unicode classes \\p{L}, \\w, \\X.\n"
          L"Matching is unicode aware and ignores case unless 'Match case' is set."
      );
      state->skip_links = CreateWindowExW(0, L"BUTTON", L"Skip symbolic links", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptSkipLinks), nullptr, nullptr);
      state->min_size = CreateWindowExW(0, L"BUTTON", L"Min data size (bytes):", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptMinSize), nullptr, nullptr);
      state->min_size_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_MULTILINE | WS_BORDER, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptMinSizeEdit), nullptr, nullptr);
      state->max_size = CreateWindowExW(0, L"BUTTON", L"Max data size (bytes):", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptMaxSize), nullptr, nullptr);
      state->max_size_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_MULTILINE | WS_BORDER, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptMaxSizeEdit), nullptr, nullptr);
      state->options_standard = CreateWindowExW(0, L"BUTTON", L"Search Root Keys", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptStandardHives), nullptr, nullptr);
      state->options_registry = CreateWindowExW(0, L"BUTTON", L"Search REGISTRY", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptRegistryRoot), nullptr, nullptr);
      state->options_trace = CreateWindowExW(0, L"BUTTON", L"Search Trace Values", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptTraceValues), nullptr, nullptr);
      state->options_offline = CreateWindowExW(0, L"BUTTON", L"Search Offline Hives", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptOfflineHives), nullptr, nullptr);
      state->options_reg_files = CreateWindowExW(0, L"BUTTON", L"Search .reg File Tabs", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptRegFiles), nullptr, nullptr);
      state->options_remote = CreateWindowExW(0, L"BUTTON", L"Search Network Registry", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOptRemoteRegistry), nullptr, nullptr);

      CreateWindowExW(0, L"STATIC", L"Modified in period:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kModifiedLabel), nullptr, nullptr);
      CreateWindowExW(0, L"STATIC", L"-", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kModifiedDash), nullptr, nullptr);
      state->modified_from = CreateWindowExW(0, DATETIMEPICK_CLASSW, L"", WS_CHILD | WS_VISIBLE | DTS_SHORTDATEFORMAT | DTS_SHOWNONE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kModifiedFrom), nullptr, nullptr);
      state->modified_to = CreateWindowExW(0, DATETIMEPICK_CLASSW, L"", WS_CHILD | WS_VISIBLE | DTS_SHORTDATEFORMAT | DTS_SHOWNONE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kModifiedTo), nullptr, nullptr);
      SendMessageW(state->modified_from, DTM_SETFORMAT, 0, reinterpret_cast<LPARAM>(L"M/d/yyyy HH:mm"));
      SendMessageW(state->modified_to, DTM_SETFORMAT, 0, reinterpret_cast<LPARAM>(L"M/d/yyyy HH:mm"));
      SendMessageW(state->modified_from, DTM_SETSYSTEMTIME, GDT_NONE, 0);
      SendMessageW(state->modified_to, DTM_SETSYSTEMTIME, GDT_NONE, 0);

      CreateWindowExW(0, L"BUTTON", L"Exclude keys", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kExcludeGroup), nullptr, nullptr);
      state->exclude_enable = CreateWindowExW(0, L"BUTTON", L"Exclude keys", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kExcludeEnable), nullptr, nullptr);
      state->exclude_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_MULTILINE | WS_BORDER, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kExcludeEdit), nullptr, nullptr);
      state->exclude_button = CreateWindowExW(0, L"BUTTON", L"Edit...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kExcludeButton), nullptr, nullptr);

      CreateWindowExW(0, L"BUTTON", L"Result options", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kResultGroup), nullptr, nullptr);
      state->result_reuse = CreateWindowExW(0, L"BUTTON", L"Reuse last Find Results window", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kResultReuse), nullptr, nullptr);
      state->result_new = CreateWindowExW(0, L"BUTTON", L"Open new Find Results window", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kResultNew), nullptr, nullptr);
      state->result_open_new_tab = CreateWindowExW(0, L"BUTTON", L"Open result in new tab", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | WS_GROUP, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kResultOpenNewTab), nullptr, nullptr);
      state->result_limit_enable = CreateWindowExW(0, L"BUTTON", L"Limit results to", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kResultLimitEnable), nullptr, nullptr);
      state->result_limit_edit = CreateWindowExW(0, L"EDIT", L"1000", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER | ES_MULTILINE | WS_BORDER, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kResultLimitEdit), nullptr, nullptr);

      state->find_button = CreateWindowExW(0, L"BUTTON", L"Find", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kFindButton), nullptr, nullptr);
      state->cancel_button = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kCancelButton), nullptr, nullptr);

      for (HWND bordered : {state->scope_edit, state->min_size_edit, state->max_size_edit, state->exclude_edit, state->result_limit_edit}) {
        appearance::AttachThemedBorder(bordered);
      }

      appearance::SetDialogFont(hwnd, font);

      state->history = LoadSearchHistory();
      PopulateHistoryCombo(state->find_combo, state->history);
      if (state->out && !state->out->criteria.query.empty()) {
        SetWindowTextW(state->find_combo, state->out->criteria.query.c_str());
      } else if (!state->history.empty()) {
        SetWindowTextW(state->find_combo, state->history.front().c_str());
      }

      auto roots = RegistryStore::DefaultRoots(state->sources.extra_hives);
      state->root_names.clear();
      state->root_selected.clear();
      state->root_names.reserve(roots.size());
      for (const auto& root : roots) {
        state->root_names.push_back(root.path_name);
      }
      state->root_selected.assign(state->root_names.size(), true);
      if (state->out && !state->out->root_paths.empty()) {
        state->root_selected.assign(state->root_names.size(), false);
        for (size_t i = 0; i < state->root_names.size(); ++i) {
          for (const auto& path : state->out->root_paths) {
            if (util::EqualsInsensitive(state->root_names[i], path)) {
              state->root_selected[i] = true;
              break;
            }
          }
        }
      }
      UpdateScopeComboText(state);

      if (state->out) {
        state->data_types = state->out->criteria.allowed_types;
        state->recursive = state->out->criteria.recursive;
      }
      const SearchDialogResult* initial = state->out;
      if (initial) {
        SetChecked(state->options_keys, initial->criteria.search_keys);
        SetChecked(state->options_values, initial->criteria.search_values);
        SetChecked(state->options_data, initial->criteria.search_data);
        SetChecked(state->match_case, initial->criteria.match_case);
        SetChecked(state->match_whole, initial->criteria.match_whole);
        SetChecked(state->use_regex, initial->criteria.use_regex);
        SetChecked(state->skip_links, initial->criteria.skip_links);
        if (initial->criteria.use_min_size) {
          SetChecked(state->min_size, true);
          SetWindowTextW(state->min_size_edit, std::to_wstring(initial->criteria.min_size).c_str());
        }
        if (initial->criteria.use_max_size) {
          SetChecked(state->max_size, true);
          SetWindowTextW(state->max_size_edit, std::to_wstring(initial->criteria.max_size).c_str());
        }
        if (initial->criteria.use_modified_from) {
          SetDateTimeValue(state->modified_from, initial->criteria.modified_from);
        }
        if (initial->criteria.use_modified_to) {
          SetDateTimeValue(state->modified_to, initial->criteria.modified_to);
        }
        bool standard_hives = initial->search_standard_hives;
        bool registry_root = initial->search_registry_root;
        bool trace_values = initial->search_trace_values;
        if (!state->sources.registry_root) {
          registry_root = false;
        }
        if (!state->sources.traces) {
          trace_values = false;
        }
        SetChecked(state->options_standard, standard_hives);
        SetChecked(state->options_registry, registry_root);
        SetChecked(state->options_trace, trace_values);
        SetChecked(state->options_offline, initial->search_offline_hives && state->sources.offline);
        SetChecked(state->options_reg_files, initial->search_reg_files && state->sources.reg_files);
        SetChecked(state->options_remote, initial->search_remote_registry && state->sources.remote);
        bool scope_top = initial->scope == SearchScope::kEntireRegistry;
        SetChecked(state->scope_top, scope_top);
        SetChecked(state->scope_key, !scope_top);
        if (!initial->start_key.empty()) {
          SetWindowTextW(state->scope_edit, initial->start_key.c_str());
        }
        bool new_tab = initial->result_mode == SearchResultMode::kNewTab;
        SetChecked(state->result_reuse, !new_tab);
        SetChecked(state->result_new, new_tab);
        SetChecked(state->result_open_new_tab, initial->open_in_new_tab);
        const bool limited = initial->criteria.max_results > 0;
        SetChecked(state->result_limit_enable, limited);
        SetWindowTextW(state->result_limit_edit, std::to_wstring(limited ? initial->criteria.max_results : 1000).c_str());
        SendMessageW(state->result_limit_edit, EM_SETSEL, 0, 0);
        EnableWindow(state->result_limit_edit, limited);
      } else {
        SetChecked(state->options_keys, false);
        SetChecked(state->options_values, true);
        SetChecked(state->options_data, true);
        SetChecked(state->options_standard, true);
        SetChecked(state->options_registry, false);
        SetChecked(state->options_trace, state->sources.traces);
        SetChecked(state->scope_top, true);
        SetChecked(state->result_reuse, true);
        SetChecked(state->result_limit_enable, true);
      }
      SetChecked(state->scope_recursive, state->recursive);

      UpdateDialogEnableState(state);
      COMBOBOXINFO combo = {sizeof(combo)};
      state->focus = GetComboBoxInfo(state->find_combo, &combo) && combo.hwndItem ? combo.hwndItem : state->find_combo;
      SendMessageW(state->focus, EM_SETSEL, 0, -1);
      LayoutDialog(hwnd, state, font);
      return 0;
    }
  case WM_SIZE:
    LayoutDialog(hwnd, state, state->font);
    return 0;
  case WM_COMMAND:
    {
      if (HIWORD(wparam) == CBN_DROPDOWN && LOWORD(wparam) == kScopeCombo) {
        ShowRootSelectionMenu(hwnd, state);
        SendMessageW(state->scope_combo, CB_SHOWDROPDOWN, FALSE, 0);
        return 0;
      }
      if (HIWORD(wparam) == BN_CLICKED) {
        switch (LOWORD(wparam)) {
        case kScopeTop:
        case kScopeKey:
        case kOptData:
        case kOptMinSize:
        case kOptMaxSize:
        case kOptStandardHives:
        case kOptRegistryRoot:
        case kOptTraceValues:
        case kOptOfflineHives:
        case kOptRegFiles:
        case kOptRemoteRegistry:
        case kExcludeEnable:
        case kResultLimitEnable:
          UpdateDialogEnableState(state);
          break;
        default:
          break;
        }
      }
      switch (LOWORD(wparam)) {
      case kScopeBrowse:
        {
          std::wstring selected;
          if (ShowBrowseKeyDialog(hwnd, &selected)) {
            if (!selected.empty()) {
              SetWindowTextW(state->scope_edit, selected.c_str());
            }
            SetChecked(state->scope_key, true);
            SetChecked(state->scope_top, false);
            UpdateDialogEnableState(state);
          }
          return 0;
        }
      case kOptDataTypes:
        query_prompts::ShowDataTypes(hwnd, &state->data_types);
        return 0;
      case kExcludeButton:
        {
          editors::TextRequest request;
          request.title = L"Exclude Keys";
          request.label = L"Each line should include one key.";
          request.text = util::JoinLines(SplitExcludePaths(util::WindowText(state->exclude_edit)));
          request.multiline = true;
          request.browse = ShowBrowseKeyDialog;
          editors::TextResult result;
          if (editors::EditText(hwnd, request, &result)) {
            SetWindowTextW(state->exclude_edit, JoinExcludePaths(SplitExcludePaths(result.text)).c_str());
          }
          return 0;
        }
      case kFindButton:
        {
          std::wstring query_text = util::WindowText(state->find_combo);
          if (query_text.empty()) {
            ui::ShowWarning(hwnd, L"Enter a search term.");
            return 0;
          }
          if (IsChecked(state->use_regex) && query_text.size() > search::regex::kMaxPatternLength) {
            ui::ShowWarning(hwnd, L"The regular expression is too long.");
            return 0;
          }
          bool keys = IsChecked(state->options_keys);
          bool values = IsChecked(state->options_values);
          bool data = IsChecked(state->options_data);
          if (!keys && !values && !data) {
            ui::ShowWarning(hwnd, L"Select at least one search option.");
            return 0;
          }
          bool standard_hives = IsChecked(state->options_standard);
          bool registry_root = IsChecked(state->options_registry);
          bool trace_values = IsChecked(state->options_trace);
          bool offline_hives = state->sources.offline && IsChecked(state->options_offline);
          bool reg_files = state->sources.reg_files && IsChecked(state->options_reg_files);
          bool remote_registry = state->sources.remote && IsChecked(state->options_remote);
          if (!state->sources.registry_root) {
            registry_root = false;
          }
          if (!state->sources.traces) {
            trace_values = false;
          }
          if (!standard_hives && !registry_root && !trace_values && !offline_hives && !reg_files && !remote_registry) {
            ui::ShowWarning(hwnd, L"Select at least one search source.");
            return 0;
          }

          SearchDialogResult result;
          result.criteria.query = query_text;
          result.criteria.search_keys = keys;
          result.criteria.search_values = values;
          result.criteria.search_data = data;
          result.criteria.match_case = IsChecked(state->match_case);
          result.criteria.match_whole = IsChecked(state->match_whole);
          result.criteria.use_regex = IsChecked(state->use_regex);
          result.criteria.skip_links = IsChecked(state->skip_links);
          if (data) {
            result.criteria.allowed_types = state->data_types;
            if (IsChecked(state->min_size)) {
              wchar_t buffer[64] = {};
              GetWindowTextW(state->min_size_edit, buffer, static_cast<int>(_countof(buffer)));
              uint64_t value = 0;
              if (!ParseUint64(buffer, &value)) {
                ui::ShowWarning(hwnd, L"Enter a valid minimum data size.");
                return 0;
              }
              result.criteria.use_min_size = true;
              result.criteria.min_size = value;
            }
            if (IsChecked(state->max_size)) {
              wchar_t buffer[64] = {};
              GetWindowTextW(state->max_size_edit, buffer, static_cast<int>(_countof(buffer)));
              uint64_t value = 0;
              if (!ParseUint64(buffer, &value)) {
                ui::ShowWarning(hwnd, L"Enter a valid maximum data size.");
                return 0;
              }
              result.criteria.use_max_size = true;
              result.criteria.max_size = value;
            }
          }
          FILETIME modified_from = {};
          FILETIME modified_to = {};
          bool has_modified_from = GetDateTimeValue(state->modified_from, &modified_from);
          bool has_modified_to = GetDateTimeValue(state->modified_to, &modified_to);
          if (has_modified_from) {
            result.criteria.use_modified_from = true;
            result.criteria.modified_from = modified_from;
          }
          if (has_modified_to) {
            result.criteria.use_modified_to = true;
            result.criteria.modified_to = modified_to;
          }
          if (result.criteria.use_min_size && result.criteria.use_max_size && result.criteria.min_size > result.criteria.max_size) {
            ui::ShowWarning(hwnd, L"Minimum data size can't exceed maximum data size.");
            return 0;
          }
          if (has_modified_from && has_modified_to && CompareFileTime(&modified_from, &modified_to) > 0) {
            ui::ShowWarning(hwnd, L"Modified date range is invalid.");
            return 0;
          }
          result.search_standard_hives = standard_hives;
          result.search_registry_root = registry_root;
          result.search_trace_values = trace_values;
          result.search_offline_hives = offline_hives;
          result.search_reg_files = reg_files;
          result.search_remote_registry = remote_registry;

          bool scope_top = IsChecked(state->scope_top);
          result.scope = scope_top ? SearchScope::kEntireRegistry : SearchScope::kCurrentKey;
          state->recursive = IsChecked(state->scope_recursive);
          result.criteria.recursive = scope_top ? true : state->recursive;
          result.result_mode = IsChecked(state->result_new) ? SearchResultMode::kNewTab : SearchResultMode::kReuseTab;
          result.open_in_new_tab = IsChecked(state->result_open_new_tab);
          if (IsChecked(state->result_limit_enable)) {
            wchar_t limit_text[32] = {};
            GetWindowTextW(state->result_limit_edit, limit_text, static_cast<int>(_countof(limit_text)));
            uint64_t limit = 0;
            if (!ParseUint64(limit_text, &limit) || limit == 0) {
              ui::ShowWarning(hwnd, L"Enter a valid result limit.");
              return 0;
            }
            result.criteria.max_results = limit;
          } else {
            result.criteria.max_results = 0;
          }

          if (IsChecked(state->exclude_enable)) {
            result.criteria.exclude_paths =
                SplitExcludePaths(util::WindowText(state->exclude_edit));
          }

          result.root_paths.clear();
          result.start_key.clear();
          if (scope_top) {
            if (standard_hives) {
              for (size_t i = 0; i < state->root_selected.size(); ++i) {
                if (state->root_selected[i] && i < state->root_names.size()) {
                  result.root_paths.push_back(state->root_names[i]);
                }
              }
              if (result.root_paths.empty()) {
                ui::ShowWarning(hwnd, L"Select at least one top level key.");
                return 0;
              }
            }
          } else {
            result.start_key = util::WindowText(state->scope_edit);
          }

          UpdateHistoryList(&state->history, query_text);
          SaveSearchHistory(state->history);

          *state->out = std::move(result);
          appearance::CloseDialogWindow(state, true);
          return 0;
        }
      default:
        break;
      }
      break;
    }
  default:
    break;
  }
  return appearance::DefDialogWindowProc(hwnd, msg, wparam, lparam);
}

} // namespace

bool ShowBrowseKeyDialog(
    HWND owner,
    std::wstring* selected_path
) {
  return query_prompts::ShowRegistryKey(owner, selected_path);
}

bool ShowSearchDialog(
    HWND owner,
    SearchDialogResult* result,
    const SearchSources& available
) {
  SearchDialogState state;
  state.out = result;
  state.owner = owner;
  state.sources = available;
  const UINT dpi = win32::DpiForWindow(owner);
  return result && appearance::RunDialogWindow(&state, kDialogClass, SearchDialogProc, L"Find", {appearance::metrics::Scaled(600, dpi), appearance::metrics::Scaled(700, dpi)});
}

} // namespace regkit
