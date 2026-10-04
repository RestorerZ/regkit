// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"

#include "appearance/dialog_layout.h"
#include "appearance/list_header.h"
#include "win32/translation.h"

namespace regkit
{
using namespace window_detail;

namespace
{

bool ValueNameExists(const RegistryNode& node, const std::wstring& name)
{
    RegistryValue entry;
    return RegistryStore::QueryValue(node, name, &entry);
}

bool KeyNameExists(const RegistryNode& parent, const std::wstring& name)
{
    KeyInfo info = {};
    return RegistryStore::QueryKeyInfo(ChildNode(parent, name), &info);
}

void ReportNameTaken(HWND owner, const wchar_t* message, const wchar_t* title, const std::wstring& name)
{
    ui::PromptKeyChoice(owner, message, registry_path::DisplayName(name), title, L"", L"", util::Tr(L"OK"));
}

void FormatCellFileTime(const FILETIME& filetime, wchar_t* buffer, int capacity)
{
    lstrcpynW(buffer, FormatFileTime(filetime).c_str(), capacity);
}

} // namespace

LRESULT MainWindow::Impl::HandleNotification(LPARAM lparam)
{
    auto* header = reinterpret_cast<NMHDR*>(lparam);
    if (!header)
    {
        return 0;
    }
    if (header->code == TTN_GETDISPINFOW || header->code == TTN_NEEDTEXTW || header->code == TTN_SHOW)
    {
        return HandleTooltipNotification(header, lparam);
    }
    LRESULT list_result = 0;
    if (appearance::HandleListViewNotify(hwnd_, header, &list_result))
    {
        return list_result;
    }
    if (header->hwndFrom == toolbar_.hwnd())
    {
        return HandleToolbarNotification(header, lparam);
    }
    if (header->hwndFrom == tab_)
    {
        return HandleTabNotification(header, lparam);
    }
    if (header->hwndFrom == browse_.tree().hwnd() || header->hwndFrom == regedit_compat_tree_.hwnd())
    {
        return HandleTreeNotification(header, lparam);
    }
    if (header->hwndFrom == browse_.values().hwnd())
    {
        return HandleValueNotification(header, lparam);
    }
    if (header->hwndFrom == history_list_)
    {
        return HandleHistoryNotification(header, lparam);
    }
    if (header->hwndFrom == search_results_list_)
    {
        return HandleSearchNotification(header, lparam);
    }
    if (header->hwndFrom == ListView_GetHeader(browse_.values().hwnd()) ||
        header->hwndFrom == ListView_GetHeader(history_list_) ||
        header->hwndFrom == ListView_GetHeader(search_results_list_))
    {
        return HandleHeaderNotification(header, lparam);
    }
    return 0;
}

std::wstring MainWindow::Impl::SearchCellFieldText(const search::Result& result, int subitem) const
{
    switch (subitem)
    {
    case 0:
        return result.key_path;
    case 1:
        return std::wstring(search::DisplayName(result));
    case 2:
        return search::TypeText(result);
    case 3:
        return result.data_text;
    case 4:
        return search::IsKeyRow(result) || result.kind == search::ResultKind::kTraceValue
                   ? std::wstring()
                   : std::to_wstring(result.data_size);
    default:
        return std::wstring();
    }
}

std::wstring MainWindow::Impl::ListCellFieldText(HWND list, int item, int display_subitem)
{
    if (list == browse_.values().hwnd())
    {
        const ListRow* row = browse_.values().RowAt(item);
        return row ? ValueRowFieldText(*row, MappedSubItem(value_column_subitems_, display_subitem)) : std::wstring();
    }
    if (list == search_results_list_)
    {
        const int subitem = MappedSubItem(search_column_subitems_, display_subitem);
        const int tab_index = SearchIndexFromTab(TabCtrl_GetCurSel(tab_));
        const SearchTab* tab = tab_index >= 0 && static_cast<size_t>(tab_index) < search_tabs_.size()
                                   ? &search_tabs_[static_cast<size_t>(tab_index)]
                                   : nullptr;
        if (tab && tab->is_compare)
        {
            if (item < 0 || static_cast<size_t>(item) >= tab->compare_rows.size())
            {
                return std::wstring();
            }
            const search::compare::Row& row = tab->compare_rows[static_cast<size_t>(item)];
            switch (subitem)
            {
            case 0:
                return row.key_path;
            case 1:
                return row.is_key ? std::wstring(util::Tr(L"(Key)"))
                                  : (row.value_name.empty() ? std::wstring(util::Tr(L"(Default)")) : row.value_name);
            case 2:
                return row.first_text;
            case 3:
                return row.second_text;
            case 4:
                return row.matches ? std::wstring(util::Tr(L"Same")) : std::wstring(util::Tr(L"Different"));
            default:
                return std::wstring();
            }
        }
        const search::Result* result = SearchResultAt(item);
        return result ? SearchCellFieldText(*result, subitem) : std::wstring();
    }
    if (list == history_list_)
    {
        const auto& entries = change_history_.entries();
        if (item < 0 || static_cast<size_t>(item) >= entries.size())
        {
            return std::wstring();
        }
        const HistoryEntry& entry = entries[static_cast<size_t>(item)];
        switch (display_subitem)
        {
        case 0:
            return entry.time_text;
        case 1:
            return entry.action;
        case 2:
            return entry.old_data;
        case 3:
            return entry.new_data;
        default:
            return std::wstring();
        }
    }
    return std::wstring();
}

bool MainWindow::Impl::ListCellTooltipText(std::wstring* out)
{
    POINT pt = {};
    if (!out || !GetCursorPos(&pt))
    {
        return false;
    }
    HWND list = WindowFromPoint(pt);
    if (list != browse_.values().hwnd() && list != search_results_list_ && list != history_list_)
    {
        return false;
    }
    ScreenToClient(list, &pt);
    LVHITTESTINFO hit = {};
    hit.pt = pt;
    const int item = ListView_SubItemHitTest(list, &hit);
    if (item < 0)
    {
        return false;
    }
    const std::wstring text = ListCellFieldText(list, item, hit.iSubItem);
    RECT cell = {};
    const bool measured = hit.iSubItem == 0
                              ? ListView_GetItemRect(list, item, &cell, LVIR_LABEL) != FALSE
                              : ListView_GetSubItemRect(list, item, hit.iSubItem, LVIR_BOUNDS, &cell) != FALSE;
    const int padding = hit.iSubItem == 0 ? kCellTooltipPadding : kCellTextInset;
    const int available = static_cast<int>(cell.right - cell.left) - padding;
    if (!measured || text.empty() || available <= 0 || !CellTextIsClipped(list, text, available))
    {
        return false;
    }
    // limit tooltip text so large registry data doesnt create huge windows
    size_t limit = std::min(text.size(), kValueTooltipTextLimit);
    size_t lines = 0;
    for (size_t i = 0; i < limit; ++i)
    {
        if (text[i] == L'\n' && ++lines == kValueTooltipLineLimit)
        {
            limit = i;
            break;
        }
    }
    out->assign(text, 0, limit);
    if (limit < text.size())
    {
        out->append(L"...");
    }
    return true;
}

LRESULT MainWindow::Impl::HandleTooltipNotification(NMHDR* header, LPARAM lparam)
{
    if (header->code == TTN_SHOW && header->hwndFrom == value_tooltip_)
    {
        RECT tip = {};
        POINT pt = {};
        MONITORINFO monitor = {};
        monitor.cbSize = sizeof(monitor);
        if (!GetWindowRect(value_tooltip_, &tip) || !GetCursorPos(&pt) ||
            !GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            return 0;
        }
        const int width = tip.right - tip.left;
        const int height = tip.bottom - tip.top;
        int x = pt.x + kValueTooltipCursorGap;
        int y = pt.y + kValueTooltipCursorGap;
        if (x + width > monitor.rcWork.right)
        {
            x = std::max<LONG>(monitor.rcWork.left, pt.x - kValueTooltipCursorGap - width);
        }
        if (y + height > monitor.rcWork.bottom)
        {
            y = std::max<LONG>(monitor.rcWork.top, pt.y - kValueTooltipCursorGap - height);
        }
        SetWindowPos(value_tooltip_, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        return TRUE;
    }
    if (header->code == TTN_GETDISPINFOW || header->code == TTN_NEEDTEXTW)
    {
        auto* info = reinterpret_cast<LPTOOLTIPTEXTW>(lparam);
        if (info && header->hwndFrom == value_tooltip_)
        {
            value_tooltip_text_.clear();
            if (ListCellTooltipText(&value_tooltip_text_))
            {
                info->lpszText = value_tooltip_text_.data();
            }
            else
            {
                info->lpszText = const_cast<wchar_t*>(L"");
            }
            return 0;
        }
        if (info)
        {
            int command_id = static_cast<int>(info->hdr.idFrom);
            std::wstring tip = CommandTooltipText(command_id);
            if (!tip.empty())
            {
                std::wstring shortcut = CommandShortcutText(command_id);
                if (!shortcut.empty())
                {
                    tip.append(L" (");
                    tip.append(shortcut);
                    tip.append(L")");
                }
                static std::wstring tip_storage;
                tip_storage = tip;
                info->lpszText = const_cast<wchar_t*>(tip_storage.c_str());
                return 0;
            }
        }
    }
    return 0;
}

LRESULT MainWindow::Impl::HandleToolbarNotification(NMHDR* header, LPARAM lparam)
{
    if (header->hwndFrom == toolbar_.hwnd() && header->code == NM_CUSTOMDRAW)
    {
        auto* draw = reinterpret_cast<NMTBCUSTOMDRAW*>(lparam);
        if (!draw)
        {
            return CDRF_DODEFAULT;
        }
        const Theme& theme = Theme::Current();
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
        {
            FillRect(draw->nmcd.hdc, &draw->nmcd.rc, theme.BackgroundBrush());
        }
        if (!Theme::UseDarkMode())
        {
            return CDRF_DODEFAULT;
        }
        HWND bar = header->hwndFrom;
        switch (draw->nmcd.dwDrawStage)
        {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            {
                bool is_separator = false;
                int command_id = static_cast<int>(draw->nmcd.dwItemSpec);
                int index = static_cast<int>(SendMessageW(bar, TB_COMMANDTOINDEX, command_id, 0));
                if (index >= 0)
                {
                    TBBUTTON button = {};
                    if (SendMessageW(bar, TB_GETBUTTON, index, reinterpret_cast<LPARAM>(&button)))
                    {
                        is_separator = (button.fsStyle & BTNS_SEP) != 0;
                    }
                }
                if (is_separator)
                {
                    return CDRF_DODEFAULT;
                }

                POINT cursor = {};
                GetCursorPos(&cursor);
                ScreenToClient(bar, &cursor);
                bool is_hovered = ((draw->nmcd.uItemState & CDIS_HOT) == CDIS_HOT) || PtInRect(&draw->nmcd.rc, cursor);

                draw->hbrMonoDither = theme.BackgroundBrush();
                draw->hbrLines = theme.BackgroundBrush();
                draw->hpenLines = appearance::CachedPen(theme.BorderColor(), 1);
                draw->clrText = theme.TextColor();
                draw->clrTextHighlight = theme.TextColor();
                draw->clrBtnFace = theme.BackgroundColor();
                draw->clrBtnHighlight = theme.SurfaceColor();
                draw->clrHighlightHotTrack = theme.HoverColor();
                draw->nStringBkMode = TRANSPARENT;
                draw->nHLStringBkMode = TRANSPARENT;

                if (is_hovered)
                {
                    DrawToolbarButtonBackground(draw->nmcd.hdc, draw->nmcd.rc, theme.HoverColor(), theme.BorderColor());
                    draw->nmcd.uItemState &= ~(CDIS_HOT | CDIS_CHECKED);
                }
                else if ((draw->nmcd.uItemState & CDIS_CHECKED) == CDIS_CHECKED)
                {
                    DrawToolbarButtonBackground(draw->nmcd.hdc, draw->nmcd.rc, theme.SurfaceColor(), theme.BorderColor());
                    draw->nmcd.uItemState &= ~CDIS_CHECKED;
                }

                LRESULT lr = TBCDRF_USECDCOLORS;
                if ((draw->nmcd.uItemState & CDIS_SELECTED) == CDIS_SELECTED)
                {
                    lr |= TBCDRF_NOBACKGROUND;
                }
                return lr;
            }
        default:
            break;
        }
        return CDRF_DODEFAULT;
    }
    return 0;
}

bool MainWindow::Impl::OpenSearchResultRow(int item, bool new_tab)
{
    const int index = SearchIndexFromTab(TabCtrl_GetCurSel(tab_));
    if (item < 0 || index < 0 || static_cast<size_t>(item) >= SearchRowCount(index))
    {
        return false;
    }
    const std::wstring path = SearchRowKeyPath(index, item);
    const search::Result* row = SearchResultAt(item);
    const bool value_row = row && !search::IsKeyRow(*row);
    const std::wstring value_name = value_row ? row->value_name : std::wstring();
    if (new_tab)
    {
        OpenLocalRegistryTab();
    }
    else
    {
        ActivateLocalRegistryTab();
    }
    ApplyViewVisibility();
    UpdateStatus();
    SelectTreePath(path);
    if (value_row)
    {
        SelectValueWhenReady(value_name);
    }
    return true;
}

bool MainWindow::Impl::OpenSelectedSearchResult(bool new_tab)
{
    if (!search_results_list_ || GetFocus() != search_results_list_ || IsCompareTabSelected())
    {
        return false;
    }
    return OpenSearchResultRow(ListView_GetNextItem(search_results_list_, -1, LVNI_SELECTED), new_tab);
}

LRESULT MainWindow::Impl::HandleTabNotification(NMHDR* header, LPARAM lparam)
{
    (void)lparam;
    if (header->hwndFrom == tab_ && header->code == TCN_SELCHANGING)
    {
        if (!suppress_tab_change_ && tab_)
        {
            int current = TabCtrl_GetCurSel(tab_);
            CaptureRegistryTabState(current);
        }
        return 0;
    }
    if (header->hwndFrom == tab_ && header->code == TCN_SELCHANGE)
    {
        if (suppress_tab_change_)
        {
            ApplyViewVisibility();
            UpdateSearchResultsView();
            UpdateStatus();
            return 0;
        }
        int sel = TabCtrl_GetCurSel(tab_);
        ApplyTabSelection(sel);
        ApplyViewVisibility();
        UpdateSearchResultsView();
        UpdateStatus();
        return 0;
    }
    return 0;
}

LRESULT MainWindow::Impl::HandleTreeNotification(NMHDR* header, LPARAM lparam)
{
    if (header->hwndFrom == regedit_compat_tree_.hwnd())
    {
        if (header->code == TVN_ITEMEXPANDINGW)
        {
            regedit_compat_tree_.OnItemExpanding(reinterpret_cast<NMTREEVIEWW*>(lparam));
            return 0;
        }
        if (header->code == TVN_GETDISPINFOW)
        {
            regedit_compat_tree_.OnGetDispInfo(reinterpret_cast<NMTVDISPINFOW*>(lparam));
            return 0;
        }
        if (header->code == TVN_SELCHANGEDW)
        {
            RegistryNode* node = regedit_compat_tree_.OnSelectionChanged(reinterpret_cast<NMTREEVIEWW*>(lparam));
            if (node)
            {
                QueueCompatJump(*node);
            }
            return 0;
        }
        return 0;
    }
    if (header->hwndFrom == browse_.tree().hwnd())
    {
        if (header->code == TVN_ITEMEXPANDINGW)
        {
            browse_.tree().OnItemExpanding(reinterpret_cast<NMTREEVIEWW*>(lparam));
            return 0;
        }
        if (header->code == TVN_GETDISPINFOW)
        {
            browse_.tree().OnGetDispInfo(reinterpret_cast<NMTVDISPINFOW*>(lparam));
            return 0;
        }
        if (header->code == TVN_ITEMEXPANDEDW)
        {
            if (!jump_ui_batch_active_)
            {
                MarkTreeStateDirty();
            }
            return 0;
        }
        if (header->code == TVN_BEGINLABELEDITW)
        {
            if (read_only_)
            {
                return TRUE;
            }
            auto* disp = reinterpret_cast<NMTVDISPINFOW*>(lparam);
            if (!disp)
            {
                return TRUE;
            }
            RegistryNode* node = browse_.tree().NodeFromItem(disp->item.hItem);
            if (!node || node->subkey.empty())
            {
                return TRUE;
            }
            HWND edit = TreeView_GetEditControl(browse_.tree().hwnd());
            if (edit)
            {
                Theme::Current().ApplyToWindow(edit);
                Theme::Current().ApplyToChildren(edit);
            }
            return FALSE;
        }
        if (header->code == TVN_ENDLABELEDITW)
        {
            if (read_only_)
            {
                return FALSE;
            }
            auto* disp = reinterpret_cast<NMTVDISPINFOW*>(lparam);
            if (!disp || !disp->item.pszText)
            {
                return FALSE;
            }
            RegistryNode* node = browse_.tree().NodeFromItem(disp->item.hItem);
            if (!node || node->subkey.empty())
            {
                return FALSE;
            }
            std::wstring new_name = registry_path::RawName(TrimWhitespace(disp->item.pszText));
            std::wstring old_name = LeafName(*node);
            if (new_name.empty() || EqualsInsensitive(new_name, old_name))
            {
                return FALSE;
            }
            RegistryNode rename_parent = *node;
            rename_parent.subkey = registry_path::Parent(node->subkey);
            if (KeyNameExists(rename_parent, new_name))
            {
                ReportNameTaken(hwnd_, util::Tr(L"A key with this name already exists:"), util::Tr(L"Rename Key"), new_name);
                return FALSE;
            }
            if (!RegistryStore::RenameKey(*node, new_name))
            {
                ui::ShowError(hwnd_, util::Tr(L"Failed to rename key."));
                return FALSE;
            }
            UpdateLeafName(node, new_name);
            if (browse_.current_node() && SameNode(*browse_.current_node(), *node))
            {
                UpdateAddressBar(browse_.current_node());
            }
            AppendHistoryEntry(L"Rename key", registry_path::DisplayName(old_name), registry_path::DisplayName(new_name));
            MarkOfflineDirty();
            changes::UndoOperation op;
            op.type = changes::UndoOperation::Type::kRenameKey;
            op.node = rename_parent;
            op.name = old_name;
            op.new_name = new_name;
            PushUndo(std::move(op));
            RefreshTreeSelection();
            UpdateValueListForNode(browse_.current_node());
            return TRUE;
        }
        if (header->code == TVN_SELCHANGEDW)
        {
            auto* info = reinterpret_cast<NMTREEVIEWW*>(lparam);
            RegistryNode* previous_node = browse_.current_node();
            RegistryNode* node = browse_.tree().OnSelectionChanged(info);
            if (startup_tree_restore_pending_ && !applying_startup_tree_restore_)
            {
                startup_tree_restore_pending_ = false;
                tree_state_restored_ = true;
            }
            browse_.set_current_node(node);
            if (previous_node && node && SameNode(*previous_node, *node))
            {
                return 0;
            }
            if (!jump_ui_batch_active_)
            {
                ApplyTreeSelectionEffects(node);
            }
            return 0;
        }
        if (header->code == NM_CUSTOMDRAW)
        {
            if (!Theme::UseDarkMode())
            {
                return CDRF_DODEFAULT;
            }
            auto* draw = reinterpret_cast<NMTVCUSTOMDRAW*>(lparam);
            if (!draw)
            {
                return CDRF_DODEFAULT;
            }
            switch (draw->nmcd.dwDrawStage)
            {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT:
                {
                    if (draw->nmcd.uItemState & CDIS_SELECTED)
                    {
                        return CDRF_DODEFAULT;
                    }
                    const Theme& theme = Theme::Current();
                    bool hot = (draw->nmcd.uItemState & CDIS_HOT) != 0;
                    if (hot)
                    {
                        draw->clrText = theme.TextColor();
                        draw->clrTextBk = theme.HoverColor();
                    }
                    else
                    {
                        draw->clrText = theme.TextColor();
                        draw->clrTextBk = theme.PanelColor();
                    }
                    return CDRF_NEWFONT;
                }
            default:
                break;
            }
        }
    }
    return 0;
}

LRESULT MainWindow::Impl::HandleHeaderNotification(NMHDR* header, LPARAM lparam)
{
    HWND value_header = ListView_GetHeader(browse_.values().hwnd());
    HWND history_header = ListView_GetHeader(history_list_);
    HWND search_header = ListView_GetHeader(search_results_list_);

    if (header->hwndFrom == value_header && (header->code == HDN_ENDTRACKW || header->code == HDN_ENDTRACKA ||
                                             header->code == HDN_ITEMCHANGEDW || header->code == HDN_ITEMCHANGEDA))
    {
        auto* info = reinterpret_cast<NMHEADERW*>(lparam);
        if (info && info->iItem >= 0 && info->pitem && (info->pitem->mask & HDI_WIDTH))
        {
            int subitem = GetListViewColumnSubItem(browse_.values().hwnd(), info->iItem);
            if (subitem >= 0 && static_cast<size_t>(subitem) < browse_.columns().widths.size())
            {
                browse_.columns().widths[static_cast<size_t>(subitem)] = info->pitem->cxy;
                if (header->code == HDN_ENDTRACKW || header->code == HDN_ENDTRACKA)
                {
                    SaveSettings();
                }
            }

            InvalidateListViewColumn(browse_.values().hwnd(), info->iItem);
            InvalidateListViewTail(browse_.values().hwnd());
        }
    }
    if (header->hwndFrom == history_header && (header->code == HDN_ENDTRACKW || header->code == HDN_ENDTRACKA ||
                                               header->code == HDN_ITEMCHANGEDW || header->code == HDN_ITEMCHANGEDA))
    {
        auto* info = reinterpret_cast<NMHEADERW*>(lparam);
        if (info && info->iItem >= 0 && info->pitem && (info->pitem->mask & HDI_WIDTH))
        {
            int subitem = GetListViewColumnSubItem(history_list_, info->iItem);
            if (subitem >= 0 && static_cast<size_t>(subitem) < history_column_widths_.size())
            {
                history_column_widths_[static_cast<size_t>(subitem)] = info->pitem->cxy;
            }
            InvalidateListViewColumn(history_list_, info->iItem);
            InvalidateListViewTail(history_list_);
        }
    }
    if (header->hwndFrom == search_header && (header->code == HDN_ENDTRACKW || header->code == HDN_ENDTRACKA ||
                                              header->code == HDN_ITEMCHANGEDW || header->code == HDN_ITEMCHANGEDA))
    {
        auto* info = reinterpret_cast<NMHEADERW*>(lparam);
        if (info && info->iItem >= 0 && info->pitem && (info->pitem->mask & HDI_WIDTH))
        {
            int subitem = GetListViewColumnSubItem(search_results_list_, info->iItem);
            bool compare = IsCompareTabSelected();
            auto& widths = compare ? compare_column_widths_ : search_column_widths_;
            if (subitem >= 0 && static_cast<size_t>(subitem) < widths.size())
            {
                widths[static_cast<size_t>(subitem)] = info->pitem->cxy;
            }
            InvalidateListViewColumn(search_results_list_, info->iItem);
            InvalidateListViewTail(search_results_list_);
        }
    }
    return 0;
}

LRESULT MainWindow::Impl::HandleValueNotification(NMHDR* header, LPARAM lparam)
{
    if (header->hwndFrom == browse_.values().hwnd() && header->code == LVN_ODCACHEHINT)
    {
        auto* hint = reinterpret_cast<NMLVCACHEHINT*>(lparam);
        if (hint)
        {
            QueueValuePreviews(hint->iFrom, hint->iTo);
        }
        return 0;
    }
    if (header->hwndFrom == browse_.values().hwnd() && header->code == LVN_GETDISPINFOW)
    {
        auto* disp = reinterpret_cast<NMLVDISPINFOW*>(lparam);
        ListRow* mutable_row = browse_.values().MutableRowAt(disp->item.iItem);
        const ListRow* row = mutable_row;
        if (!row)
        {
            if (disp->item.mask & LVIF_TEXT)
            {
                if (disp->item.pszText && disp->item.cchTextMax > 0)
                {
                    disp->item.pszText[0] = L'\0';
                }
            }
            if (disp->item.mask & LVIF_IMAGE)
            {
                disp->item.iImage = 0;
            }
            return 0;
        }
        if (disp->item.mask & LVIF_TEXT)
        {
            const int subitem = MappedSubItem(value_column_subitems_, disp->item.iSubItem);
            if (subitem == kValueColData && mutable_row && !mutable_row->data_ready && !value_preview_request_posted_)
            {
                value_preview_request_posted_ = true;
                if (!PostMessageW(hwnd_, frame::message_id::kValuePreviewRequest, static_cast<WPARAM>(disp->item.iItem), 0))
                {
                    value_preview_request_posted_ = false;
                }
            }
            const std::wstring& text = ValueRowFieldText(*row, subitem);
            if (text.size() > kCellTextDrawLimit && disp->item.pszText && disp->item.cchTextMax > 0)
            {
                lstrcpynW(disp->item.pszText, text.c_str(), disp->item.cchTextMax);
            }
            else
            {
                disp->item.pszText = const_cast<wchar_t*>(text.c_str());
            }
        }
        if (disp->item.mask & LVIF_IMAGE)
        {
            disp->item.iImage = row->image_index;
        }
        return 0;
    }
    if (header->hwndFrom == browse_.values().hwnd() && header->code == LVN_BEGINLABELEDITW)
    {
        if (read_only_)
        {
            return TRUE;
        }
        auto* disp = reinterpret_cast<NMLVDISPINFOW*>(lparam);
        if (!disp)
        {
            return TRUE;
        }
        const ListRow* row = browse_.values().RowAt(disp->item.iItem);
        if (!row || row->extra.empty() || (row->kind != rowkind::kValue && row->kind != rowkind::kKey))
        {
            return TRUE;
        }
        HWND edit = ListView_GetEditControl(browse_.values().hwnd());
        if (edit)
        {
            Theme::Current().ApplyToWindow(edit);
            Theme::Current().ApplyToChildren(edit);
        }
        return FALSE;
    }
    if (header->hwndFrom == browse_.values().hwnd() && header->code == LVN_ENDLABELEDITW)
    {
        if (read_only_)
        {
            return FALSE;
        }
        auto* disp = reinterpret_cast<NMLVDISPINFOW*>(lparam);
        if (!disp || !disp->item.pszText || !browse_.current_node())
        {
            return FALSE;
        }
        const ListRow* row = browse_.values().RowAt(disp->item.iItem);
        if (!row || row->extra.empty())
        {
            return FALSE;
        }
        std::wstring new_name = TrimWhitespace(disp->item.pszText);
        std::wstring old_name = row->extra;
        if (row->kind == rowkind::kKey)
        {
            new_name = registry_path::RawName(new_name);
        }
        if (new_name.empty() || EqualsInsensitive(new_name, old_name))
        {
            return FALSE;
        }
        if (row->kind == rowkind::kKey)
        {
            RegistryNode child = ChildNode(*browse_.current_node(), old_name);
            if (KeyNameExists(*browse_.current_node(), new_name))
            {
                ReportNameTaken(hwnd_, util::Tr(L"A key with this name already exists:"), util::Tr(L"Rename Key"), new_name);
                return FALSE;
            }
            if (!RegistryStore::RenameKey(child, new_name))
            {
                ui::ShowError(hwnd_, util::Tr(L"Failed to rename key."));
                return FALSE;
            }
            AppendHistoryEntry(L"Rename key " + registry_path::DisplayName(old_name), registry_path::DisplayName(old_name), registry_path::DisplayName(new_name));
            MarkOfflineDirty();
            changes::UndoOperation op;
            op.type = changes::UndoOperation::Type::kRenameKey;
            op.node = *browse_.current_node();
            op.name = old_name;
            op.new_name = new_name;
            PushUndo(std::move(op));
            RefreshTreeSelection();
            UpdateValueListForNode(browse_.current_node());
            return TRUE;
        }
        if (ValueNameExists(*browse_.current_node(), new_name))
        {
            ReportNameTaken(hwnd_, util::Tr(L"A value with this name already exists:"), util::Tr(L"Rename Value"), new_name);
            return FALSE;
        }
        bool both_names_left = false;
        if (!RegistryStore::RenameValue(*browse_.current_node(), old_name, new_name, &both_names_left))
        {
            if (both_names_left)
            {
                // value rename is a copy followed by delete and can fail in between
                MarkOfflineDirty();
                UpdateValueListForNode(browse_.current_node());
                ui::ShowError(hwnd_, util::Tr(L"The value was copied to the new name but the old name "
                                              L"couldn't be removed. Both names now exist."));
            }
            else
            {
                ui::ShowError(hwnd_, util::Tr(L"Failed to rename value."));
            }
            return FALSE;
        }
        AppendValueHistoryEntry(L"Rename value " + old_name, old_name, new_name, *browse_.current_node(), new_name, HistoryEntry::RevertKind::kNone);
        MarkOfflineDirty();
        changes::UndoOperation op;
        op.type = changes::UndoOperation::Type::kRenameValue;
        op.node = *browse_.current_node();
        op.name = old_name;
        op.new_name = new_name;
        PushUndo(std::move(op));
        if (EqualsInsensitive(old_name, appended_value_name_))
        {
            ListRow* updated = browse_.values().MutableRowAt(disp->item.iItem);
            if (updated)
            {
                updated->name = new_name;
                updated->extra = new_name;
                browse_.values().InvalidateFilterCache(updated);
                browse_.values().RefreshFilter();
                ListView_RedrawItems(browse_.values().hwnd(), disp->item.iItem, disp->item.iItem);
                browse_.SelectValue(new_name);
            }
            appended_value_name_.clear();
            return TRUE;
        }
        UpdateValueListForNode(browse_.current_node());
        retained_value_name_ = new_name;
        retained_value_key_path_ = registry_path::Build(*browse_.current_node());
        return TRUE;
    }
    if (header->hwndFrom == browse_.values().hwnd() && header->code == LVN_ITEMCHANGED)
    {
        auto* info = reinterpret_cast<NMLISTVIEW*>(lparam);
        if (!updating_value_list_ && info && ((info->uOldState ^ info->uNewState) & LVIS_SELECTED) != 0)
        {
            UpdateStatus();
        }
        return 0;
    }
    if (header->hwndFrom == browse_.values().hwnd() && header->code == LVN_COLUMNCLICK)
    {
        auto* info = reinterpret_cast<NMLISTVIEW*>(lparam);
        if (info)
        {
            SortValueList(info->iSubItem, true);
        }
        return 0;
    }
    if (header->hwndFrom == browse_.values().hwnd() && (header->code == NM_DBLCLK || header->code == LVN_ITEMACTIVATE))
    {
        auto* activate = reinterpret_cast<NMITEMACTIVATE*>(lparam);
        if (activate && activate->iItem >= 0 && browse_.current_node())
        {
            const ListRow* row = browse_.values().RowAt(activate->iItem);
            bool fast_activate = false;
            if (header->code == LVN_ITEMACTIVATE)
            {
                if (!value_activate_from_key_)
                {
                    return 0;
                }
                value_activate_from_key_ = false;
                fast_activate = true;
            }
            if (header->code == NM_DBLCLK)
            {
                fast_activate = true;
            }
            if (row && row->kind == rowkind::kKey)
            {
                if (fast_activate)
                {
                    std::wstring path = registry_path::Build(*browse_.current_node());
                    if (!row->extra.empty())
                    {
                        path.append(L"\\");
                        path.append(row->extra);
                    }
                    SelectTreePath(path);
                }
                return 0;
            }
            if (row && row->kind == rowkind::kValue)
            {
                if (activate->iSubItem == kValueColComment)
                {
                    HandleMenuCommand(cmd::kEditModifyComment);
                }
                else
                {
                    HandleMenuCommand(cmd::kEditModify);
                }
                return 0;
            }
        }
        return 0;
    }

    if (header->code == NM_CUSTOMDRAW)
    {
        auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
        if (!draw)
        {
            return CDRF_DODEFAULT;
        }
        LRESULT result = CDRF_DODEFAULT;
        switch (draw->nmcd.dwDrawStage)
        {
        case CDDS_PREPAINT:
            result = CDRF_NOTIFYITEMDRAW;
            break;
        case CDDS_ITEMPREPAINT:
            draw->nmcd.uItemState &= ~CDIS_FOCUS;
            break;
        default:
            break;
        }
        return appearance::HandleListGridCustomDraw(browse_.values().hwnd(), draw, result);
    }
    return 0;
}

search::Result* MainWindow::Impl::SearchResultAt(int item)
{
    const int tab_index = SearchIndexFromTab(TabCtrl_GetCurSel(tab_));
    if (item < 0 || tab_index < 0 || static_cast<size_t>(tab_index) >= search_tabs_.size())
    {
        return nullptr;
    }
    SearchTab& tab = search_tabs_[static_cast<size_t>(tab_index)];
    if (tab.is_compare)
    {
        return nullptr;
    }
    return static_cast<size_t>(item) < tab.results.size() ? &tab.results[static_cast<size_t>(item)] : nullptr;
}

std::wstring MainWindow::Impl::SearchRowKeyPath(int tab_index, int item) const
{
    if (item < 0 || tab_index < 0 || static_cast<size_t>(tab_index) >= search_tabs_.size())
    {
        return std::wstring();
    }
    const SearchTab& tab = search_tabs_[static_cast<size_t>(tab_index)];
    const size_t row = static_cast<size_t>(item);
    if (tab.is_compare)
    {
        return row < tab.compare_rows.size() ? tab.compare_rows[row].key_path : std::wstring();
    }
    return row < tab.results.size() ? tab.results[row].key_path : std::wstring();
}

size_t MainWindow::Impl::SearchRowCount(int tab_index) const
{
    if (tab_index < 0 || static_cast<size_t>(tab_index) >= search_tabs_.size())
    {
        return 0;
    }
    const SearchTab& tab = search_tabs_[static_cast<size_t>(tab_index)];
    return tab.is_compare ? tab.compare_rows.size() : tab.results.size();
}

LRESULT MainWindow::Impl::HandleSearchListCustomDraw(NMLVCUSTOMDRAW* draw)
{
    if (!draw || !search_results_list_)
    {
        return CDRF_DODEFAULT;
    }
    const int item = static_cast<int>(draw->nmcd.dwItemSpec);
    switch (draw->nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
        return appearance::HandleListGridCustomDraw(search_results_list_, draw, CDRF_NOTIFYITEMDRAW);
    case CDDS_ITEMPREPAINT:
        {
            draw->nmcd.uItemState &= ~CDIS_FOCUS;
            LRESULT stage = CDRF_DODEFAULT;
            const search::Result* result = SearchResultAt(item);
            if (result && result->match_length > 0)
            {
                stage |= CDRF_NOTIFYSUBITEMDRAW;
            }
            return appearance::HandleListGridCustomDraw(search_results_list_, draw, stage);
        }
    case CDDS_ITEMPOSTPAINT:
        return appearance::HandleListGridCustomDraw(search_results_list_, draw, CDRF_DODEFAULT);
    case CDDS_POSTPAINT:
        return appearance::HandleListGridCustomDraw(search_results_list_, draw, CDRF_DODEFAULT);
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
        {
            search::Result* result = SearchResultAt(item);
            if (!result || SearchMatchSubItem(*result) != MappedSubItem(search_column_subitems_, draw->iSubItem))
            {
                return CDRF_DODEFAULT;
            }
            return CDRF_NOTIFYPOSTPAINT;
        }
    case CDDS_ITEMPOSTPAINT | CDDS_SUBITEM:
        {
            const search::Result* result = SearchResultAt(item);
            const int subitem = MappedSubItem(search_column_subitems_, draw->iSubItem);
            if (!result || SearchMatchSubItem(*result) != subitem)
            {
                return CDRF_DODEFAULT;
            }
            RECT cell = {};
            if (draw->iSubItem == 0)
            {
                RECT row = {};
                if (!ListView_GetItemRect(search_results_list_, item, &cell, LVIR_LABEL) ||
                    !ListView_GetItemRect(search_results_list_, item, &row, LVIR_BOUNDS))
                {
                    return CDRF_DODEFAULT;
                }
                cell.right = row.left + ListView_GetColumnWidth(search_results_list_, 0);
                cell.left += kLabelTextInset;
            }
            else
            {
                cell.top = draw->iSubItem;
                cell.left = LVIR_BOUNDS;
                if (!SendMessageW(search_results_list_, LVM_GETSUBITEMRECT, item, reinterpret_cast<LPARAM>(&cell)))
                {
                    return CDRF_DODEFAULT;
                }
                cell.left += kCellTextPadding;
            }
            cell.right -= kCellTextPadding;
            if (cell.left >= cell.right)
            {
                return CDRF_DODEFAULT;
            }
            std::wstring_view cell_text;
            switch (subitem)
            {
            case 0:
                cell_text = result->key_path;
                break;
            case 1:
                cell_text = search::DisplayName(*result);
                break;
            case 3:
                cell_text = result->data_text;
                break;
            default:
                return CDRF_DODEFAULT;
            }
            HFONT font = reinterpret_cast<HFONT>(SendMessageW(search_results_list_, WM_GETFONT, 0, 0));
            HFONT old_font = font ? reinterpret_cast<HFONT>(SelectObject(draw->nmcd.hdc, font)) : nullptr;
            // draw matched range after the list view paints the full cell
            DrawSearchMatchOverlay(draw->nmcd.hdc, cell, cell_text, static_cast<int>(result->match_start), static_cast<int>(result->match_length));
            if (old_font)
            {
                SelectObject(draw->nmcd.hdc, old_font);
            }
            return CDRF_DODEFAULT;
        }
    default:
        return CDRF_DODEFAULT;
    }
}

LRESULT MainWindow::Impl::HandleHistoryNotification(NMHDR* header, LPARAM lparam)
{
    if (header->hwndFrom == history_list_ && header->code == LVN_GETDISPINFOW)
    {
        auto* disp = reinterpret_cast<NMLVDISPINFOW*>(lparam);
        const auto& entries = change_history_.entries();
        if (!disp || disp->item.iItem < 0 || static_cast<size_t>(disp->item.iItem) >= entries.size())
        {
            if (disp && (disp->item.mask & LVIF_TEXT) && disp->item.pszText && disp->item.cchTextMax > 0)
            {
                disp->item.pszText[0] = L'\0';
            }
            return 0;
        }
        if (disp->item.mask & LVIF_TEXT)
        {
            const auto& entry = entries[static_cast<size_t>(disp->item.iItem)];
            const std::wstring* text = &entry.time_text;
            switch (disp->item.iSubItem)
            {
            case 1:
                text = &entry.action;
                break;
            case 2:
                text = &entry.old_data;
                break;
            case 3:
                text = &entry.new_data;
                break;
            default:
                break;
            }
            if (text->size() > kCellTextDrawLimit && disp->item.pszText && disp->item.cchTextMax > 0)
            {
                lstrcpynW(disp->item.pszText, text->c_str(), disp->item.cchTextMax);
            }
            else
            {
                disp->item.pszText = const_cast<wchar_t*>(text->c_str());
            }
        }
        return 0;
    }
    if (header->hwndFrom == history_list_ && header->code == LVN_COLUMNCLICK)
    {
        auto* info = reinterpret_cast<NMLISTVIEW*>(lparam);
        if (info)
        {
            SortHistoryList(info->iSubItem, true);
        }
        return 0;
    }
    if (header->code == NM_CUSTOMDRAW)
    {
        auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
        if (!draw)
        {
            return CDRF_DODEFAULT;
        }
        LRESULT result = CDRF_DODEFAULT;
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
        {
            result = CDRF_NOTIFYITEMDRAW;
        }
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
        {
            draw->nmcd.uItemState &= ~CDIS_FOCUS;
        }
        return appearance::HandleListGridCustomDraw(history_list_, draw, result);
    }
    return 0;
}

LRESULT MainWindow::Impl::HandleSearchNotification(NMHDR* header, LPARAM lparam)
{
    if (header->hwndFrom == search_results_list_ && header->code == LVN_ODCACHEHINT)
    {
        auto* hint = reinterpret_cast<NMLVCACHEHINT*>(lparam);
        if (hint)
        {
            QueueSearchPreviews(hint->iFrom, hint->iTo);
        }
        return 0;
    }
    if (header->hwndFrom == search_results_list_ && header->code == LVN_GETDISPINFOW)
    {
        auto* disp = reinterpret_cast<NMLVDISPINFOW*>(lparam);
        const int index = SearchIndexFromTab(TabCtrl_GetCurSel(tab_));
        SearchTab* tab = index >= 0 && static_cast<size_t>(index) < search_tabs_.size()
                             ? &search_tabs_[static_cast<size_t>(index)]
                             : nullptr;
        auto clear_cell = [&]() {
            if ((disp->item.mask & LVIF_TEXT) && disp->item.pszText && disp->item.cchTextMax > 0)
            {
                disp->item.pszText[0] = L'\0';
            }
            if (disp->item.mask & LVIF_IMAGE)
            {
                disp->item.iImage = 0;
            }
        };
        if (!tab || disp->item.iItem < 0)
        {
            clear_cell();
            return 0;
        }
        const size_t row_index = static_cast<size_t>(disp->item.iItem);
        const int subitem = MappedSubItem(search_column_subitems_, disp->item.iSubItem);

        if (tab->is_compare)
        {
            if (row_index >= tab->compare_rows.size())
            {
                clear_cell();
                return 0;
            }
            const search::compare::Row& row = tab->compare_rows[row_index];
            if (disp->item.mask & LVIF_TEXT)
            {
                if (disp->item.pszText && disp->item.cchTextMax > 0)
                {
                    disp->item.pszText[0] = L'\0';
                }
                auto set_text = [&](const std::wstring& text) {
                    if (text.size() > kCellTextDrawLimit && disp->item.pszText && disp->item.cchTextMax > 0)
                    {
                        lstrcpynW(disp->item.pszText, text.c_str(), disp->item.cchTextMax);
                    }
                    else
                    {
                        disp->item.pszText = const_cast<wchar_t*>(text.c_str());
                    }
                };
                switch (subitem)
                {
                case 0:
                    set_text(row.key_path);
                    break;
                case 1:
                    if (row.is_key)
                    {
                        disp->item.pszText = const_cast<wchar_t*>(util::Tr(L"(Key)"));
                    }
                    else if (row.value_name.empty())
                    {
                        disp->item.pszText = const_cast<wchar_t*>(util::Tr(L"(Default)"));
                    }
                    else
                    {
                        set_text(row.value_name);
                    }
                    break;
                case 2:
                    set_text(row.first_text);
                    break;
                case 3:
                    set_text(row.second_text);
                    break;
                case 4:
                    disp->item.pszText = const_cast<wchar_t*>(row.matches ? util::Tr(L"Same") : util::Tr(L"Different"));
                    break;
                default:
                    break;
                }
            }
            if (disp->item.mask & LVIF_IMAGE)
            {
                disp->item.iImage = row.is_key ? kFolderIconIndex : kValueIconIndex;
            }
            return 0;
        }

        if (row_index >= tab->results.size())
        {
            clear_cell();
            return 0;
        }
        search::Result& result = tab->results[row_index];

        if (subitem == 3 && result.data_state == search::DataState::kNotLoaded && !search_preview_request_posted_)
        {
            search_preview_request_posted_ = true;
            if (!PostMessageW(hwnd_, frame::message_id::kSearchPreviewRequest, 0, 0))
            {
                search_preview_request_posted_ = false;
            }
        }
        if (disp->item.mask & LVIF_TEXT)
        {
            wchar_t* buffer = disp->item.pszText;
            const int capacity = disp->item.cchTextMax;
            if (buffer && capacity > 0)
            {
                buffer[0] = L'\0';
            }
            auto set_text = [&](const std::wstring& text) {
                if (text.size() > kCellTextDrawLimit && buffer && capacity > 0)
                {
                    lstrcpynW(buffer, text.c_str(), capacity);
                }
                else
                {
                    disp->item.pszText = const_cast<wchar_t*>(text.c_str());
                }
            };
            switch (subitem)
            {
            case 0:
                set_text(result.key_path);
                break;
            case 1:
                if (search::IsKeyRow(result))
                {
                    disp->item.pszText = const_cast<wchar_t*>(L"");
                }
                else if (result.value_name.empty())
                {
                    disp->item.pszText = const_cast<wchar_t*>(util::Tr(L"(Default)"));
                }
                else
                {
                    disp->item.pszText = const_cast<wchar_t*>(result.value_name.c_str());
                }
                break;
            case 2:
                if (search::IsKeyRow(result))
                {
                    disp->item.pszText = const_cast<wchar_t*>(util::Tr(L"Key"));
                }
                else if (result.kind == search::ResultKind::kTraceValue)
                {
                    disp->item.pszText = const_cast<wchar_t*>(L"TRACE");
                }
                else if (buffer && capacity > 0)
                {
                    lstrcpynW(buffer, value_format::TypeName(result.type).c_str(), capacity);
                }
                break;
            case 3:
                set_text(result.data_text);
                break;
            case 4:
                if (buffer && capacity > 0 && !search::IsKeyRow(result) &&
                    result.kind != search::ResultKind::kTraceValue)
                {
                    swprintf_s(buffer, static_cast<size_t>(capacity), L"%lu", static_cast<unsigned long>(result.data_size));
                }
                break;
            case 5:
                if (buffer && capacity > 0)
                {
                    FormatCellFileTime(result.modified, buffer, capacity);
                }
                break;
            case 6:
                if (buffer && capacity > 0 && result.source < tab->sources.size())
                {
                    std::wstring label = search::SourceLabel(tab->sources[result.source]);
                    if (const wchar_t* field = search::MatchFieldLabel(result.match_field))
                    {
                        label.append(L" (").append(field).append(L")");
                    }
                    lstrcpynW(buffer, label.c_str(), capacity);
                }
                break;
            default:
                break;
            }
        }
        if (disp->item.mask & LVIF_IMAGE)
        {
            if (search::IsKeyRow(result))
            {
                disp->item.iImage = kFolderIconIndex;
            }
            else if (result.kind == search::ResultKind::kTraceValue)
            {
                disp->item.iImage = kTraceIconIndex;
            }
            else if (UseBinaryValueIcon(result.type))
            {
                disp->item.iImage = kBinaryIconIndex;
            }
            else
            {
                disp->item.iImage = kValueIconIndex;
            }
        }
        return 0;
    }
    if (header->hwndFrom == search_results_list_ && header->code == LVN_COLUMNCLICK)
    {
        auto* info = reinterpret_cast<NMLISTVIEW*>(lparam);
        if (info)
        {
            SortSearchResults(info->iSubItem, true);
        }
        return 0;
    }
    if (header->hwndFrom == search_results_list_ && (header->code == NM_DBLCLK || header->code == LVN_ITEMACTIVATE))
    {
        if (IsCompareTabSelected())
        {
            return 0;
        }
        auto* activate = reinterpret_cast<NMITEMACTIVATE*>(lparam);
        if (activate && activate->iItem >= 0)
        {
            OpenSearchResultRow(activate->iItem, SearchResultOpensInNewTab());
        }
        return 0;
    }
    if (header->code == NM_CUSTOMDRAW)
    {
        return HandleSearchListCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(lparam));
    }
    return 0;
}

} // namespace regkit
