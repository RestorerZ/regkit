// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "appearance/dialog_layout.h"

#include "appearance/default_font.h"
#include "appearance/dialog_metrics.h"
#include "appearance/gdi_cache.h"
#include "appearance/list_view_support.h"
#include "appearance/theme.h"
#include "win32/text_transform.h"
#include "win32/translation.h"
#include "win32/window_metrics.h"

#include <algorithm>
#include <commctrl.h>

#ifndef DCX_USESTYLE
#define DCX_USESTYLE 0x00010000
#endif
#ifndef DCX_NODELETERGN
#define DCX_NODELETERGN 0x00040000
#endif
#ifndef HRGN_FULL
#define HRGN_FULL reinterpret_cast<HRGN>(1)
#endif

namespace regkit::appearance
{

namespace
{

constexpr UINT_PTR kThemedBorderSubclassId = 31;

LRESULT CALLBACK ThemedBorderProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR)
{
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(hwnd, ThemedBorderProc, id);
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }
    if (message != WM_NCPAINT)
    {
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }
    const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
    HRGN region = reinterpret_cast<HRGN>(wparam);
    UINT flags = DCX_WINDOW | DCX_CACHE | DCX_USESTYLE;
    if (region == HRGN_FULL)
    {
        region = nullptr;
    }
    else if (region)
    {
        flags |= DCX_INTERSECTRGN | DCX_NODELETERGN;
    }
    HDC hdc = GetDCEx(hwnd, region, flags);
    if (!hdc)
    {
        return result;
    }
    RECT frame = {};
    if (GetWindowRect(hwnd, &frame))
    {
        OffsetRect(&frame, -frame.left, -frame.top);
        FrameRect(hdc, &frame, CachedBrush(Theme::Current().BorderColor()));
    }
    ReleaseDC(hwnd, hdc);
    return result;
}

} // namespace

void AttachThemedBorder(HWND control)
{
    if (!control || GetWindowSubclass(control, ThemedBorderProc, kThemedBorderSubclassId, nullptr))
    {
        return;
    }
    if (!SetWindowSubclass(control, ThemedBorderProc, kThemedBorderSubclassId, 0))
    {
        return;
    }
    SetWindowLongPtrW(control, GWL_EXSTYLE, GetWindowLongPtrW(control, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
    SetWindowLongPtrW(control, GWL_STYLE, GetWindowLongPtrW(control, GWL_STYLE) | WS_BORDER);
    SetWindowPos(control, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

HWND CreateControl(HWND parent, const wchar_t* class_name, const wchar_t* text, DWORD style, int id)
{
    return CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
}

void SetControlFont(HWND control, HFONT font)
{
    if (control && font)
    {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void SetDialogFont(HWND dialog, HFONT font)
{
    SetControlFont(dialog, font);
    EnumChildWindows(
        dialog,
        [](HWND child, LPARAM param) -> BOOL {
            SetControlFont(child, reinterpret_cast<HFONT>(param));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(font)
    );
}

void Place(HWND control, int x, int y, int width, int height)
{
    if (control)
    {
        SetWindowPos(control, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void RestoreDialogOwner(HWND owner, bool* restored)
{
    if (!owner || !restored || *restored)
    {
        return;
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    SetForegroundWindow(owner);
    *restored = true;
}

void CenterWindow(HWND window, HWND owner)
{
    RECT rect = {};
    if (!window || !GetWindowRect(window, &rect))
    {
        return;
    }
    RECT target = {};
    if ((!owner || !GetWindowRect(owner, &target)) && !SystemParametersInfoW(SPI_GETWORKAREA, 0, &target, 0))
    {
        return;
    }
    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    SetWindowPos(window, nullptr, target.left + std::max<LONG>(0, (target.right - target.left - width) / 2), target.top + std::max<LONG>(0, (target.bottom - target.top - height) / 2), 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RefreshDialogFont(HWND window, HFONT* owned_font, UINT dpi)
{
    if (!window || !owned_font)
    {
        return;
    }
    HFONT font = ui::DefaultUIFont(dpi);
    if (!font)
    {
        return;
    }
    SetDialogFont(window, font);
    if (*owned_font)
    {
        DeleteObject(*owned_font);
    }
    *owned_font = font;
}

void ApplyDpiChange(HWND window, LPARAM suggested_rect)
{
    const RECT* rect = reinterpret_cast<const RECT*>(suggested_rect);
    if (!window || !rect)
    {
        return;
    }
    SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void CenterEditText(HWND edit, HFONT font, int left_pad, int right_pad)
{
    if (!edit || !font)
    {
        return;
    }
    RECT rect = {};
    GetClientRect(edit, &rect);
    HDC hdc = GetDC(edit);
    if (!hdc)
    {
        return;
    }
    HFONT old = reinterpret_cast<HFONT>(SelectObject(hdc, font));
    TEXTMETRICW tm = {};
    const bool ok = GetTextMetricsW(hdc, &tm) != FALSE;
    SelectObject(hdc, old);
    ReleaseDC(edit, hdc);
    if (!ok)
    {
        return;
    }
    const int line = static_cast<int>(tm.tmHeight + tm.tmExternalLeading);
    const int pad = std::max(0, (static_cast<int>(rect.bottom - rect.top) - line) / 2);
    rect.left += left_pad;
    rect.right -= right_pad;
    rect.top += pad;
    rect.bottom = rect.top + line;
    SendMessageW(edit, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&rect));
}

int TextFitWidth(HWND control)
{
    wchar_t class_name[16] = {};
    GetClassNameW(control, class_name, static_cast<int>(_countof(class_name)));
    const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
    if (util::EqualsInsensitive(class_name, WC_BUTTONW))
    {
        const LONG_PTR type = style & BS_TYPEMASK;
        SIZE ideal = {};
        if (type == BS_GROUPBOX || type == BS_OWNERDRAW || !SendMessageW(control, BCM_GETIDEALSIZE, 0, reinterpret_cast<LPARAM>(&ideal)))
        {
            return 0;
        }
        // themed check and radio buttons pad glyph and text wider than the native ideal size
        const bool push = type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON;
        return ideal.cx + (push ? 0 : metrics::Scaled(12, win32::DpiForWindow(control)));
    }
    const LONG_PTR type = style & SS_TYPEMASK;
    if (!util::EqualsInsensitive(class_name, WC_STATICW) || (type != SS_LEFT && type != SS_CENTER && type != SS_RIGHT && type != SS_LEFTNOWORDWRAP && type != SS_SIMPLE))
    {
        return 0;
    }
    const std::wstring text = util::WindowText(control);
    HDC hdc = GetDC(control);
    HGDIOBJ old_font = SelectObject(hdc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
    RECT needed = {};
    DrawTextW(hdc, text.c_str(), -1, &needed, DT_CALCRECT | DT_SINGLELINE | ((style & SS_NOPREFIX) ? DT_NOPREFIX : 0));
    SelectObject(hdc, old_font);
    ReleaseDC(control, hdc);
    RECT rect = {};
    GetClientRect(control, &rect);
    // wrapping labels lay out across lines
    return rect.bottom >= 2 * needed.bottom ? 0 : needed.right;
}

void LocalizeDialog(HWND dialog)
{
    if (!util::TranslateDialog(dialog))
    {
        return;
    }
    struct Control
    {
        HWND hwnd = nullptr;
        RECT rect = {};
        RECT initial = {};
        int fit = 0;
        bool field = false;
        bool push = false;
        bool group = false;
        const Control* container = nullptr;
        LONG left_limit = 0;
        LONG right_limit = 0;
    };
    std::vector<Control> controls;
    for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    {
        const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
        if (!(style & WS_VISIBLE))
        {
            continue;
        }
        wchar_t class_name[16] = {};
        GetClassNameW(child, class_name, static_cast<int>(_countof(class_name)));
        const bool button = util::EqualsInsensitive(class_name, WC_BUTTONW);
        Control control;
        control.hwnd = child;
        GetWindowRect(child, &control.rect);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&control.rect), 2);
        control.initial = control.rect;
        control.fit = TextFitWidth(child);
        // empty labels hold data filled in later and lay out like input fields
        control.field = !button && (!util::EqualsInsensitive(class_name, WC_STATICW) || GetWindowTextLengthW(child) == 0);
        control.push = button && ((style & BS_TYPEMASK) == BS_PUSHBUTTON || (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON);
        control.group = button && (style & BS_TYPEMASK) == BS_GROUPBOX;
        controls.push_back(control);
    }
    RECT client = {};
    GetClientRect(dialog, &client);
    const int gap = metrics::Scaled(4, win32::DpiForWindow(dialog));
    auto width = [](const RECT& rect) { return static_cast<int>(rect.right - rect.left); };
    // fields keep at least half of their designed width
    auto shrinkable = [&](const Control& field) { return std::max(0, width(field.rect) - width(field.initial) / 2); };
    // rows are bounded by the innermost group box around them
    for (Control& control : controls)
    {
        control.left_limit = client.left + gap;
        control.right_limit = client.right - gap;
        for (const Control& group : controls)
        {
            const RECT& box = group.rect;
            if (group.group && &group != &control && box.left <= control.rect.left && box.right >= control.rect.right && box.top <= control.rect.top &&
                box.bottom >= control.rect.bottom && (!control.container || width(box) < width(control.container->rect)))
            {
                control.container = &group;
                control.left_limit = box.left + gap;
                control.right_limit = box.right - gap;
            }
        }
    }
    for (Control& control : controls)
    {
        int missing = control.fit - width(control.rect);
        if (missing <= 0)
        {
            continue;
        }
        std::vector<Control*> row;
        for (Control& other : controls)
        {
            if (&other != &control && !other.group && other.container == control.container && other.rect.top < control.rect.bottom &&
                other.rect.bottom > control.rect.top)
            {
                row.push_back(&other);
            }
        }
        // the control pushes its right neighbours into wider gaps and the free space before the row end
        std::vector<Control*> right_side;
        for (Control* other : row)
        {
            if (other->rect.left >= control.rect.right)
            {
                right_side.push_back(other);
            }
        }
        std::sort(right_side.begin(), right_side.end(), [](const Control* a, const Control* b) { return a->rect.left < b->rect.left; });
        // fields sharing a column with other fields stay put so the column remains aligned, the step below shrinks them instead
        const auto first_field = std::find_if(right_side.begin(), right_side.end(), [&](const Control* other) {
            return other->field && std::count_if(controls.begin(), controls.end(), [&](const Control& field) { return field.field && field.rect.left == other->rect.left; }) > 1;
        });
        Control* next = first_field != right_side.end() ? *first_field : right_side.empty() ? nullptr
                                                                                            : right_side.front();
        const LONG end = first_field != right_side.end() ? (*first_field)->rect.left - gap : control.right_limit;
        right_side.erase(first_field, right_side.end());
        std::vector<LONG> spacing;
        std::vector<LONG> slack;
        LONG previous = control.rect.right;
        int available = 0;
        for (const Control* other : right_side)
        {
            // labels, check and radio buttons wider than their text give up the excess
            const int fit = other->push ? 0 : other->fit;
            spacing.push_back(std::min(static_cast<LONG>(gap), other->rect.left - previous));
            slack.push_back(fit > 0 ? std::max(0, width(other->rect) - fit) : 0);
            available += other->rect.left - previous - spacing.back() + slack.back();
            previous = other->rect.right;
        }
        available += std::max(0L, end - previous);
        const int grow = std::clamp(available, 0, missing);
        control.rect.right += grow;
        missing -= grow;
        previous = control.rect.right;
        for (size_t i = 0; i < right_side.size(); ++i)
        {
            RECT& rect = right_side[i]->rect;
            const LONG move = std::max(0L, previous + spacing[i] - rect.left);
            rect.left += move;
            rect.right += move - std::min(move, slack[i]);
            previous = rect.right;
        }
        // an input field on the right gives up width, fields starting in the same column follow it
        if (missing > 0 && next && next->field)
        {
            const LONG column = next->rect.left;
            int take = missing;
            for (const Control& field : controls)
            {
                take = field.field && field.rect.left == column ? std::min(take, shrinkable(field)) : take;
            }
            for (Control& field : controls)
            {
                field.rect.left += field.field && field.rect.left == column ? take : 0;
            }
            control.rect.right += take;
            missing -= take;
        }
        // a push button moves its neighbouring buttons into free space on the left
        if (missing > 0 && control.push)
        {
            std::vector<Control*> cluster = {&control};
            for (bool added = true; added;)
            {
                added = false;
                for (Control* other : row)
                {
                    if (other->push && std::find(cluster.begin(), cluster.end(), other) == cluster.end() && other->rect.right <= cluster.back()->rect.left &&
                        other->rect.right >= cluster.back()->rect.left - 3 * gap)
                    {
                        cluster.push_back(other);
                        added = true;
                    }
                }
            }
            LONG limit = control.left_limit;
            Control* before = nullptr;
            for (Control* other : row)
            {
                if (std::find(cluster.begin(), cluster.end(), other) == cluster.end() && other->rect.right <= cluster.back()->rect.left && other->rect.right + gap > limit)
                {
                    limit = other->rect.right + gap;
                    before = other;
                }
            }
            int take = std::clamp(static_cast<int>(cluster.back()->rect.left - limit), 0, missing);
            // an input field on the left gives up width from its right edge
            if (before && before->field)
            {
                const int shrink = std::min(missing - take, shrinkable(*before));
                before->rect.right -= shrink;
                take += shrink;
            }
            for (Control* member : cluster)
            {
                member->rect.left -= take;
                member->rect.right -= member == &control ? 0 : take;
            }
        }
    }
    for (const Control& control : controls)
    {
        if (!EqualRect(&control.rect, &control.initial))
        {
            SetWindowPos(control.hwnd, nullptr, control.rect.left, control.rect.top, width(control.rect), control.rect.bottom - control.rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

void FitDialogHeight(HWND dialog, int client_height)
{
    if (!dialog || client_height <= 0)
    {
        return;
    }
    RECT client = {};
    if (!GetClientRect(dialog, &client) || client.bottom - client.top == client_height)
    {
        return;
    }
    RECT want = {0, 0, client.right - client.left, client_height};
    win32::AdjustWindowRectForDpi(&want, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)), static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)), win32::DpiForWindow(dialog));
    SetWindowPos(dialog, nullptr, 0, 0, want.right - want.left, want.bottom - want.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void RunModalLoop(HWND dialog)
{
    MSG msg = {};
    while (IsWindow(dialog))
    {
        const BOOL available = GetMessageW(&msg, nullptr, 0, 0);
        if (available == -1)
        {
            break;
        }
        if (available == 0)
        {
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        const LONG_PTR style = GetWindowLongPtrW(msg.hwnd, GWL_STYLE);
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            (style & (ES_MULTILINE | ES_WANTRETURN)) == ES_MULTILINE && IsChild(dialog, msg.hwnd))
        {
            wchar_t class_name[8] = {};
            const LRESULT default_id = SendMessageW(dialog, DM_GETDEFID, 0, 0);
            if (GetClassNameW(msg.hwnd, class_name, static_cast<int>(_countof(class_name))) &&
                util::EqualsInsensitive(class_name, WC_EDITW) && HIWORD(default_id) == DC_HASDEFID)
            {
                const WORD id = LOWORD(default_id);
                SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dialog, id)));
                continue;
            }
        }
        if (!IsDialogMessageW(dialog, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

namespace
{

constexpr DWORD kDialogWindowStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
constexpr DWORD kDialogWindowExStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;

} // namespace

SIZE DialogWindowSize(HWND owner, int client_width, int client_height, DWORD extra_style)
{
    RECT rect = {0, 0, client_width, client_height};
    win32::AdjustWindowRectForDpi(&rect, kDialogWindowStyle | extra_style, kDialogWindowExStyle, win32::DpiForWindow(owner));
    return {rect.right - rect.left, rect.bottom - rect.top};
}

void ApplyDialogTheme(HWND dialog)
{
    const Theme& theme = Theme::Current();
    theme.ApplyToWindow(dialog);
    theme.ApplyToChildren(dialog);
    EnumChildWindows(
        dialog,
        [](HWND child, LPARAM) -> BOOL {
            wchar_t class_name[32] = {};
            GetClassNameW(child, class_name, static_cast<int>(_countof(class_name)));
            if (wcscmp(class_name, WC_TREEVIEWW) == 0)
            {
                Theme::Current().ApplyToTreeView(child);
            }
            else if (wcscmp(class_name, WC_LISTVIEWW) == 0)
            {
                RefreshListView(child);
            }
            return TRUE;
        },
        0
    );
    InvalidateRect(dialog, nullptr, TRUE);
}

void CloseDialogWindow(DialogWindow* dialog, bool accepted)
{
    dialog->accepted = accepted;
    RestoreDialogOwner(dialog->owner, &dialog->owner_restored);
    DestroyWindow(dialog->hwnd);
}

LRESULT DefDialogWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    DialogWindow* dialog = DialogWindowState<DialogWindow>(hwnd);
    switch (message)
    {
    case WM_NCCREATE:
        dialog = static_cast<DialogWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd = hwnd;
        dialog->font = ui::DefaultUIFont(win32::DpiForWindow(hwnd));
        break;
    case WM_NCDESTROY:
        if (dialog && dialog->font)
        {
            DeleteObject(dialog->font);
            dialog->font = nullptr;
        }
        break;
    case WM_DPICHANGED:
        if (dialog)
        {
            RefreshDialogFont(hwnd, &dialog->font, LOWORD(wparam));
        }
        ApplyDpiChange(hwnd, lparam);
        return 0;
    case WM_SETTINGCHANGE:
        if (Theme::UpdateFromSystem())
        {
            ApplyDialogTheme(hwnd);
        }
        return 0;
    case WM_ERASEBKGND:
        {
            RECT rect = {};
            GetClientRect(hwnd, &rect);
            FillRect(reinterpret_cast<HDC>(wparam), &rect, Theme::Current().BackgroundBrush());
            return TRUE;
        }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(Theme::Current().ControlColor(reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam), static_cast<int>(message - WM_CTLCOLORMSGBOX)));
    case DM_GETDEFID:
        return MAKELRESULT(dialog ? dialog->default_id : IDOK, DC_HASDEFID);
    case WM_NEXTDLGCTL:
        if (HWND next = LOWORD(lparam) ? reinterpret_cast<HWND>(wparam) : GetNextDlgTabItem(hwnd, GetFocus(), wparam != 0))
        {
            SetFocus(next);
            if (!LOWORD(lparam) && (SendMessageW(next, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL))
            {
                SendMessageW(next, EM_SETSEL, 0, -1);
            }
        }
        return 0;
    case WM_ACTIVATE:
        if (dialog && LOWORD(wparam) == WA_INACTIVE)
        {
            HWND focus = GetFocus();
            dialog->focus = IsChild(hwnd, focus) ? focus : dialog->focus;
        }
        else if (dialog)
        {
            HWND focus =
                dialog->focus && IsWindow(dialog->focus) ? dialog->focus : GetNextDlgTabItem(hwnd, nullptr, FALSE);
            if (focus)
            {
                SetFocus(focus);
                return 0;
            }
        }
        break;
    case WM_CLOSE:
        SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
        return 0;
    case WM_COMMAND:
        if (dialog && LOWORD(wparam) == IDCANCEL)
        {
            CloseDialogWindow(dialog, false);
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool RunDialogWindow(DialogWindow* dialog, const wchar_t* class_name, WNDPROC proc, const wchar_t* title, SIZE size, DWORD extra_style)
{
    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = class_name;
    RegisterClassW(&window_class);
    const HWND hwnd =
        CreateWindowExW(kDialogWindowExStyle, class_name, title, kDialogWindowStyle | extra_style, CW_USEDEFAULT, CW_USEDEFAULT, size.cx, size.cy, dialog->owner, nullptr, window_class.hInstance, dialog);
    if (!hwnd)
    {
        return false;
    }
    ApplyDialogTheme(hwnd);
    CenterWindow(hwnd, dialog->owner);
    EnableWindow(dialog->owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    RunModalLoop(hwnd);
    RestoreDialogOwner(dialog->owner, &dialog->owner_restored);
    return dialog->accepted;
}

void DialogResizer::Attach(HWND dialog, std::initializer_list<AnchorRule> rules)
{
    items_.clear();
    client_ = {};
    min_window_ = {};
    if (!dialog)
    {
        return;
    }

    RECT client = {};
    RECT window = {};
    if (!GetClientRect(dialog, &client) || !GetWindowRect(dialog, &window))
    {
        return;
    }
    client_ = {client.right - client.left, client.bottom - client.top};
    min_window_ = {window.right - window.left, window.bottom - window.top};

    items_.reserve(rules.size());
    for (const AnchorRule& rule : rules)
    {
        HWND control = GetDlgItem(dialog, rule.id);
        RECT rect = {};
        if (!control || !GetWindowRect(control, &rect))
        {
            continue;
        }
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
        items_.push_back({rule.id, rule.anchors, rect});
    }
}

void DialogResizer::Apply(HWND dialog) const
{
    if (!dialog || items_.empty() || client_.cx <= 0 || client_.cy <= 0)
    {
        return;
    }
    RECT client = {};
    if (!GetClientRect(dialog, &client))
    {
        return;
    }
    const LONG dx = (client.right - client.left) - client_.cx;
    const LONG dy = (client.bottom - client.top) - client_.cy;

    HDWP defer = BeginDeferWindowPos(static_cast<int>(items_.size()));
    for (const Item& item : items_)
    {
        HWND control = GetDlgItem(dialog, item.id);
        if (!control)
        {
            continue;
        }
        const LONG left = (item.anchors & kAnchorLeft) ? item.rect.left : item.rect.left + dx;
        const LONG right = (item.anchors & kAnchorRight) ? item.rect.right + dx : item.rect.right;
        const LONG top = (item.anchors & kAnchorTop) ? item.rect.top : item.rect.top + dy;
        const LONG bottom = (item.anchors & kAnchorBottom) ? item.rect.bottom + dy : item.rect.bottom;
        const int width = static_cast<int>(std::max<LONG>(0, right - left));
        const int height = static_cast<int>(std::max<LONG>(0, bottom - top));
        if (defer)
        {
            defer = DeferWindowPos(defer, control, nullptr, static_cast<int>(left), static_cast<int>(top), width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else
        {
            SetWindowPos(control, nullptr, static_cast<int>(left), static_cast<int>(top), width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (defer)
    {
        EndDeferWindowPos(defer);
    }
    InvalidateRect(dialog, nullptr, TRUE);
}

void DialogResizer::ClampMinSize(MINMAXINFO* info) const
{
    if (!info || min_window_.cx <= 0 || min_window_.cy <= 0)
    {
        return;
    }
    info->ptMinTrackSize.x = std::max<LONG>(info->ptMinTrackSize.x, min_window_.cx);
    info->ptMinTrackSize.y = std::max<LONG>(info->ptMinTrackSize.y, min_window_.cy);
}

} // namespace regkit::appearance
