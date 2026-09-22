// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "editors/decoder_dialog.h"

#include "appearance/dialog_layout.h"
#include "appearance/feedback.h"
#include "editors/binary_text.h"
#include "editors/dialog_support.h"
#include "registry/value_decoder.h"
#include "registry/value_format.h"

#include "resource.h"
#include "win32/text_transform.h"

#include <algorithm>
#include <span>
#include <utility>

namespace regkit::editors
{

namespace
{

using value_decoder::DecoderEntry;
using value_decoder::DecoderId;
using value_decoder::TransformEntry;
using value_decoder::TransformId;

constexpr size_t kRawBytePreviewLimit = 64 * 1024;

struct State
{
    const DecodeRequest* request = nullptr;
    std::vector<BYTE> transformed;
    std::vector<TransformEntry> transforms;
    std::vector<DecoderEntry> decoders;
    TransformId transform = TransformId::kNone;
    std::wstring output;
    HFONT ui_font = nullptr;
    HFONT mono_font = nullptr;
    appearance::DialogResizer resizer;
};

const std::vector<BYTE>& Bytes(const State& state)
{
    // no transform reads original bytes without making a copy
    return state.transform == TransformId::kNone ? state.request->data : state.transformed;
}

void FillCombo(HWND dialog, int control, const std::vector<TransformEntry>& entries, int selected)
{
    SendDlgItemMessageW(dialog, control, CB_RESETCONTENT, 0, 0);
    for (const TransformEntry& entry : entries)
    {
        SendDlgItemMessageW(dialog, control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.name));
    }
    SendDlgItemMessageW(dialog, control, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
}

void FillCombo(HWND dialog, int control, const std::vector<DecoderEntry>& entries, int selected)
{
    SendDlgItemMessageW(dialog, control, CB_RESETCONTENT, 0, 0);
    for (const DecoderEntry& entry : entries)
    {
        SendDlgItemMessageW(dialog, control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.name));
    }
    SendDlgItemMessageW(dialog, control, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
}

int IndexOfDecoder(const std::vector<DecoderEntry>& entries, DecoderId id)
{
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].id == id)
        {
            return static_cast<int>(i);
        }
    }
    return 0;
}

void ShowOutput(HWND dialog, State* state, std::wstring text)
{
    state->output = std::move(text);
    SetDlgItemTextW(dialog, IDC_EDIT, state->output.c_str());
}

void RunDecoder(HWND dialog, State* state)
{
    const int index = static_cast<int>(SendDlgItemMessageW(dialog, IDC_DECODE_FORMAT, CB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<size_t>(index) >= state->decoders.size())
    {
        return;
    }
    const DecoderId id = state->decoders[static_cast<size_t>(index)].id;
    const std::vector<BYTE>& bytes = Bytes(*state);
    const value_decoder::Decoded decoded = value_decoder::Decode(id, bytes.data(), bytes.size());
    if (!decoded.ok)
    {
        ShowOutput(dialog, state, decoded.error);
        return;
    }
    std::wstring text;
    for (const value_decoder::Field& field : decoded.fields)
    {
        text.append(field.name).append(L": ").append(field.value).append(L"\r\n");
    }
    if (id == DecoderId::kRawBytes)
    {
        // cap formatted preview so large registry values stay responsive
        const size_t shown = std::min(bytes.size(), kRawBytePreviewLimit);
        const std::span<const BYTE> span(bytes.data(), shown);
        text.append(L"\r\n").append(util::ToHex(span, L' ', true));
        text.append(L"\r\n\r\n").append(binary_text::Preview(span, 1, false));
        if (shown != bytes.size())
        {
            text.append(L"\r\n\r\nShowing the first ").append(std::to_wstring(shown)).append(L" bytes.");
        }
    }
    ShowOutput(dialog, state, std::move(text));
}

void RebuildDecoders(HWND dialog, State* state)
{
    const std::vector<BYTE>& bytes = Bytes(*state);
    state->decoders = value_decoder::AvailableDecoders(bytes.data(), bytes.size());
    DecoderId wanted = DecoderId::kRawBytes;
    if (state->transform == TransformId::kNone)
    {
        wanted = value_decoder::Suggest(state->request->type, state->request->key_path, state->request->value_name, bytes.size());
    }
    FillCombo(dialog, IDC_DECODE_FORMAT, state->decoders, IndexOfDecoder(state->decoders, wanted));
    RunDecoder(dialog, state);
}

void ApplyTransform(HWND dialog, State* state)
{
    const int index = static_cast<int>(SendDlgItemMessageW(dialog, IDC_DECODE_ENCODING, CB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<size_t>(index) >= state->transforms.size())
    {
        return;
    }
    const TransformId id = state->transforms[static_cast<size_t>(index)].id;
    std::vector<BYTE> bytes;
    std::wstring error;
    if (!value_decoder::Transform(id, state->request->type, state->request->data, &bytes, &error))
    {
        state->transformed.clear();
        state->decoders.clear();
        SendDlgItemMessageW(dialog, IDC_DECODE_FORMAT, CB_RESETCONTENT, 0, 0);
        ShowOutput(dialog, state, error);
        return;
    }
    state->transform = id;
    state->transformed = std::move(bytes);
    RebuildDecoders(dialog, state);
}

void ConfigureIdentity(HWND dialog, const DecodeRequest& request)
{
    const std::wstring name = request.value_name.empty() ? L"(Default)" : request.value_name;
    SetDlgItemTextW(dialog, IDC_VALUE_NAME, name.c_str());
    SendDlgItemMessageW(dialog, IDC_VALUE_NAME, EM_SETREADONLY, TRUE, 0);
    const HWND name_control = GetDlgItem(dialog, IDC_VALUE_NAME);
    SetWindowLongPtrW(name_control, GWL_STYLE, GetWindowLongPtrW(name_control, GWL_STYLE) & ~WS_TABSTOP);
    std::wstring summary = value_format::TypeName(request.type);
    summary.append(L", ")
        .append(std::to_wstring(request.data.size()))
        .append(request.data.size() == 1 ? L" byte" : L" bytes");
    SetDlgItemTextW(dialog, IDC_VALUE_BYTES, summary.c_str());
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG)
    {
        state = reinterpret_cast<State*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog, L"Decode Value");
        SetDlgItemTextW(dialog, IDC_VALUE_NAME_LABEL, L"Name:");
        SetDlgItemTextW(dialog, IDC_LABEL, L"Encoding:");
        SetDlgItemTextW(dialog, IDC_NOTE, L"Interpret as:");
        ConfigureIdentity(dialog, *state->request);
        SendDlgItemMessageW(dialog, IDC_EDIT, EM_SETREADONLY, TRUE, 0);
        state->transforms = value_decoder::AvailableTransforms(state->request->type, state->request->data);
        FillCombo(dialog, IDC_DECODE_ENCODING, state->transforms, 0);
        dialog_support::Initialize(dialog, &state->ui_font, {IDC_VALUE_NAME, IDC_EDIT});
        state->mono_font =
            CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_MODERN, L"Consolas");
        if (state->mono_font)
        {
            SendDlgItemMessageW(dialog, IDC_EDIT, WM_SETFONT, reinterpret_cast<WPARAM>(state->mono_font), TRUE);
        }
        using namespace appearance;
        state->resizer.Attach(dialog, {
                                          {IDC_VALUE_NAME, kAnchorLeft | kAnchorTop | kAnchorRight},
                                          {IDC_VALUE_BYTES, kAnchorTop | kAnchorRight},
                                          {IDC_DECODE_ENCODING, kAnchorLeft | kAnchorTop},
                                          {IDC_DECODE_FORMAT, kAnchorLeft | kAnchorTop},
                                          {IDC_EDIT, kAnchorLeft | kAnchorTop | kAnchorRight | kAnchorBottom},
                                          {IDOK, kAnchorRight | kAnchorBottom},
                                          {IDCANCEL, kAnchorRight | kAnchorBottom},
                                      });
        RebuildDecoders(dialog, state);
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
    if (message == WM_SIZE && state)
    {
        state->resizer.Apply(dialog);
        return TRUE;
    }
    if (message == WM_GETMINMAXINFO && state)
    {
        state->resizer.ClampMinSize(reinterpret_cast<MINMAXINFO*>(lparam));
        return TRUE;
    }
    INT_PTR themed = 0;
    if (dialog_support::HandleThemeMessage(dialog, message, wparam, lparam, &themed))
    {
        return themed;
    }
    if (message != WM_COMMAND || !state)
    {
        return FALSE;
    }
    const int id = LOWORD(wparam);
    if (HIWORD(wparam) == CBN_SELCHANGE)
    {
        if (id == IDC_DECODE_ENCODING)
        {
            ApplyTransform(dialog, state);
            return TRUE;
        }
        if (id == IDC_DECODE_FORMAT)
        {
            RunDecoder(dialog, state);
            return TRUE;
        }
    }
    switch (id)
    {
    case IDOK:
        // keep decoder open after copying so other interpretations can be tried
        ui::CopyTextToClipboard(dialog, state->output);
        return TRUE;
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

void ShowValueDecoder(HWND owner, const DecodeRequest& request)
{
    State state;
    state.request = &request;
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_DECODE_VALUE), owner, DialogProc, reinterpret_cast<LPARAM>(&state));
}

} // namespace regkit::editors
