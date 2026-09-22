// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "editors/comment_editor.h"

#include "appearance/dialog_layout.h"
#include "editors/dialog_support.h"

#include "resource.h"

#include <utility>

namespace regkit::editors {

namespace {

struct State {
  const CommentRequest* request = nullptr;
  CommentResult value;
  HFONT font = nullptr;
  appearance::DialogResizer resizer;
  bool accepted = false;
};

bool Checked(
    HWND dialog,
    int id
) {
  return IsDlgButtonChecked(dialog, id) == BST_CHECKED;
}

CommentScope ReadScope(
    HWND dialog
) {
  CommentScope scope;
  scope.rule = Checked(dialog, IDC_COMMENT_RULE);
  scope.same_type = Checked(dialog, IDC_COMMENT_TYPE);
  scope.same_size = Checked(dialog, IDC_COMMENT_SIZE);
  scope.in_key = Checked(dialog, IDC_COMMENT_KEY);
  scope.include_subkeys = scope.in_key && Checked(dialog, IDC_COMMENT_SUBKEYS);
  scope.key_path = dialog_support::ReadText(dialog, IDC_COMMENT_KEY_PATH);
  return scope;
}

void UpdateControls(
    HWND dialog
) {
  const CommentScope scope = ReadScope(dialog);
  for (const int id : {IDC_COMMENT_TYPE, IDC_COMMENT_SIZE, IDC_COMMENT_KEY}) {
    EnableWindow(GetDlgItem(dialog, id), scope.rule);
  }
  EnableWindow(GetDlgItem(dialog, IDC_COMMENT_KEY_PATH), scope.rule && scope.in_key);
  EnableWindow(GetDlgItem(dialog, IDC_COMMENT_SUBKEYS), scope.rule && scope.in_key);
}

void InitControls(
    HWND dialog,
    const State* state
) {
  const CommentRequest& request = *state->request;
  std::wstring text;
  for (const wchar_t ch : request.text) {
    if (ch == L'\n' && (text.empty() || text.back() != L'\r')) {
      text.push_back(L'\r');
    }
    text.push_back(ch);
  }
  SetDlgItemTextW(dialog, IDC_EDIT, text.c_str());
  CheckRadioButton(dialog, IDC_COMMENT_VALUE, IDC_COMMENT_RULE, request.scope.rule ? IDC_COMMENT_RULE : IDC_COMMENT_VALUE);
  if (request.multiple) {
    SetDlgItemTextW(dialog, IDC_COMMENT_VALUE, L"Selected values only");
  }
  SetDlgItemTextW(dialog, IDC_COMMENT_RULE, (request.multiple ? std::wstring(L"Values with the selected names") : L"Values named " + request.name).c_str());
  CheckDlgButton(dialog, IDC_COMMENT_TYPE, request.scope.same_type ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(dialog, IDC_COMMENT_SIZE, request.scope.same_size ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(dialog, IDC_COMMENT_KEY, request.scope.in_key ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(dialog, IDC_COMMENT_SUBKEYS, request.scope.include_subkeys ? BST_CHECKED : BST_UNCHECKED);
  SetDlgItemTextW(dialog, IDC_COMMENT_TYPE_TEXT, request.type.c_str());
  SetDlgItemTextW(dialog, IDC_COMMENT_SIZE_TEXT, request.size.c_str());
  SetDlgItemTextW(dialog, IDC_COMMENT_KEY_PATH, request.scope.key_path.c_str());
  ShowWindow(GetDlgItem(dialog, IDC_COMMENT_RESTORE), request.can_restore ? SW_SHOW : SW_HIDE);
  if (!request.key) {
    UpdateControls(dialog);
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
    dialog_support::Initialize(dialog, &state->font, {IDC_EDIT, IDC_COMMENT_KEY_PATH});
    dialog_support::AllowNewlines(dialog, IDC_EDIT);
    InitControls(dialog, state);
    using namespace appearance;
    state->resizer.Attach(dialog, {
                                      {IDC_LABEL, kAnchorLeft | kAnchorTop | kAnchorRight},
                                      {IDC_EDIT, kAnchorLeft | kAnchorTop | kAnchorRight | kAnchorBottom},
                                      {IDC_COMMENT_APPLY_GROUP, kAnchorLeft | kAnchorRight | kAnchorBottom},
                                      {IDC_COMMENT_VALUE, kAnchorLeft | kAnchorBottom},
                                      {IDC_COMMENT_RULE, kAnchorLeft | kAnchorRight | kAnchorBottom},
                                      {IDC_COMMENT_MATCH_GROUP, kAnchorLeft | kAnchorRight | kAnchorBottom},
                                      {IDC_COMMENT_TYPE, kAnchorLeft | kAnchorBottom},
                                      {IDC_COMMENT_TYPE_TEXT, kAnchorLeft | kAnchorRight | kAnchorBottom},
                                      {IDC_COMMENT_SIZE, kAnchorLeft | kAnchorBottom},
                                      {IDC_COMMENT_SIZE_TEXT, kAnchorLeft | kAnchorRight | kAnchorBottom},
                                      {IDC_COMMENT_KEY, kAnchorLeft | kAnchorBottom},
                                      {IDC_COMMENT_KEY_PATH, kAnchorLeft | kAnchorRight | (state->request->key ? kAnchorTop : kAnchorBottom)},
                                      {IDC_COMMENT_SUBKEYS, kAnchorLeft | kAnchorBottom},
                                      {IDC_COMMENT_RESTORE, kAnchorLeft | kAnchorBottom},
                                      {IDOK, kAnchorRight | kAnchorBottom},
                                      {IDCANCEL, kAnchorRight | kAnchorBottom},
                                  });
    return TRUE;
  }
  if (message == WM_DESTROY) {
    if (state) {
      dialog_support::ReleaseFont(&state->font);
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
  INT_PTR themed = 0;
  if (dialog_support::HandleThemeMessage(dialog, message, wparam, lparam, &themed)) {
    return themed;
  }
  if (message != WM_COMMAND || !state) {
    return FALSE;
  }
  switch (LOWORD(wparam)) {
  case IDOK:
  case IDC_COMMENT_RESTORE:
    state->value.text = dialog_support::ReadText(dialog, IDC_EDIT);
    std::erase(state->value.text, L'\r');
    state->value.scope = ReadScope(dialog);
    state->value.restore_default = LOWORD(wparam) == IDC_COMMENT_RESTORE;
    state->accepted = true;
    EndDialog(dialog, IDOK);
    return TRUE;
  case IDCANCEL:
    EndDialog(dialog, IDCANCEL);
    return TRUE;
  case IDC_COMMENT_VALUE:
  case IDC_COMMENT_RULE:
  case IDC_COMMENT_TYPE:
  case IDC_COMMENT_SIZE:
  case IDC_COMMENT_KEY:
    UpdateControls(dialog);
    return TRUE;
  default:
    return FALSE;
  }
}

} // namespace

bool EditComment(
    HWND owner,
    const CommentRequest& request,
    CommentResult* result
) {
  State state;
  state.request = &request;
  const INT_PTR dialog_result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(request.key ? IDD_KEY_COMMENT : IDD_COMMENT), owner, DialogProc, reinterpret_cast<LPARAM>(&state));
  if (dialog_result != IDOK || !state.accepted) {
    return false;
  }
  *result = std::move(state.value);
  return true;
}

} // namespace regkit::editors
