// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/autocomplete.h"

#include "ui/gdi_cache.h"
#include "ui/theme.h"
#include "win32/text_transform.h"
#include "win32/window_metrics.h"

#include <commctrl.h>
#include <shldisp.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <algorithm>
#include <memory>
#include <new>
#include <unordered_set>

namespace regkit::appearance
{
namespace
{

constexpr UINT_PTR kPopupSubclassId = 1;
constexpr UINT_PTR kSizeBoxSubclassId = 1;
constexpr UINT_PTR kEditSubclassId = 1;

bool autocomplete_enabled = true;
bool popup_sizing = false;

struct SuggestCall
{
    AutoCompleteSuggest suggest;
    std::wstring query;
    std::vector<std::wstring> result;
};

UINT SuggestMessage()
{
    static const UINT message = RegisterWindowMessageW(L"RegKitAutoCompleteSuggest");
    return message;
}

LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR)
{
    if (msg == SuggestMessage() && lparam)
    {
        const std::unique_ptr<std::shared_ptr<SuggestCall>> call(reinterpret_cast<std::shared_ptr<SuggestCall>*>(lparam));
        (*call)->result = (*call)->suggest((*call)->query);
        return TRUE;
    }
    if (msg == WM_NCDESTROY)
    {
        RemoveWindowSubclass(hwnd, EditSubclassProc, id);
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

class AutoCompleteSource : public ::IEnumString, public ::IACList
{
  public:
    AutoCompleteSource(AutoCompleteSuggest suggest, HWND edit)
        : suggest_(std::move(suggest)), edit_(edit)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out)
        {
            return E_POINTER;
        }
        *out = nullptr;
        if (riid == IID_IUnknown || riid == IID_IEnumString)
        {
            *out = static_cast<::IEnumString*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IACList)
        {
            *out = static_cast<::IACList*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG celt, LPOLESTR* rgelt, ULONG* pceltFetched) override
    {
        if (!rgelt)
        {
            return E_POINTER;
        }
        if (celt > 1 && !pceltFetched)
        {
            return E_POINTER;
        }
        UpdateSuggestionsIfNeeded();
        ULONG fetched = 0;
        for (; fetched < celt && index_ < suggestions_.size(); ++fetched, ++index_)
        {
            const std::wstring& item = suggestions_[index_];
            size_t bytes = (item.size() + 1) * sizeof(wchar_t);
            // return strings with COM task memory so the caller can free them
            wchar_t* buffer = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
            if (!buffer)
            {
                for (ULONG i = 0; i < fetched; ++i)
                {
                    CoTaskMemFree(rgelt[i]);
                }
                if (pceltFetched)
                {
                    *pceltFetched = 0;
                }
                return E_OUTOFMEMORY;
            }
            wcscpy_s(buffer, item.size() + 1, item.c_str());
            rgelt[fetched] = buffer;
        }
        if (pceltFetched)
        {
            *pceltFetched = fetched;
        }
        return fetched == celt ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override
    {
        UpdateSuggestionsIfNeeded();
        if (celt > suggestions_.size() - std::min(index_, suggestions_.size()))
        {
            index_ = suggestions_.size();
            return S_FALSE;
        }
        index_ += celt;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Reset() override
    {
        UpdateSuggestionsIfNeeded();
        index_ = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumString** out) override
    {
        if (!out)
        {
            return E_POINTER;
        }
        *out = nullptr;
        AutoCompleteSource* clone = nullptr;
        try
        {
            clone = new AutoCompleteSource(suggest_, edit_);
            clone->suggestions_ = suggestions_;
            clone->index_ = index_;
            clone->last_text_ = last_text_;
            clone->query_override_ = query_override_;
        }
        catch (const std::bad_alloc&)
        {
            if (clone)
            {
                clone->Release();
            }
            return E_OUTOFMEMORY;
        }
        *out = clone;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Expand(PCWSTR text) noexcept override
    {
        if (!text)
        {
            query_override_.clear();
            return S_OK;
        }
        query_override_ = text;
        suggestions_.clear();
        index_ = 0;
        last_text_.clear();
        return S_OK;
    }

  private:
    std::wstring ReadEditText() const
    {
        return util::WindowText(edit_);
    }

    void UpdateSuggestionsIfNeeded()
    {
        if (!suggest_ || !autocomplete_enabled)
        {
            suggestions_.clear();
            index_ = 0;
            last_text_.clear();
            return;
        }
        std::wstring query = query_override_.empty() ? ReadEditText() : query_override_;
        if (query_override_.empty() && edit_ && !query.empty())
        {
            DWORD sel_start = 0;
            DWORD sel_end = 0;
            SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start), reinterpret_cast<LPARAM>(&sel_end));
            if (sel_end > sel_start && sel_end == query.size())
            {
                // ignore auto appended selection when rebuilding suggestions
                query = query.substr(0, sel_start);
            }
        }
        if (query == last_text_)
        {
            return;
        }
        last_text_ = query;
        index_ = 0;
        suggestions_ = SuggestOnEditThread(query);
    }

    std::vector<std::wstring> SuggestOnEditThread(const std::wstring& query) const
    {
        auto call = std::make_shared<SuggestCall>(SuggestCall{suggest_, query, {}});
        auto* message_ref = new std::shared_ptr<SuggestCall>(call);
        DWORD_PTR handled = 0;
        if (!SendMessageTimeoutW(edit_, SuggestMessage(), 0, reinterpret_cast<LPARAM>(message_ref), SMTO_BLOCK, 2000, &handled))
        {
            if (GetLastError() != ERROR_TIMEOUT)
            {
                delete message_ref;
            }
            return {};
        }
        return handled ? std::move(call->result) : std::vector<std::wstring>();
    }

    ~AutoCompleteSource() = default;

    LONG ref_count_ = 1;
    AutoCompleteSuggest suggest_;
    HWND edit_ = nullptr;
    std::vector<std::wstring> suggestions_;
    size_t index_ = 0;
    std::wstring last_text_;
    std::wstring query_override_;
};

AutoCompleteSuggest& KeySuggest()
{
    static AutoCompleteSuggest suggest;
    return suggest;
}

bool ClassIs(HWND hwnd, const wchar_t* name)
{
    wchar_t buffer[64] = {};
    return GetClassNameW(hwnd, buffer, static_cast<int>(_countof(buffer))) > 0 && _wcsicmp(buffer, name) == 0;
}

BOOL CALLBACK ThemePopupProc(HWND hwnd, LPARAM);

void AlignPopup(HWND hwnd, WINDOWPOS* pos)
{
    HWND anchor = GetFocus();
    if (!anchor || popup_sizing)
    {
        return;
    }
    if (ClassIs(GetParent(anchor), WC_COMBOBOXW))
    {
        anchor = GetParent(anchor);
    }
    RECT box = {};
    RECT current = {};
    GetWindowRect(anchor, &box);
    GetWindowRect(hwnd, &current);
    if ((pos->flags & SWP_NOSIZE) == 0)
    {
        pos->cx = box.right - box.left;
    }
    if ((pos->flags & SWP_NOMOVE) == 0)
    {
        const int height = (pos->flags & SWP_NOSIZE) ? current.bottom - current.top : pos->cy;
        pos->x = box.left;
        pos->y = pos->y < box.top ? box.top - height : box.bottom;
    }
}

LRESULT CALLBACK PopupSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR)
{
    switch (msg)
    {
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, PopupSubclassProc, id);
        break;
    case WM_WINDOWPOSCHANGED:
        if (reinterpret_cast<WINDOWPOS*>(lparam)->flags & SWP_SHOWWINDOW)
        {
            ThemePopupProc(hwnd, 0);
        }
        break;
    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
        popup_sizing = msg == WM_ENTERSIZEMOVE;
        break;
    case WM_WINDOWPOSCHANGING:
        AlignPopup(hwnd, reinterpret_cast<WINDOWPOS*>(lparam));
        break;
    case WM_DRAWITEM:
        {
            // the popup owner draws its suggestion list with system colors
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (draw->CtlType != ODT_LISTVIEW)
            {
                break;
            }
            const Theme& theme = Theme::Current();
            const bool selected = (draw->itemState & ODS_SELECTED) != 0;
            FillRect(draw->hDC, &draw->rcItem, CachedBrush(selected ? theme.SelectionColor() : theme.SurfaceColor()));
            wchar_t text[1024] = {};
            ListView_GetItemText(draw->hwndItem, draw->itemID, 0, text, static_cast<int>(_countof(text)));
            RECT rect = draw->rcItem;
            rect.left += MulDiv(4, static_cast<int>(win32::DpiForWindow(hwnd)), 96);
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, selected ? theme.SelectionTextColor() : theme.TextColor());
            DrawTextW(draw->hDC, text, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            return TRUE;
        }
    case WM_ERASEBKGND:
        {
            RECT rect = {};
            GetClientRect(hwnd, &rect);
            FillRect(reinterpret_cast<HDC>(wparam), &rect, Theme::Current().SurfaceBrush());
            return TRUE;
        }
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK SizeBoxSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR)
{
    switch (msg)
    {
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, SizeBoxSubclassProc, id);
        break;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
        {
            PAINTSTRUCT ps = {};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rect = {};
            GetClientRect(hwnd, &rect);
            FillRect(hdc, &rect, Theme::Current().SurfaceBrush());
            if (HTHEME theme = OpenThemeData(hwnd, VSCLASS_STATUS))
            {
                SIZE grip = {};
                GetThemePartSize(theme, hdc, SP_GRIPPER, 0, &rect, TS_DRAW, &grip);
                rect.left = rect.right - grip.cx;
                rect.top = rect.bottom - grip.cy;
                DrawThemeBackground(theme, hdc, SP_GRIPPER, 0, &rect, nullptr);
                CloseThemeData(theme);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

BOOL CALLBACK ThemePopupProc(HWND hwnd, LPARAM)
{
    if (!ClassIs(hwnd, L"Auto-Suggest Dropdown"))
    {
        return TRUE;
    }
    Theme::Current().ApplyToWindow(hwnd);
    EnsureSubclass(hwnd, PopupSubclassProc, kPopupSubclassId);
    EnumChildWindows(
        hwnd,
        [](HWND child, LPARAM) -> BOOL {
            SetDarkWindowTheme(child, Theme::UseDarkMode());
            if (ClassIs(child, WC_LISTVIEWW))
            {
                ListView_SetBkColor(child, Theme::Current().SurfaceColor());
            }
            else if (ClassIs(child, WC_SCROLLBARW) && (GetWindowLongPtrW(child, GWL_STYLE) & (SBS_SIZEBOX | SBS_SIZEGRIP)))
            {
                EnsureSubclass(child, SizeBoxSubclassProc, kSizeBoxSubclassId);
            }
            return TRUE;
        },
        0
    );
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    return TRUE;
}

LRESULT CALLBACK CreateHook(int code, WPARAM wparam, LPARAM lparam)
{
    // subclass the popup before its first paint
    if (code == HCBT_CREATEWND && ClassIs(reinterpret_cast<HWND>(wparam), L"Auto-Suggest Dropdown"))
    {
        EnsureSubclass(reinterpret_cast<HWND>(wparam), PopupSubclassProc, kPopupSubclassId);
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

} // namespace

bool AttachAutoComplete(HWND edit, AutoCompleteSuggest suggest)
{
    COMBOBOXINFO combo = {sizeof(combo)};
    if (GetComboBoxInfo(edit, &combo) && combo.hwndItem)
    {
        edit = combo.hwndItem;
    }
    ::IAutoComplete2* autocomplete = nullptr;
    if (!edit || FAILED(CoCreateInstance(CLSID_AutoComplete, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&autocomplete))) || !autocomplete)
    {
        return false;
    }
    // Init creates the popup
    static const HHOOK create_hook = SetWindowsHookExW(WH_CBT, CreateHook, nullptr, GetCurrentThreadId());
    (void)create_hook;
    EnsureSubclass(edit, EditSubclassProc, kEditSubclassId);
    ::IEnumString* source = new AutoCompleteSource(std::move(suggest), edit);
    const HRESULT hr = autocomplete->Init(edit, source, nullptr, nullptr);
    source->Release();
    if (SUCCEEDED(hr))
    {
        autocomplete->SetOptions(ACO_AUTOSUGGEST | ACO_AUTOAPPEND | ACO_UPDOWNKEYDROPSLIST | ACO_USETAB);
    }
    // attached edit keeps autocomplete object alive
    autocomplete->Release();
    return SUCCEEDED(hr);
}

void SetAutoCompleteEnabled(bool enabled)
{
    autocomplete_enabled = enabled;
}

void SetKeySuggest(AutoCompleteSuggest suggest)
{
    KeySuggest() = std::move(suggest);
}

std::vector<std::wstring> SuggestKeys(const std::wstring& text)
{
    return KeySuggest() ? KeySuggest()(text) : std::vector<std::wstring>();
}

std::vector<std::wstring> SuggestComboPaths(HWND combo, const std::wstring& text)
{
    const size_t sep = text.find_last_of(L'\\');
    const std::wstring parent = sep == std::wstring::npos ? std::wstring() : text.substr(0, sep + 1);
    std::vector<std::wstring> items;
    std::unordered_set<std::wstring> seen;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int index = 0; index < count; ++index)
    {
        std::wstring path(static_cast<size_t>(std::max<LRESULT>(0, SendMessageW(combo, CB_GETLBTEXTLEN, index, 0))), L'\0');
        SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(path.data()));
        if (path.size() > parent.size() && util::StartsWithInsensitive(path, parent))
        {
            path.resize(std::min(path.size(), path.find(L'\\', parent.size())));
            if (seen.insert(util::ToLower(path)).second)
            {
                items.push_back(std::move(path));
            }
        }
    }
    return items;
}

void ApplyAutoCompleteTheme()
{
    EnumThreadWindows(GetCurrentThreadId(), ThemePopupProc, 0);
}

} // namespace regkit::appearance
