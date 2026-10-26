// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_impl.h"

#include "dialogs/value_editor.h"
#include "ui/feedback.h"
#include "ui/list_view_support.h"
#include "win32/file_dialog.h"
#include "win32/text_transform.h"
#include "win32/translation.h"

#include <win32ui/theme/preset_editor.h>

namespace regkit
{

namespace
{

constexpr wchar_t kThemeFilter[] = L"RegKit Theme Presets (*.rktheme)\0*.rktheme\0All Files (*.*)\0*.*\0";

const wchar_t* PresetEditorText(win32ui::PresetEditorText text)
{
    using Text = win32ui::PresetEditorText;
    switch (text)
    {
    case Text::title:
        return util::Tr(L"Theme Presets");
    case Text::presets:
        return util::Tr(L"Presets");
    case Text::colors:
        return util::Tr(L"Colors");
    case Text::templates:
        return util::Tr(L"Templates");
    case Text::color_column:
        return util::Tr(L"Color");
    case Text::hex_column:
        return util::Tr(L"Hex");
    case Text::dark:
        return util::Tr(L"Treat as dark theme");
    case Text::new_preset:
        return util::Tr(L"New...");
    case Text::duplicate:
        return util::Tr(L"Duplicate");
    case Text::rename:
        return util::Tr(L"Rename...");
    case Text::delete_preset:
        return util::Tr(L"Delete");
    case Text::import_presets:
        return util::Tr(L"Import...");
    case Text::export_presets:
        return util::Tr(L"Export...");
    case Text::edit_color:
        return util::Tr(L"Edit Color...");
    case Text::apply_template:
        return util::Tr(L"Apply Template");
    case Text::apply:
        return util::Tr(L"Apply");
    case Text::ok:
        return util::Tr(L"OK");
    case Text::cancel:
        return util::Tr(L"Cancel");
    // editing a built in creates a new preset
    case Text::new_title:
    case Text::copy_title:
        return util::Tr(L"New Preset");
    case Text::rename_title:
        return util::Tr(L"Rename Preset");
    case Text::copy_suffix:
        return L" Copy";
    case Text::background:
        return util::Tr(L"Background");
    case Text::panel:
        return util::Tr(L"Panel");
    case Text::surface:
        return util::Tr(L"Surface");
    case Text::field:
        return util::Tr(L"Field");
    case Text::header:
        return util::Tr(L"Header");
    case Text::border:
        return util::Tr(L"Border");
    case Text::text:
        return util::Tr(L"Text");
    case Text::muted_text:
        return util::Tr(L"Muted Text");
    case Text::accent:
        return util::Tr(L"Accent");
    case Text::selection:
        return util::Tr(L"Selection");
    case Text::selection_text:
        return util::Tr(L"Selection Text");
    case Text::hover:
        return util::Tr(L"Hover");
    case Text::focus:
        return util::Tr(L"Focus");
    }
    return L"";
}

} // namespace

void MainWindow::Impl::ShowThemePresetsDialog()
{
    if (theme_presets_.empty())
    {
        LoadThemePresets();
    }

    static const win32ui::ListViewHooks list_hooks = appearance::DialogListHooks();
    win32ui::PresetEditorHooks hooks;
    hooks.text = [](win32ui::PresetEditorText text) { return PresetEditorText(text); };
    hooks.prompt_name = [](HWND owner, const wchar_t* title, const std::wstring& initial, std::wstring* name) {
        editors::TextRequest request;
        request.title = title;
        request.label = util::Tr(L"Preset name:");
        request.text = initial;
        editors::TextResult result;
        if (!editors::EditText(owner, request, &result))
        {
            return false;
        }
        // preset reader trims names
        std::wstring trimmed = util::TrimWhitespace(result.text);
        if (trimmed.empty())
        {
            ui::ShowError(owner, util::Tr(L"Preset name can't be empty."));
            return false;
        }
        *name = std::move(trimmed);
        return true;
    };
    hooks.confirm_delete = [](HWND owner, const std::wstring& name) { return ui::ConfirmDelete(owner, util::Tr(L"Delete Preset"), name); };
    hooks.import_presets = [](HWND owner, std::vector<ThemePreset>* presets) {
        std::wstring path;
        if (!ui::PromptOpenFile(owner, kThemeFilter, &path))
        {
            return false;
        }
        std::wstring error;
        if (!ThemePresetStore::ImportFromFile(path, presets, &error))
        {
            ui::ShowError(owner, error.empty() ? util::Tr(L"Failed to import theme presets.") : error);
            return false;
        }
        return true;
    };
    hooks.export_presets = [](HWND owner, const std::vector<ThemePreset>& presets) {
        std::wstring path;
        win32::OpenAfter open_after = win32::OpenAfter::kNone;
        if (!ui::ReportFileDialogResult(owner, win32::ChooseFileToSave(owner, kThemeFilter, nullptr, &path, &open_after)))
        {
            return;
        }
        std::wstring error;
        if (!ThemePresetStore::ExportToFile(path, presets, &error))
        {
            ui::ShowError(owner, error.empty() ? util::Tr(L"Failed to export theme presets.") : error);
        }
        else if (open_after == win32::OpenAfter::kEditor)
        {
            ui::ReportFileDialogResult(owner, win32::OpenInTextEditor(owner, path));
        }
    };
    hooks.apply = [](void* context, const std::vector<ThemePreset>& presets, const std::wstring& active_name) {
        static_cast<MainWindow::Impl*>(context)->UpdateThemePresets(presets, active_name, true);
    };
    hooks.list_hooks = &list_hooks;
    hooks.context = this;
    win32ui::ShowThemePresetEditor(hwnd_, theme_presets_, settings_.theme_preset, &hooks);
}

} // namespace regkit
