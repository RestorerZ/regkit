// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "dialogs/binary_editor.h"

#include "dialogs/binary_text.h"
#include "dialogs/bitfield_editor.h"
#include "registry/registry_path.h"
#include "registry/value_format.h"
#include "ui/dialog_layout.h"
#include "ui/dialog_support.h"
#include "ui/feedback.h"

#include "resource.h"
#include "win32/text_transform.h"
#include "win32/translation.h"

#include <initializer_list>
#include <utility>

namespace regkit::editors
{

namespace
{

struct State
{
    const BinaryRequest* request = nullptr;
    BinaryResult value;
    std::wstring text;
    int group_bytes = 1;
    bool unicode = false;
    bool accepted = false;
    HFONT ui_font = nullptr;
    HFONT mono_font = nullptr;
    appearance::DialogResizer resizer;
};

void SelectGroup(HWND dialog, int selected)
{
    constexpr int controls[] = {IDC_FORMAT_BYTE, IDC_FORMAT_WORD, IDC_FORMAT_DWORD, IDC_FORMAT_QWORD};
    for (const int id : controls)
    {
        CheckDlgButton(dialog, id, id == selected ? BST_CHECKED : BST_UNCHECKED);
    }
}

void SelectTextMode(HWND dialog, int selected)
{
    CheckDlgButton(dialog, IDC_TEXT_ANSI, selected == IDC_TEXT_ANSI ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_TEXT_UNICODE, selected == IDC_TEXT_UNICODE ? BST_CHECKED : BST_UNCHECKED);
}

void UpdatePreview(HWND dialog, State* state)
{
    if (!state)
    {
        return;
    }
    state->text = dialog_support::ReadText(dialog, IDC_EDIT);
    std::vector<BYTE> bytes;
    if (!value_format::ParseHex(state->text, &bytes))
    {
        SetDlgItemTextW(dialog, IDC_BINARY_PREVIEW, util::Tr(L"Invalid hex input."));
        SetDlgItemTextW(dialog, IDC_VALUE_BYTES, util::Tr(L"Invalid"));
        return;
    }
    // grouping & text mode change only the preview
    const std::wstring preview = binary_text::Preview(bytes, state->group_bytes, state->unicode);
    SetDlgItemTextW(dialog, IDC_BINARY_PREVIEW, preview.c_str());
    SetDlgItemTextW(dialog, IDC_VALUE_BYTES, value_format::ByteCount(bytes.size()).c_str());
}

void ConfigureIdentity(HWND dialog, const BinaryRequest& request)
{
    const std::wstring name = request.value_name.empty() ? util::Tr(L"(Default)") : registry_path::DisplayName(request.value_name);
    SetDlgItemTextW(dialog, IDC_VALUE_NAME, name.c_str());
    SendDlgItemMessageW(dialog, IDC_VALUE_NAME, EM_SETREADONLY, TRUE, 0);
    const HWND name_control = GetDlgItem(dialog, IDC_VALUE_NAME);
    SetWindowLongPtrW(name_control, GWL_STYLE, GetWindowLongPtrW(name_control, GWL_STYLE) & ~WS_TABSTOP);
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG)
    {
        state = reinterpret_cast<State*>(lparam);
        state->text = util::ToHexLines(state->request->data);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog, util::Tr(L"Edit Value"));
        SetDlgItemTextW(dialog, IDC_LABEL, util::Tr(L"Hex bytes:"));
        SetDlgItemTextW(dialog, IDC_NOTE, util::Tr(L"Preview:"));
        SetDlgItemTextW(dialog, IDC_EDIT, state->text.c_str());
        ConfigureIdentity(dialog, *state->request);
        SelectGroup(dialog, IDC_FORMAT_BYTE);
        SelectTextMode(dialog, IDC_TEXT_ANSI);
        if (state->request->read_only)
        {
            SendDlgItemMessageW(dialog, IDC_EDIT, EM_SETREADONLY, TRUE, 0);
            for (const int id : {IDC_FORMAT_BYTE, IDC_FORMAT_WORD, IDC_FORMAT_DWORD, IDC_FORMAT_QWORD, IDC_TEXT_ANSI, IDC_TEXT_UNICODE})
            {
                EnableWindow(GetDlgItem(dialog, id), FALSE);
            }
        }
        dialog_support::Initialize(dialog, &state->ui_font, {IDC_VALUE_NAME, IDC_EDIT, IDC_BINARY_PREVIEW});
        state->mono_font = dialog_support::ApplyMonoFont(dialog, {IDC_EDIT, IDC_BINARY_PREVIEW});
        using namespace appearance;
        state->resizer.Attach(dialog, {
                                          {IDC_VALUE_NAME, kAnchorLeft | kAnchorTop | kAnchorRight},
                                          {IDC_VALUE_BYTES_LABEL, kAnchorTop | kAnchorRight},
                                          {IDC_VALUE_BYTES, kAnchorTop | kAnchorRight},
                                          {IDC_LABEL, kAnchorLeft | kAnchorTop | kAnchorRight},
                                          {IDC_EDIT, kAnchorLeft | kAnchorTop | kAnchorRight | kAnchorBottom},
                                          {IDC_NOTE, kAnchorLeft | kAnchorRight | kAnchorBottom},
                                          {IDC_BINARY_PREVIEW, kAnchorLeft | kAnchorRight | kAnchorBottom},
                                          {IDC_FORMAT_GROUP, kAnchorLeft | kAnchorBottom},
                                          {IDC_FORMAT_BYTE, kAnchorLeft | kAnchorBottom},
                                          {IDC_FORMAT_WORD, kAnchorLeft | kAnchorBottom},
                                          {IDC_FORMAT_DWORD, kAnchorLeft | kAnchorBottom},
                                          {IDC_FORMAT_QWORD, kAnchorLeft | kAnchorBottom},
                                          {IDC_TEXT_GROUP, kAnchorRight | kAnchorBottom},
                                          {IDC_TEXT_ANSI, kAnchorRight | kAnchorBottom},
                                          {IDC_TEXT_UNICODE, kAnchorRight | kAnchorBottom},
                                          {IDC_BITS, kAnchorLeft | kAnchorBottom},
                                          {IDOK, kAnchorRight | kAnchorBottom},
                                          {IDCANCEL, kAnchorRight | kAnchorBottom},
                                      });
        UpdatePreview(dialog, state);
        return TRUE;
    }
    if (message == WM_DESTROY)
    {
        if (state)
        {
            dialog_support::ReleaseFont(&state->mono_font);
            dialog_support::ReleaseFont(&state->ui_font);
        }
        return TRUE;
    }
    INT_PTR themed = 0;
    if (dialog_support::HandleThemeMessage(dialog, message, wparam, lparam, &themed, state ? &state->resizer : nullptr))
    {
        return themed;
    }
    if (message != WM_COMMAND || !state)
    {
        return FALSE;
    }
    const int id = LOWORD(wparam);
    if (id == IDC_EDIT && HIWORD(wparam) == EN_CHANGE)
    {
        UpdatePreview(dialog, state);
        return TRUE;
    }
    switch (id)
    {
    case IDC_BITS:
        {
            // pass unsaved hex edits to the bit editor and copy its result back
            std::vector<BYTE> bytes;
            if (!value_format::ParseHex(dialog_support::ReadText(dialog, IDC_EDIT), &bytes) || bytes.empty())
            {
                ui::ShowError(dialog, util::Tr(L"Invalid hex input."));
                return TRUE;
            }
            BitfieldRequest request;
            request.value_name = state->request->value_name;
            request.data = bytes;
            request.read_only = state->request->read_only;
            BitfieldResult result;
            if (!EditBitfield(dialog, request, &result))
            {
                return TRUE;
            }
            SetDlgItemTextW(dialog, IDC_EDIT, util::ToHexLines(result.data).c_str());
            UpdatePreview(dialog, state);
            return TRUE;
        }
    case IDC_FORMAT_BYTE:
    case IDC_FORMAT_WORD:
    case IDC_FORMAT_DWORD:
    case IDC_FORMAT_QWORD:
        {
            const int group = id == IDC_FORMAT_BYTE ? 1 : id == IDC_FORMAT_WORD ? 2
                                                      : id == IDC_FORMAT_DWORD  ? 4
                                                                                : 8;
            if (group != state->group_bytes)
            {
                state->group_bytes = group;
                SelectGroup(dialog, id);
                UpdatePreview(dialog, state);
            }
            return TRUE;
        }
    case IDC_TEXT_ANSI:
    case IDC_TEXT_UNICODE:
        if ((id == IDC_TEXT_UNICODE) != state->unicode)
        {
            state->unicode = id == IDC_TEXT_UNICODE;
            SelectTextMode(dialog, id);
            UpdatePreview(dialog, state);
        }
        return TRUE;
    case IDOK:
        {
            std::wstring text = dialog_support::ReadText(dialog, IDC_EDIT);
            std::vector<BYTE> parsed;
            if (!value_format::ParseHex(text, &parsed))
            {
                ui::ShowError(dialog, util::Tr(L"Invalid hex input."));
                SetFocus(GetDlgItem(dialog, IDC_EDIT));
                return TRUE;
            }
            state->text = std::move(text);
            state->value.data = std::move(parsed);
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

} // namespace

bool EditBinary(HWND owner, const BinaryRequest& request, BinaryResult* result)
{
    if (!result)
    {
        return false;
    }
    State state;
    state.request = &request;
    const INT_PTR dialog_result = dialog_support::Modal(owner, IDD_BINARY, DialogProc, reinterpret_cast<LPARAM>(&state));
    if (dialog_result != IDOK || !state.accepted)
    {
        return false;
    }
    *result = std::move(state.value);
    return true;
}

} // namespace regkit::editors
