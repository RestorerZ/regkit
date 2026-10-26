// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"
#include "win32/text_transform.h"

#include <win32ui/process_token.h>
#include <win32ui/window_metrics.h>

namespace regkit
{
using namespace window_detail;

MainWindow::Impl::~Impl() = default;

bool MainWindow::Impl::Create(HINSTANCE instance)
{
    instance_ = instance;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWindow::Impl::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kMainWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm =
        static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hbrBackground = nullptr;

    RegisterClassExW(&wc);

    std::wstring title = L"RegKit V";
    title.append(REGKIT_VERSION_TEXT);
    if (win32ui::IsProcessTrustedInstaller())
    {
        title.append(L" - [TrustedInstaller]");
    }
    else if (win32ui::IsProcessSystem())
    {
        title.append(L" - [SYSTEM]");
    }
    else if (win32ui::IsProcessElevated())
    {
        title.append(L" - [").append(util::Tr(L"Administrator")).append(L"]");
    }
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, nullptr, nullptr, instance, this);
    if (hwnd_)
    {
        SetPropW(hwnd_, kRegKitWindowProperty, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
        DragAcceptFiles(hwnd_, TRUE);
    }
    return hwnd_ != nullptr;
}

void MainWindow::Impl::Show(int cmd_show)
{
    int show_cmd = cmd_show;
    if (window_placement_loaded_ && settings_.window_width > 0 && settings_.window_height > 0)
    {
        show_cmd = settings_.window_maximized ? SW_MAXIMIZE : SW_SHOWNORMAL;
    }
    else if (window_placement_loaded_ && settings_.window_maximized)
    {
        show_cmd = SW_MAXIMIZE;
    }
    if (show_cmd != SW_MAXIMIZE)
    {
        RECT rect = {};
        if (GetWindowRect(hwnd_, &rect))
        {
            RECT fitted = rect;
            win32ui::ClampToWorkArea(&fitted);
            if (!EqualRect(&fitted, &rect))
            {
                SetWindowPos(hwnd_, nullptr, fitted.left, fitted.top, fitted.right - fitted.left, fitted.bottom - fitted.top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    pending_show_cmd_ = show_cmd;
    PostMessageW(hwnd_, frame::message_id::kDeferredStartup, 0, 0);
    PostMessageW(hwnd_, frame::message_id::kLoadTraces, 0, 0);
    PostMessageW(hwnd_, frame::message_id::kLoadDefaults, 0, 0);
}

void MainWindow::Impl::QueueExternalJump(const std::wstring& target)
{
    queued_external_jump_target_ = target;
}

void MainWindow::Impl::BeginJumpUiBatch()
{
    if (jump_ui_batch_active_)
    {
        return;
    }
    jump_ui_batch_active_ = true;
    browse_.tree().SuspendRedraw();
    if (browse_.values().hwnd())
    {
        SendMessageW(browse_.values().hwnd(), WM_SETREDRAW, FALSE, 0);
    }
}

void MainWindow::Impl::EndJumpUiBatch()
{
    if (!jump_ui_batch_active_)
    {
        return;
    }
    jump_ui_batch_active_ = false;
    browse_.tree().ResumeRedraw();
    if (browse_.values().hwnd())
    {
        SendMessageW(browse_.values().hwnd(), WM_SETREDRAW, TRUE, 0);
        InvalidateRect(browse_.values().hwnd(), nullptr, TRUE);
    }
}

void MainWindow::Impl::ApplyTreeSelectionEffects(RegistryNode* node)
{
    ResetValueFilter();
    UpdateAddressBar(node);
    UpdateValueListForNode(node);
    MarkTreeStateDirty();
}

void MainWindow::Impl::FocusAddressBarForExternalJump(bool defer_if_needed)
{
    if (!hwnd_)
    {
        return;
    }
    HWND target = browse_.address();
    if (!target)
    {
        return;
    }
    if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_))
    {
        SetFocus(target);
        SendMessageW(target, EM_SETSEL, 0, -1);
        return;
    }
    if (defer_if_needed)
    {
        PostMessageW(hwnd_, frame::message_id::kFocusAddressBar, 0, 0);
    }
}

void MainWindow::Impl::CyclePaneFocus(bool forward)
{
    HWND panes[] = {toolbar_, browse_.address(), tab_, browse_.filter(), browse_.tree().hwnd(), tree_splitter_, browse_.values().hwnd(), search_results_list_, history_splitter_, history_list_};
    HWND visible[_countof(panes)] = {};
    int count = 0;
    for (HWND pane : panes)
    {
        // panes without layout space have no usable content
        RECT area = {};
        if (pane && IsWindowVisible(pane) && IsWindowEnabled(pane) && GetClientRect(pane, &area) && !IsRectEmpty(&area))
        {
            visible[count++] = pane;
        }
    }
    if (count == 0)
    {
        return;
    }
    HWND focus = GetFocus();
    int current = -1;
    for (int i = 0; i < count; ++i)
    {
        if (visible[i] == focus || IsChild(visible[i], focus))
        {
            current = i;
            break;
        }
    }
    const int next = current < 0 ? (forward ? 0 : count - 1) : (current + (forward ? 1 : count - 1)) % count;
    // keyboard moves focus, panes show it
    SendMessageW(hwnd_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
    FocusPane(visible[next]);
}

void MainWindow::Impl::FocusPane(HWND pane)
{
    if (!pane)
    {
        return;
    }
    SetFocus(pane);
    if (pane != browse_.values().hwnd() && pane != search_results_list_ && pane != history_list_)
    {
        return;
    }
    if (ListView_GetItemCount(pane) <= 0 || ListView_GetNextItem(pane, -1, LVNI_FOCUSED) >= 0 ||
        ListView_GetSelectedCount(pane) > 0)
    {
        return;
    }
    SelectListRowAtIndex(pane, 0);
}

namespace
{

bool IsTextInput(HWND hwnd)
{
    if (!hwnd)
    {
        return false;
    }
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, static_cast<int>(_countof(cls)));
    if (util::EqualsInsensitive(cls, L"Edit"))
    {
        return true;
    }
    if (util::EqualsInsensitive(cls, L"RichEdit20W") || util::EqualsInsensitive(cls, L"RichEdit20A"))
    {
        return true;
    }
    if (util::EqualsInsensitive(cls, L"ComboBox") || util::EqualsInsensitive(cls, L"ComboBoxEx32"))
    {
        return true;
    }
    HWND parent = GetParent(hwnd);
    if (parent)
    {
        GetClassNameW(parent, cls, static_cast<int>(_countof(cls)));
        if (util::EqualsInsensitive(cls, L"ComboBox") || util::EqualsInsensitive(cls, L"ComboBoxEx32"))
        {
            return true;
        }
    }
    return false;
}

// text box keys take priority over menu shortcuts
bool KeepForTextInput(const MSG& msg)
{
    if ((msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) || !IsTextInput(GetFocus()))
    {
        return false;
    }
    if (msg.wParam == VK_DELETE || msg.wParam == VK_BACK || msg.wParam == VK_F2)
    {
        return true;
    }
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    return ctrl && !alt && (msg.wParam == 'C' || msg.wParam == 'V' || msg.wParam == 'X' || msg.wParam == 'Z' || msg.wParam == 'Y');
}

} // namespace

bool MainWindow::Impl::TranslateAccelerator(const MSG& msg)
{
    if (IsWindow(key_handles_window_) && IsDialogMessageW(key_handles_window_, const_cast<MSG*>(&msg)))
    {
        return true;
    }
    // main shortcuts apply to the window and its children, other windows keep their own
    if (GetAncestor(msg.hwnd, GA_ROOT) != hwnd_)
    {
        return false;
    }
    if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)
    {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        HWND focus = GetFocus();
        const bool focus_edit = IsTextInput(focus);
        if (!ctrl && !shift && !alt && msg.wParam == VK_F7 && (HIWORD(msg.lParam) & KF_REPEAT) != 0)
        {
            return true;
        }

        // leave Alt+D for the menu bar, German File label is "&Datei"
        if (ctrl && !alt && msg.wParam == 'L')
        {
            if (browse_.address())
            {
                SetFocus(browse_.address());
                SendMessageW(browse_.address(), EM_SETSEL, 0, -1);
                return true;
            }
        }

        if (ctrl && !alt && !shift && msg.wParam == 'A')
        {
            if (SelectAllInFocusedList())
            {
                return true;
            }
            if (focus_edit && focus)
            {
                SendMessageW(focus, EM_SETSEL, 0, -1);
                return true;
            }
        }

        if (ctrl && !alt && shift && msg.wParam == 'G')
        {
            if (OpenSelectedSearchResult(true))
            {
                return true;
            }
        }

        if (!ctrl && !alt)
        {
            if (msg.wParam == VK_TAB && !focus_edit)
            {
                HWND tree = browse_.tree().hwnd();
                HWND values = browse_.values().hwnd();
                const bool tree_ready = tree && IsWindowVisible(tree);
                const bool values_ready = values && IsWindowVisible(values);
                HWND next = (focus == values && tree_ready) ? tree
                            : values_ready                  ? values
                            : tree_ready                    ? tree
                                                            : nullptr;
                if (next)
                {
                    FocusPane(next);
                    return true;
                }
            }
            if (msg.wParam == VK_F6)
            {
                CyclePaneFocus(!shift);
                return true;
            }
            // Backspace edits active search text
            if (msg.wParam == VK_BACK && !focus_edit && !win32ui::ListTypeSearchActive(focus))
            {
                HandleMenuCommand(cmd::kNavBack);
                return true;
            }
        }
    }
    return win32ui::TranslateShortcuts(hwnd_, shortcuts_, const_cast<MSG*>(&msg), KeepForTextInput);
}

LRESULT CALLBACK MainWindow::Impl::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<MainWindow::Impl*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }

    auto* self = reinterpret_cast<MainWindow::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self)
    {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return self->HandleMessage(message, wparam, lparam);
}

LRESULT CALLBACK MainWindow::Impl::AddressEditProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref_data)
{
    auto* self = reinterpret_cast<MainWindow::Impl*>(ref_data);
    if (message == WM_CONTEXTMENU)
    {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (pt.x == -1 && pt.y == -1)
        {
            RECT rect = {};
            GetWindowRect(hwnd, &rect);
            pt.x = rect.left + 8;
            pt.y = rect.top + (rect.bottom - rect.top) / 2;
        }
        if (self)
        {
            self->ShowAddressContextMenu(hwnd, pt);
        }
        return 0;
    }
    if (message == WM_CHAR && wparam == 0x16 && (GetKeyState(VK_SHIFT) & 0x8000) != 0)
    {
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
        (GetKeyState(VK_SHIFT) & 0x8000) != 0)
    {
        if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & ES_READONLY) == 0 && IsClipboardFormatAvailable(CF_UNICODETEXT))
        {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            SendMessageW(hwnd, WM_PASTE, 0, 0);
            SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);
        }
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_RETURN)
    {
        if (self && self->hwnd_)
        {
            SendMessageW(self->hwnd_, frame::message_id::kAddressEnter, 0, 0);
        }
        return 0;
    }
    if (message == WM_CHAR && wparam == VK_RETURN)
    {
        return 0;
    }
    if (message == WM_SETFOCUS)
    {
        LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        SendMessageW(hwnd, EM_SCROLLCARET, 0, 0);
        return result;
    }
    if (message == WM_LBUTTONDOWN)
    {
        if (GetFocus() != hwnd)
        {
            LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            SendMessageW(hwnd, EM_SCROLLCARET, 0, 0);
            return result;
        }
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK MainWindow::Impl::FilterEditProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref_data)
{
    auto* self = reinterpret_cast<MainWindow::Impl*>(ref_data);
    if (message == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_DOWN))
    {
        if (self)
        {
            self->FocusFirstValue();
        }
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE)
    {
        if (self)
        {
            self->ClearValueFilter(GetWindowTextLengthW(hwnd) == 0);
        }
        return 0;
    }
    if (message == WM_CHAR && wparam == VK_ESCAPE)
    {
        return 0;
    }
    if (message == WM_CHAR && wparam == VK_RETURN)
    {
        return 0;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK MainWindow::Impl::ListViewProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref_data)
{
    auto* self = reinterpret_cast<MainWindow::Impl*>(ref_data);
    if (self && InSendMessage() &&
        (message == WM_CHAR || message == WM_KEYDOWN || message == WM_SETFOCUS || message == LVM_GETITEMCOUNT ||
         message == LVM_FINDITEMW || message == LVM_GETITEMTEXTW || message == LVM_GETITEMW))
    {
        self->FlushExternalNavigation();
        if (message == WM_SETFOCUS && hwnd != self->browse_.values().hwnd())
        {
            SetFocus(self->browse_.values().hwnd());
            return 0;
        }
        // other programs type value names into the first list they find
        if (message == WM_CHAR && hwnd != self->browse_.values().hwnd())
        {
            return SendMessageW(self->browse_.values().hwnd(), message, wparam, lparam);
        }
    }

    if (message == WM_KEYDOWN && wparam == VK_RETURN && self && hwnd == self->browse_.values().hwnd())
    {
        self->value_activate_from_key_ = true;
    }
    if (message == WM_SETFOCUS && self)
    {
        self->last_focus_ = hwnd;
    }
    if (message == WM_CTLCOLOREDIT)
    {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        SetTextColor(hdc, win32ui::CurrentThemeColors().text);
        SetBkColor(hdc, win32ui::CurrentThemeColors().panel);
        return reinterpret_cast<LRESULT>(win32ui::ThemeBrush(win32ui::ThemeFill::panel));
    }
    if (message == WM_PRINTCLIENT)
    {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        RECT rect = {};
        GetClientRect(hwnd, &rect);
        FillRect(hdc, &rect, win32ui::ThemeBrush(win32ui::ThemeFill::panel));
    }
    if (message == WM_THEMECHANGED)
    {
        if (self)
        {
            InvalidateRect(hwnd, nullptr, TRUE);
        }
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK MainWindow::Impl::TreeViewProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref_data)
{
    auto* self = reinterpret_cast<MainWindow::Impl*>(ref_data);
    if (message == TVM_SELECTITEM && wparam == TVGN_CARET && self && hwnd == self->regedit_compat_tree_.hwnd())
    {
        const HTREEITEM item = reinterpret_cast<HTREEITEM>(lparam);
        const bool unchanged = TreeView_GetSelection(hwnd) == item;
        const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
        if (unchanged)
        {
            RegistryNode* node = self->regedit_compat_tree_.NodeFromItem(item);
            if (node)
            {
                self->QueueCompatJump(*node);
            }
        }
        return result;
    }
    if (self && hwnd == self->browse_.tree().hwnd())
    {
        if ((message == WM_PAINT || message == WM_ERASEBKGND) && self->tree_painted_ && self->browse_.tree().IsRedrawSuspended())
        {
            ValidateRect(hwnd, nullptr);
            return message == WM_ERASEBKGND;
        }
        if (message == WM_PAINT || message == WM_SHOWWINDOW)
        {
            self->tree_painted_ = message == WM_PAINT;
        }
    }
    if (message == WM_SETFOCUS && self)
    {
        self->last_focus_ = hwnd;
    }
    if (message == WM_KEYDOWN && self && hwnd == self->browse_.tree().hwnd())
    {
        switch (wparam)
        {
        case VK_RIGHT:
            self->browse_.set_tree_type_select_descend(true);
            break;
        case VK_LEFT:
        case VK_UP:
        case VK_DOWN:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
            self->browse_.set_tree_type_select_descend(false);
            break;
        default:
            break;
        }
    }
    if (message == WM_CHAR && self && hwnd == self->browse_.tree().hwnd())
    {
        wchar_t ch = static_cast<wchar_t>(wparam);
        if (ch == L'\b' || (iswprint(ch) && ch != L'\r' && ch != L'\n' && ch != L'\t'))
        {
            self->HandleTypeToSelectTree(ch);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

} // namespace regkit
