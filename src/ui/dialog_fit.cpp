// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/dialog_fit.h"

#include "ui/dialog_layout.h"
#include "ui/dialog_metrics.h"
#include "win32/text_transform.h"
#include "win32/translation.h"
#include "win32/window_metrics.h"

#include <algorithm>
#include <commctrl.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace regkit::appearance
{

namespace
{

enum class Kind
{
    other,
    button,
    link,
    label,
};

struct Fit
{
    std::wstring text;
    HFONT font = nullptr;
    LONG_PTR style = 0;
    Kind kind = Kind::other;
    int width = 0;
    int line = 0;
};

std::unordered_map<HWND, Fit> fits;

int Measure(HWND control, Kind kind, LONG_PTR style, const std::wstring& text, HFONT font, int* line)
{
    SIZE ideal = {};
    if (kind == Kind::button)
    {
        const LONG_PTR type = style & BS_TYPEMASK;
        if (type == BS_GROUPBOX || type == BS_OWNERDRAW || !SendMessageW(control, BCM_GETIDEALSIZE, 0, reinterpret_cast<LPARAM>(&ideal)))
        {
            return 0;
        }
        const bool push = type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON;
        return ideal.cx + metrics::Scaled(push ? 13 : 12, win32::DpiForWindow(control));
    }
    if (kind == Kind::link)
    {
        SendMessageW(control, LM_GETIDEALSIZE, SHRT_MAX, reinterpret_cast<LPARAM>(&ideal));
        return ideal.cx;
    }
    const LONG_PTR type = style & SS_TYPEMASK;
    if (type != SS_LEFT && type != SS_CENTER && type != SS_RIGHT && type != SS_LEFTNOWORDWRAP && type != SS_SIMPLE)
    {
        return 0;
    }
    HDC hdc = GetDC(control);
    HGDIOBJ old_font = SelectObject(hdc, font);
    RECT needed = {};
    DrawTextW(hdc, text.c_str(), -1, &needed, DT_CALCRECT | DT_SINGLELINE | ((style & SS_NOPREFIX) ? DT_NOPREFIX : 0));
    SelectObject(hdc, old_font);
    ReleaseDC(control, hdc);
    *line = needed.bottom;
    return needed.right;
}

} // namespace

int TextFitWidth(HWND control)
{
    wchar_t class_name[16] = {};
    GetClassNameW(control, class_name, static_cast<int>(_countof(class_name)));
    const Kind kind = util::EqualsInsensitive(class_name, WC_BUTTONW) ? Kind::button
                      : util::EqualsInsensitive(class_name, WC_LINK)  ? Kind::link
                      : util::EqualsInsensitive(class_name, WC_STATICW) ? Kind::label
                                                                       : Kind::other;
    if (kind == Kind::other)
    {
        return 0;
    }
    std::wstring text = util::WindowText(control);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE) & 0xFFFF;
    auto cached = fits.find(control);
    if (cached == fits.end() || cached->second.kind != kind || cached->second.font != font || cached->second.style != style || cached->second.text != text)
    {
        if (fits.size() >= 512)
        {
            fits.clear();
        }
        Fit fit = {std::move(text), font, style, kind};
        fit.width = Measure(control, kind, style, fit.text, font, &fit.line);
        cached = fits.insert_or_assign(control, std::move(fit)).first;
    }
    RECT rect = {};
    GetClientRect(control, &rect);
    return cached->second.line && rect.bottom >= 2 * cached->second.line ? 0 : cached->second.width;
}

int TextFitWidth(std::initializer_list<HWND> controls)
{
    int fit = 0;
    for (HWND control : controls)
    {
        fit = std::max(fit, TextFitWidth(control));
    }
    return fit;
}

int PlaceButtonRow(std::initializer_list<HWND> buttons, int right, int y, int min_width, int height, int gap)
{
    int x = right + gap;
    for (auto button = std::rbegin(buttons); button != std::rend(buttons); ++button)
    {
        const int width = std::max(min_width, TextFitWidth(*button));
        x -= gap + width;
        Place(*button, x, y, width, height);
    }
    return right - x;
}

void LocalizeDialog(HWND dialog)
{
    util::TranslateDialog(dialog);
    struct Control
    {
        HWND hwnd = nullptr;
        RECT rect = {};
        RECT initial = {};
        int fit = 0;
        bool field = false;
        bool push = false;
        bool group = false;
        bool option = false;
        bool group_start = false;
        bool edge = false;
        Control* container = nullptr;
        int depth = 0;
        int short_by = 0;
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
        const LONG_PTR type = style & BS_TYPEMASK;
        Control control;
        control.hwnd = child;
        GetWindowRect(child, &control.rect);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&control.rect), 2);
        control.initial = control.rect;
        control.fit = TextFitWidth(child);
        control.field = !button && (!util::EqualsInsensitive(class_name, WC_STATICW) || GetWindowTextLengthW(child) == 0);
        control.push = button && (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON);
        control.group = button && type == BS_GROUPBOX;
        control.option = button && (type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON || type == BS_AUTOCHECKBOX || type == BS_CHECKBOX);
        control.group_start = (style & WS_GROUP) != 0;
        controls.push_back(control);
    }
    RECT client = {};
    GetClientRect(dialog, &client);
    const UINT dpi = win32::DpiForWindow(dialog);
    const int gap = metrics::Scaled(4, dpi);
    const int option_gap = metrics::Scaled(metrics::kOptionGap, dpi);
    auto width = [](const RECT& rect) { return static_cast<int>(rect.right - rect.left); };
    auto contains = [](const RECT& box, const RECT& rect) { return box.left <= rect.left && box.right >= rect.right && box.top <= rect.top && box.bottom >= rect.bottom; };
    LONG content_left = client.right;
    LONG content_right = client.left;
    for (Control& control : controls)
    {
        control.edge = control.rect.right >= client.right;
        if (!control.edge)
        {
            content_left = std::min(content_left, control.rect.left);
            content_right = std::max(content_right, control.rect.right);
        }
        for (Control& group : controls)
        {
            if (group.group && &group != &control && contains(group.initial, control.initial) && (!control.container || width(group.initial) < width(control.container->initial)))
            {
                control.container = &group;
            }
        }
    }
    const LONG designed_right = content_right;
    std::vector<Control*> order;
    for (Control& control : controls)
    {
        for (const Control* parent = control.container; parent; parent = parent->container)
        {
            ++control.depth;
        }
        order.push_back(&control);
    }
    std::stable_sort(order.begin(), order.end(), [](const Control* a, const Control* b) { return a->depth > b->depth; });
    for (size_t i = 1; i < controls.size(); ++i)
    {
        Control& prior = controls[i - 1];
        Control& option = controls[i];
        if (prior.option && option.option && !option.group_start && option.rect.top == prior.rect.top && option.rect.left > prior.rect.left)
        {
            prior.rect.right = prior.rect.left + prior.fit;
            const LONG left = prior.rect.right + option_gap;
            option.rect.right = std::max(option.rect.right, left + option.fit);
            option.rect.left = left;
        }
    }
    auto inside = [](const Control& control, const Control& group) {
        for (const Control* parent = control.container; parent; parent = parent->container)
        {
            if (parent == &group)
            {
                return true;
            }
        }
        return false;
    };
    auto shift = [&](Control& control, LONG dx) {
        control.rect.left += dx;
        control.rect.right += dx;
        for (Control& child : controls)
        {
            if (control.group && inside(child, control))
            {
                child.rect.left += dx;
                child.rect.right += dx;
            }
        }
    };
    auto needed = [&](const Control& control) {
        if (!control.group)
        {
            return control.fit;
        }
        LONG right = control.rect.left;
        int short_by = 0;
        for (const Control& child : controls)
        {
            if (child.container == &control)
            {
                right = std::max(right, child.rect.left + std::max(width(child.rect), child.fit) + std::clamp(control.initial.right - child.initial.right, 0L, static_cast<LONG>(gap)));
                short_by = std::max(short_by, child.short_by);
            }
        }
        return static_cast<int>(right + short_by - control.rect.left);
    };
    auto left_limit = [&](const Control& control) { return control.container ? control.container->rect.left + gap : content_left; };
    auto right_limit = [&](const Control& control) { return control.container ? control.container->rect.right - gap : content_right; };
    auto shrinkable = [&](const Control& field) { return std::max(0, width(field.rect) - width(field.initial) / 2); };
    auto same_row = [](const Control& a, const Control& b) { return &a != &b && a.container == b.container && !b.edge && a.rect.top < b.rect.bottom && a.rect.bottom > b.rect.top; };
    auto widen = [&](Control& control, int missing) {
        std::vector<Control*> row;
        for (Control& other : controls)
        {
            if (same_row(control, other))
            {
                row.push_back(&other);
            }
        }
        std::vector<Control*> right_side;
        for (Control* other : row)
        {
            if (other->rect.left >= control.rect.right)
            {
                right_side.push_back(other);
            }
        }
        std::sort(right_side.begin(), right_side.end(), [](const Control* a, const Control* b) { return a->rect.left < b->rect.left; });
        const auto first_field = std::find_if(right_side.begin(), right_side.end(), [&](const Control* other) {
            return other->field && std::count_if(controls.begin(), controls.end(), [&](const Control& field) { return field.field && field.rect.left == other->rect.left; }) > 1;
        });
        Control* next = first_field != right_side.end() ? *first_field : right_side.empty() ? nullptr
                                                                                            : right_side.front();
        const LONG end = first_field != right_side.end() ? (*first_field)->rect.left - gap : right_limit(control);
        right_side.erase(first_field, right_side.end());
        const size_t count = right_side.size();
        std::vector<int> chained(count, -1);
        std::vector<LONG> spacing(count);
        std::vector<LONG> slack(count);
        std::vector<int> reach(count);
        auto previous_right = [&](size_t i) { return chained[i] < 0 ? control.rect.right : right_side[static_cast<size_t>(chained[i])]->rect.right; };
        for (size_t i = 0; i < count; ++i)
        {
            const Control* other = right_side[i];
            for (size_t j = 0; j < i; ++j)
            {
                chained[i] = other->rect.top < right_side[j]->rect.bottom && other->rect.bottom > right_side[j]->rect.top ? static_cast<int>(j) : chained[i];
            }
            const LONG previous = previous_right(i);
            const int fit = other->push ? 0 : needed(*other);
            spacing[i] = std::min(static_cast<LONG>(gap), other->rect.left - previous);
            slack[i] = fit > 0 ? std::max(0, width(other->rect) - fit) : 0;
            reach[i] = (chained[i] < 0 ? 0 : reach[static_cast<size_t>(chained[i])]) + other->rect.left - previous - spacing[i] + slack[i];
        }
        int available = count ? INT_MAX : std::max(0, static_cast<int>(end - control.rect.right));
        for (size_t i = 0; i < count; ++i)
        {
            if (std::find(chained.begin() + static_cast<std::ptrdiff_t>(i) + 1, chained.end(), static_cast<int>(i)) == chained.end())
            {
                available = std::min(available, reach[i] + std::max(0, static_cast<int>(end - right_side[i]->rect.right)));
            }
        }
        const int grow = std::clamp(available, 0, missing);
        control.rect.right += grow;
        missing -= grow;
        for (size_t i = 0; i < count; ++i)
        {
            const LONG move = std::max(0L, previous_right(i) + spacing[i] - right_side[i]->rect.left);
            shift(*right_side[i], move);
            right_side[i]->rect.right -= std::min(move, slack[i]);
        }
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
            LONG limit = left_limit(control);
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
            missing -= take;
        }
        return missing;
    };
    MONITORINFO monitor = {sizeof(monitor)};
    RECT window = {};
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    GetWindowRect(dialog, &window);
    int room = std::max(0, width(monitor.rcWork) - width(window));
    for (int pass = 0; pass < 4; ++pass)
    {
        for (Control* control : order)
        {
            const int missing = needed(*control) - width(control->rect);
            control->short_by = missing > 0 && !control->edge ? widen(*control, missing) : 0;
        }
        int grow = 0;
        for (const Control& control : controls)
        {
            if (!control.container && !control.edge)
            {
                grow = std::max({grow, control.short_by, static_cast<int>(control.rect.left + std::max(width(control.rect), needed(control)) - content_right)});
            }
        }
        grow = std::min(grow, room);
        if (grow <= 0)
        {
            break;
        }
        room -= grow;
        std::vector<Control*> moving;
        std::vector<Control*> stretching;
        auto anchored = [&](const Control& control) { return std::find(moving.begin(), moving.end(), &control) != moving.end() || std::find(stretching.begin(), stretching.end(), &control) != stretching.end(); };
        auto stretches = [&](const Control& control) { return control.field || control.group || control.short_by > 0 || control.rect.left <= content_left + gap; };
        for (Control& control : controls)
        {
            if (!control.container && !control.edge && control.rect.right >= content_right - gap)
            {
                (stretches(control) ? stretching : moving).push_back(&control);
            }
        }
        for (size_t i = 0; i < moving.size(); ++i)
        {
            for (Control& other : controls)
            {
                if (same_row(*moving[i], other) && other.rect.right <= moving[i]->rect.left && other.rect.right >= moving[i]->rect.left - 3 * gap && !anchored(other))
                {
                    (stretches(other) ? stretching : moving).push_back(&other);
                }
            }
        }
        for (Control& control : controls)
        {
            if (control.edge)
            {
                moving.push_back(&control);
            }
        }
        for (Control* control : stretching)
        {
            control->rect.right += grow;
        }
        for (Control* control : moving)
        {
            shift(*control, grow);
        }
        content_right += grow;
    }
    GrowDialogWidth(dialog, client.right + content_right - designed_right);
    for (Control& control : controls)
    {
        const LONG margin = control.container ? control.container->initial.right - control.initial.right : 0;
        if (control.field && control.container && margin >= 0 && margin <= 3 * gap)
        {
            control.rect.right = std::max(control.rect.right, control.container->rect.right - margin);
        }
        if (!EqualRect(&control.rect, &control.initial))
        {
            SetWindowPos(control.hwnd, nullptr, control.rect.left, control.rect.top, width(control.rect), control.rect.bottom - control.rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

bool GrowDialogWidth(HWND dialog, int client_width)
{
    RECT client = {};
    RECT window = {};
    if (!GetClientRect(dialog, &client) || client.right >= client_width || !GetWindowRect(dialog, &window))
    {
        return false;
    }
    SetWindowPos(dialog, nullptr, 0, 0, window.right - window.left + client_width - client.right, window.bottom - window.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
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

} // namespace regkit::appearance
