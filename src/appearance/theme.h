// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <commctrl.h>

#include "win32/handle_owner.h"

#include <string_view>

namespace regkit
{

enum class ThemeMode
{
    kSystem = 0,
    kLight,
    kDark,
    kCustom,
};

struct ThemeColors
{
    COLORREF background = RGB(0, 0, 0);
    COLORREF panel = RGB(0, 0, 0);
    COLORREF surface = RGB(0, 0, 0);
    COLORREF field = RGB(0, 0, 0);
    COLORREF header = RGB(0, 0, 0);
    COLORREF border = RGB(0, 0, 0);
    COLORREF text = RGB(0, 0, 0);
    COLORREF muted_text = RGB(0, 0, 0);
    COLORREF accent = RGB(0, 0, 0);
    COLORREF selection = RGB(0, 0, 0);
    COLORREF selection_text = RGB(0, 0, 0);
    COLORREF hover = RGB(0, 0, 0);
    COLORREF focus = RGB(0, 0, 0);

    bool operator==(const ThemeColors&) const = default;
};

class Theme
{
  public:
    static Theme& Dark();
    static Theme& Light();
    static Theme& Custom();
    static Theme& Current();
    static void SetCustomColors(const ThemeColors& colors, bool is_dark);
    static void SetMode(ThemeMode mode);
    static ThemeMode Mode();
    static bool UseDarkMode();
    static bool IsSystemDarkMode();
    static bool UpdateFromSystem();
    static void InitializeDarkModeSupport();

    void ApplyToWindow(HWND hwnd) const;
    void ApplyToChildren(HWND hwnd) const;
    void ApplyToTreeView(HWND hwnd) const;
    void ApplyToListView(HWND hwnd) const;
    void ApplyToTabControl(HWND hwnd) const;
    void ApplyToToolbar(HWND hwnd) const;
    void ApplyToComboBox(HWND hwnd) const;
    void ApplyToStatusBar(HWND hwnd) const;

    HBRUSH ControlColor(HDC hdc, HWND target, int type) const;

    HBRUSH BackgroundBrush() const;
    HBRUSH PanelBrush() const;
    HBRUSH SurfaceBrush() const;
    HBRUSH FieldBrush() const;
    HBRUSH HeaderBrush() const;

    COLORREF BackgroundColor() const;
    COLORREF PanelColor() const;
    COLORREF SurfaceColor() const;
    COLORREF FieldColor() const;
    COLORREF HeaderColor() const;
    COLORREF BorderColor() const;
    COLORREF TextColor() const;
    COLORREF MutedTextColor() const;
    COLORREF SelectionColor() const;
    COLORREF SelectionTextColor() const;
    COLORREF HoverColor() const;
    COLORREF FocusColor() const;

  private:
    explicit Theme(const ThemeColors& colors, bool is_dark);
    void SetColors(const ThemeColors& colors, bool is_dark);

    ThemeColors colors_;
    bool is_dark_ = true;
    util::UniqueGdiObject<HBRUSH> background_brush_;
    util::UniqueGdiObject<HBRUSH> panel_brush_;
    util::UniqueGdiObject<HBRUSH> surface_brush_;
    util::UniqueGdiObject<HBRUSH> field_brush_;
    util::UniqueGdiObject<HBRUSH> header_brush_;
};

ThemeMode ParseThemeMode(std::wstring_view name);
const wchar_t* ThemeModeName(ThemeMode mode);
void EnableImmersiveDarkMode(HWND hwnd, bool enabled);
void AllowDarkModeForWindow(HWND hwnd, bool enabled);
void SetDarkWindowTheme(HWND hwnd, bool dark, const wchar_t* dark_theme = L"DarkMode_Explorer", const wchar_t* light_theme = L"Explorer");
void EnsureSubclass(HWND hwnd, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR data = 0);

} // namespace regkit
