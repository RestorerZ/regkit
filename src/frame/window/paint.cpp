// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"

#include "ui/autocomplete.h"
#include "ui/dialog_layout.h"

namespace regkit
{
using namespace window_detail;

void MainWindow::Impl::OnSize(int width, int height)
{
    LayoutControls(width, height);
}

void MainWindow::Impl::OnPrintClient(HDC mem_dc)
{
    const Theme& theme = Theme::Current();
    RECT client = {};
    GetClientRect(hwnd_, &client);
    FillRect(mem_dc, &client, theme.BackgroundBrush());

    HPEN pen = appearance::CachedPen(theme.BorderColor(), 1);
    HPEN old_pen = reinterpret_cast<HPEN>(SelectObject(mem_dc, pen));
    HBRUSH old_brush = reinterpret_cast<HBRUSH>(SelectObject(mem_dc, GetStockObject(NULL_BRUSH)));

    if (show_value_)
    {
        tree_splitter_.Paint(mem_dc, theme.PanelBrush());
    }
    history_splitter_.Paint(mem_dc, theme.PanelBrush());

    SelectObject(mem_dc, old_brush);
    SelectObject(mem_dc, old_pen);
}

void MainWindow::Impl::PaintMenuBarSeparator()
{
    if (!hwnd_ || !GetMenu(hwnd_))
    {
        return;
    }

    MENUBARINFO menu_info = {};
    menu_info.cbSize = sizeof(menu_info);
    if (!GetMenuBarInfo(hwnd_, OBJID_MENU, 0, &menu_info))
    {
        return;
    }

    RECT window_rect = {};
    if (!GetWindowRect(hwnd_, &window_rect))
    {
        return;
    }

    RECT separator = menu_info.rcBar;
    OffsetRect(&separator, -window_rect.left, -window_rect.top);
    separator.top = separator.bottom - 1;
    separator.bottom += 1;
    if (separator.bottom <= separator.top)
    {
        return;
    }

    HDC hdc = GetWindowDC(hwnd_);
    if (!hdc)
    {
        return;
    }
    FillRect(hdc, &separator, Theme::Current().BackgroundBrush());
    ReleaseDC(hwnd_, hdc);
}

void MainWindow::Impl::ApplyThemeToChildren()
{
    const Theme& theme = Theme::Current();

    theme.ApplyToToolbar(toolbar_.hwnd());
    theme.ApplyToTreeView(browse_.tree().hwnd());
    appearance::RefreshListView(browse_.values().hwnd());
    appearance::RefreshListView(history_list_);
    appearance::RefreshListView(search_results_list_);
    theme.ApplyToTabControl(tab_);
    theme.ApplyToStatusBar(status_bar_);
    if (IsWindow(key_handles_window_))
    {
        appearance::ApplyDialogTheme(key_handles_window_);
    }

    if (browse_.address())
    {
        SetDarkWindowTheme(browse_.address(), Theme::UseDarkMode());
        SetEditMargins(browse_.address(), 6, 6);
        SetEditVerticalRect(browse_.address(), ui_font_, 2, 6, 6);
    }
    if (browse_.filter())
    {
        SetDarkWindowTheme(browse_.filter(), Theme::UseDarkMode());
        SetEditMargins(browse_.filter(), 6, 6);
        SetEditVerticalRect(browse_.filter(), ui_font_, 2, 6, 6);
    }
    if (tree_header_)
    {
        SetWindowTheme(tree_header_, L"", L"");
    }
    appearance::ApplyAutoCompleteTheme();
    DrawMenuBar(hwnd_);
}

void MainWindow::Impl::ApplySystemTheme()
{
    if (applying_theme_)
    {
        return;
    }
    applying_theme_ = true;
    Theme::UpdateFromSystem();
    Theme::Current().ApplyToWindow(hwnd_);
    ApplyThemeToChildren();
    ReloadThemeIcons();
    if (hwnd_)
    {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    applying_theme_ = false;
}

void MainWindow::Impl::LoadThemePresets()
{
    std::vector<ThemePreset> presets;
    std::wstring load_error;
    bool loaded = ThemePresetStore::Load(&presets, &load_error);
    if (!loaded && !load_error.empty())
    {
        ui::ShowError(hwnd_, load_error);
    }
    bool updated_builtins = false;
    if (!loaded || presets.empty())
    {
        presets = ThemePresetStore::BuiltInPresets();
    }
    else
    {
        std::vector<ThemePreset> builtins = ThemePresetStore::BuiltInPresets();
        for (const auto& builtin : builtins)
        {
            auto it = std::find_if(presets.begin(), presets.end(), [&](const ThemePreset& existing) {
                return EqualsInsensitive(existing.name, builtin.name);
            });
            if (it == presets.end())
            {
                presets.push_back(builtin);
                updated_builtins = true;
            }
            else if (it->is_dark != builtin.is_dark || it->colors != builtin.colors)
            {
                *it = builtin;
                updated_builtins = true;
            }
        }
    }
    theme_presets_ = std::move(presets);
    if (theme_presets_.empty())
    {
        return;
    }
    settings_.theme_preset = FindThemePreset(theme_presets_, settings_.theme_preset)->name;
    if (!loaded || updated_builtins)
    {
        SaveThemePresets();
    }
}

void MainWindow::Impl::SaveThemePresets() const
{
    ThemePresetStore::Save(theme_presets_, nullptr);
}

bool MainWindow::Impl::ApplyThemePresetByName(const std::wstring& name, bool persist)
{
    if (theme_presets_.empty())
    {
        return false;
    }
    const ThemePreset* it = FindThemePreset(theme_presets_, name);
    Theme::SetCustomColors(it->colors, it->is_dark);
    theme_mode_ = ThemeMode::kCustom;
    settings_.theme_preset = it->name;
    Theme::SetMode(theme_mode_);
    ApplySystemTheme();
    if (persist)
    {
        SaveSettings();
        BuildMenus();
    }
    return true;
}

void MainWindow::Impl::UpdateThemePresets(const std::vector<ThemePreset>& presets, const std::wstring& active_name, bool apply_now)
{
    theme_presets_ = presets;
    settings_.theme_preset = active_name;
    SaveThemePresets();
    if (apply_now)
    {
        ApplyThemePresetByName(settings_.theme_preset, true);
    }
    else
    {
        SaveSettings();
        BuildMenus();
    }
}

void MainWindow::Impl::ApplyAlwaysOnTop()
{
    if (!hwnd_)
    {
        return;
    }
    SetWindowPos(hwnd_, settings_.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

void MainWindow::Impl::UpdateUIFont()
{
    ui::SetCustomFont(settings_.use_custom_font ? &custom_font_ : nullptr);
    const LOGFONTW lf = ui::DefaultUIFontLogFont(win32::DpiForWindow(hwnd_));
    HFONT next_font = CreateFontIndirectW(&lf);
    bool next_owned = next_font != nullptr;
    if (!next_font)
    {
        next_font = CreateUIFont();
        next_owned = false;
    }
    if (ui_font_ && ui_font_owned_)
    {
        DeleteObject(ui_font_);
    }
    ui_font_ = next_font;
    ui_font_owned_ = next_owned;
    ApplyUIFontToControls();
}

void MainWindow::Impl::ApplyUIFontToControls()
{
    if (!ui_font_)
    {
        return;
    }
    ApplyFont(toolbar_.hwnd(), ui_font_);
    ApplyFont(browse_.address(), ui_font_);
    ApplyFont(browse_.go_button(), ui_font_);
    ApplyFont(browse_.filter(), ui_font_);
    ApplyFont(tab_, ui_font_);
    ApplyFont(tree_header_, ui_font_);
    ApplyFont(tree_close_btn_, ui_font_);
    ApplyFont(browse_.tree().hwnd(), ui_font_);
    ApplyFont(browse_.values().hwnd(), ui_font_);
    ApplyFont(history_close_btn_, ui_font_);
    ApplyFont(history_label_, ui_font_);
    ApplyFont(history_list_, ui_font_);
    ApplyFont(status_bar_, ui_font_);
    ApplyFont(search_results_list_, ui_font_);
    if (tab_)
    {
        tab_height_ = tab_strip_.Refit(kTabMinWidth);
    }
    if (status_bar_)
    {
        SendMessageW(status_bar_, SB_SETMINHEIGHT, TextRowHeight(0, 0), 0);
        SendMessageW(status_bar_, WM_SIZE, 0, 0);
    }
    if (hwnd_)
    {
        DrawMenuBar(hwnd_);
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (hwnd_)
    {
        RECT rect = {};
        GetClientRect(hwnd_, &rect);
        LayoutControls(rect.right, rect.bottom);
    }
}

} // namespace regkit
