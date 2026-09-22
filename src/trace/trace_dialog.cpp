// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "trace/trace_dialog.h"

#include <algorithm>
#include <cwchar>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <commctrl.h>
#include <windowsx.h>

#include "appearance/dialog_layout.h"
#include "appearance/dialog_metrics.h"
#include "appearance/feedback.h"
#include "appearance/theme.h"
#include "registry/registry_path.h"
#include "win32/text_transform.h"
#include "win32/window_metrics.h"

#ifndef NMTVITEMCHANGE
typedef struct tagNMTVITEMCHANGE
{
    NMHDR hdr;
    UINT uChanged;
    HTREEITEM hItem;
    UINT uStateNew;
    UINT uStateOld;
    LPARAM lParam;
} NMTVITEMCHANGE;
#endif

namespace regkit
{

namespace
{

constexpr wchar_t kDialogClass[] = L"RegKitTraceDialog";
constexpr UINT kDialogAddEntriesMessage = WM_APP + 1;
constexpr UINT kDialogDoneMessage = WM_APP + 2;
constexpr UINT kDialogProcessEntriesMessage = WM_APP + 3;

using util::ToLower;

enum ControlId
{
    kTraceLabel = 100,
    kTraceStatus = 101,
    kTraceTree = 102,
    kRecursiveCheck = 103,
    kSelectAllButton = 104,
    kOkButton = IDOK,
    kCancelButton = IDCANCEL,
};

struct TraceNodeData
{
    bool is_value = false;
    std::wstring key_path;
    std::wstring value_name;
};

struct TraceDialogState : appearance::DialogWindow
{
    HWND label = nullptr;
    HWND status = nullptr;
    HWND tree = nullptr;
    HWND recursive = nullptr;
    HWND select_all = nullptr;
    HWND ok_button = nullptr;
    HWND cancel_button = nullptr;
    trace::Selection* out = nullptr;
    bool loading_done = false;
    bool show_values = true;
    TraceDialogReadyCallback on_ready = nullptr;
    void* on_ready_context = nullptr;
    std::wstring prompt;
    bool processing_entries = false;
    bool updating_checks = false;
    size_t pending_index = 0;

    std::unordered_map<std::wstring, HTREEITEM> key_nodes;
    std::unordered_set<std::wstring> key_entries;
    std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>> values_by_key;
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> values_loaded;
    std::vector<std::unique_ptr<TraceNodeData>> node_storage;
    std::vector<KeyValueDialogEntry> pending_entries;
    size_t key_count = 0;
    size_t value_count = 0;
};

TraceNodeData* StoreNodeData(TraceDialogState* state, bool is_value, const std::wstring& key_path, const std::wstring& value_name)
{
    auto node = std::make_unique<TraceNodeData>();
    node->is_value = is_value;
    node->key_path = key_path;
    node->value_name = value_name;
    TraceNodeData* raw = node.get();
    state->node_storage.push_back(std::move(node));
    return raw;
}

void UpdateStatus(TraceDialogState* state)
{
    if (!state || !state->status)
    {
        return;
    }
    std::wstring text = L"Loaded ";
    text.append(std::to_wstring(state->key_count));
    text.append(L" keys");
    if (state->show_values)
    {
        text.append(L", ");
        text.append(std::to_wstring(state->value_count));
        text.append(L" values");
    }
    const bool ready = state->loading_done && !state->processing_entries;
    if (!ready)
    {
        text.append(L" (loading...)");
    }
    EnableWindow(state->ok_button, ready);
    SetWindowTextW(state->status, text.c_str());
}

HTREEITEM EnsureKeyNode(HWND tree, TraceDialogState* state, const std::wstring& key_path, const std::wstring& display_path)
{
    if (!state || !tree || key_path.empty())
    {
        return nullptr;
    }
    std::wstring tree_path = display_path.empty() ? key_path : display_path;
    std::vector<std::wstring> tree_parts = registry_path::Split(tree_path);
    if (tree_parts.empty())
    {
        return nullptr;
    }
    std::vector<std::wstring> key_parts = registry_path::Split(key_path);
    size_t match_suffix = 0;
    while (match_suffix < tree_parts.size() && match_suffix < key_parts.size())
    {
        size_t tree_index = tree_parts.size() - 1 - match_suffix;
        size_t key_index = key_parts.size() - 1 - match_suffix;
        if (!util::EqualsInsensitive(tree_parts[tree_index], key_parts[key_index]))
        {
            break;
        }
        ++match_suffix;
    }
    size_t align_start_tree = tree_parts.size() - match_suffix;
    size_t align_start_key = key_parts.size() - match_suffix;
    auto key_prefix_for_index = [&](size_t index) -> std::wstring {
        if (key_parts.empty())
        {
            return L"";
        }
        size_t part_count = 0;
        if (index < align_start_tree)
        {
            if (index == 0 && util::EqualsInsensitive(tree_parts[0], L"REGISTRY"))
            {
                return L"";
            }
            part_count = 1;
        }
        else
        {
            part_count = align_start_key + (index - align_start_tree) + 1;
        }
        return registry_path::JoinPrefix(key_parts, part_count);
    };

    std::wstring current_tree;
    HTREEITEM parent = TVI_ROOT;
    for (size_t i = 0; i < tree_parts.size(); ++i)
    {
        if (!current_tree.empty())
        {
            current_tree.append(L"\\");
        }
        current_tree.append(tree_parts[i]);
        std::wstring tree_lower = ToLower(current_tree);
        auto it = state->key_nodes.find(tree_lower);
        if (it != state->key_nodes.end())
        {
            parent = it->second;
            continue;
        }

        std::wstring node_key_path = (i + 1 == tree_parts.size()) ? key_path : key_prefix_for_index(i);
        TraceNodeData* data = StoreNodeData(state, false, node_key_path, L"");
        TVINSERTSTRUCTW insert = {};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        insert.item.pszText = const_cast<wchar_t*>(tree_parts[i].c_str());
        insert.item.lParam = reinterpret_cast<LPARAM>(data);
        HTREEITEM item = TreeView_InsertItem(tree, &insert);
        if (parent != TVI_ROOT && TreeView_GetCheckState(tree, parent) != FALSE)
        {
            bool prior = state->updating_checks;
            state->updating_checks = true;
            TreeView_SetCheckState(tree, item, TRUE);
            state->updating_checks = prior;
        }
        state->key_nodes.emplace(std::move(tree_lower), item);
        parent = item;
    }
    return parent == TVI_ROOT ? nullptr : parent;
}

HTREEITEM InsertValueNode(HWND tree, TraceDialogState* state, HTREEITEM key_item, const std::wstring& key_path, const std::wstring& value_name)
{
    if (!state || !tree || !key_item)
    {
        return nullptr;
    }
    std::wstring display = value_name.empty() ? L"(Default)" : value_name;
    TraceNodeData* data = StoreNodeData(state, true, key_path, value_name);
    TVINSERTSTRUCTW insert = {};
    insert.hParent = key_item;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = const_cast<wchar_t*>(display.c_str());
    insert.item.lParam = reinterpret_cast<LPARAM>(data);
    HTREEITEM item = TreeView_InsertItem(tree, &insert);
    if (TreeView_GetCheckState(tree, key_item) != FALSE)
    {
        bool prior = state->updating_checks;
        state->updating_checks = true;
        TreeView_SetCheckState(tree, item, TRUE);
        state->updating_checks = prior;
    }
    return item;
}

void EnsureValueNodes(HWND tree, TraceDialogState* state, HTREEITEM key_item, const std::wstring& key_path)
{
    if (!state || !tree || !key_item || key_path.empty())
    {
        return;
    }
    std::wstring key_lower = ToLower(key_path);
    auto values_it = state->values_by_key.find(key_lower);
    if (values_it == state->values_by_key.end())
    {
        return;
    }
    auto& loaded = state->values_loaded[key_lower];
    for (const auto& entry : values_it->second)
    {
        const std::wstring& value_lower = entry.first;
        const std::wstring& display = entry.second;
        if (!loaded.insert(value_lower).second)
        {
            continue;
        }
        std::wstring value_name = display == L"(Default)" ? L"" : display;
        InsertValueNode(tree, state, key_item, key_path, value_name);
    }
}

void AddEntry(HWND tree, TraceDialogState* state, const KeyValueDialogEntry& entry)
{
    if (!state || !tree || entry.key_path.empty())
    {
        return;
    }
    std::wstring display_path = entry.display_path.empty() ? entry.key_path : entry.display_path;
    HTREEITEM key_item = EnsureKeyNode(tree, state, entry.key_path, display_path);
    std::wstring key_lower = ToLower(entry.key_path);
    if (state->key_entries.insert(key_lower).second)
    {
        state->key_count++;
    }
    if (!entry.has_value || !state->show_values)
    {
        return;
    }
    std::wstring value_name = entry.value_name;
    std::wstring display_name = value_name.empty() ? L"(Default)" : value_name;
    std::wstring value_lower = ToLower(value_name);
    auto& values = state->values_by_key[key_lower];
    if (values.emplace(value_lower, display_name).second)
    {
        state->value_count++;
        if (key_item)
        {
            UINT state_mask = TreeView_GetItemState(tree, key_item, TVIS_EXPANDED);
            if (state_mask & TVIS_EXPANDED)
            {
                InsertValueNode(tree, state, key_item, entry.key_path, value_name);
            }
        }
    }
}

TraceNodeData* GetNodeData(HWND tree, HTREEITEM item)
{
    if (!tree || !item)
    {
        return nullptr;
    }
    TVITEMW info = {};
    info.mask = TVIF_PARAM;
    info.hItem = item;
    if (!TreeView_GetItem(tree, &info))
    {
        return nullptr;
    }
    return reinterpret_cast<TraceNodeData*>(info.lParam);
}

void AppendCheckedNodes(HWND tree, HTREEITEM item, trace::Selection* selection, std::unordered_set<std::wstring>* seen_keys)
{
    while (item)
    {
        TraceNodeData* data = GetNodeData(tree, item);
        if (data && TreeView_GetCheckState(tree, item))
        {
            if (data->is_value)
            {
                std::wstring key_lower = ToLower(data->key_path);
                std::wstring value_lower = ToLower(data->value_name);
                selection->values_by_key[key_lower].insert(value_lower);
                if (seen_keys && seen_keys->insert(key_lower).second)
                {
                    selection->key_paths.push_back(data->key_path);
                }
            }
            else
            {
                std::wstring key_lower = ToLower(data->key_path);
                if (seen_keys && seen_keys->insert(key_lower).second)
                {
                    selection->key_paths.push_back(data->key_path);
                }
                for (HTREEITEM child = TreeView_GetChild(tree, item); child;
                     child = TreeView_GetNextSibling(tree, child))
                {
                    TraceNodeData* child_data = GetNodeData(tree, child);
                    if (child_data && child_data->is_value)
                    {
                        selection->values_by_key[key_lower];
                        break;
                    }
                }
            }
        }
        HTREEITEM child = TreeView_GetChild(tree, item);
        if (child)
        {
            AppendCheckedNodes(tree, child, selection, seen_keys);
        }
        item = TreeView_GetNextSibling(tree, item);
    }
}

void ApplyCheckStateToChildren(HWND tree, HTREEITEM parent, bool checked)
{
    if (!tree || !parent)
    {
        return;
    }
    HTREEITEM child = TreeView_GetChild(tree, parent);
    while (child)
    {
        TreeView_SetCheckState(tree, child, checked);
        ApplyCheckStateToChildren(tree, child, checked);
        child = TreeView_GetNextSibling(tree, child);
    }
}

void QueueEntries(HWND hwnd, TraceDialogState* state, std::vector<KeyValueDialogEntry>&& entries)
{
    if (!state || entries.empty())
    {
        return;
    }
    state->pending_entries.reserve(state->pending_entries.size() + entries.size());
    for (auto& entry : entries)
    {
        state->pending_entries.push_back(std::move(entry));
    }
    if (!state->processing_entries)
    {
        state->processing_entries = true;
        PostMessageW(hwnd, kDialogProcessEntriesMessage, 0, 0);
    }
}

void ProcessPendingEntries(HWND hwnd, TraceDialogState* state)
{
    if (!state || !state->processing_entries)
    {
        return;
    }
    constexpr size_t kBatchSize = 128;
    constexpr DWORD kBatchMs = 8;
    uint64_t start_tick = GetTickCount64();
    size_t processed = 0;
    while (state->pending_index < state->pending_entries.size())
    {
        AddEntry(state->tree, state, state->pending_entries[state->pending_index]);
        ++state->pending_index;
        ++processed;
        if (processed >= kBatchSize)
        {
            break;
        }
        if (GetTickCount64() - start_tick >= kBatchMs)
        {
            break;
        }
    }
    if (state->pending_index >= state->pending_entries.size())
    {
        state->pending_entries.clear();
        state->pending_index = 0;
        state->processing_entries = false;
    }
    else
    {
        PostMessageW(hwnd, kDialogProcessEntriesMessage, 0, 0);
    }
    UpdateStatus(state);
}

void AcceptSelection(HWND hwnd, TraceDialogState* state, bool select_all)
{
    if (!state || !state->out)
    {
        return;
    }
    bool recursive = Button_GetCheck(state->recursive) == BST_CHECKED;
    if (select_all)
    {
        state->out->select_all = true;
        state->out->recursive = recursive;
        state->out->key_paths.clear();
        state->out->values_by_key.clear();
        appearance::CloseDialogWindow(state, true);
        return;
    }

    trace::Selection selection = {};
    selection.select_all = false;
    selection.recursive = recursive;
    std::unordered_set<std::wstring> seen_keys;
    HTREEITEM root = TreeView_GetRoot(state->tree);
    if (root)
    {
        AppendCheckedNodes(state->tree, root, &selection, &seen_keys);
    }
    if (selection.key_paths.empty() && selection.values_by_key.empty())
    {
        ui::ShowWarning(hwnd, L"Select at least one key or value.");
        return;
    }
    *state->out = std::move(selection);
    appearance::CloseDialogWindow(state, true);
}

void LayoutDialog(HWND hwnd, TraceDialogState* state, HFONT font)
{
    if (!state)
    {
        return;
    }
    RECT rect = {};
    GetClientRect(hwnd, &rect);
    using namespace appearance::metrics;
    const UINT dpi = win32::DpiForWindow(hwnd);
    const int padding = Scaled(kDialogContentMargin, dpi);
    const int gap = Scaled(kRowGap, dpi);
    const int block_gap = Scaled(kBlockGap, dpi);
    const int right_margin = Scaled(kDialogButtonRightMargin, dpi);
    const int bottom_margin = Scaled(kDialogButtonBottomMargin, dpi);
    const int button_h = Scaled(kButtonHeight, dpi);
    const int button_w = Scaled(kButtonMinWidth, dpi);
    const int button_gap = Scaled(kButtonGap, dpi);
    const int check_h = Scaled(kCheckHeight, dpi);
    const int label_h = Scaled(kLabelHeight, dpi);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int content_w = width - padding * 2;

    int y = padding;
    if (state->label)
    {
        appearance::Place(state->label, padding, y, content_w, label_h);
        y += label_h + gap;
    }
    if (state->status)
    {
        appearance::Place(state->status, padding, y, content_w, label_h);
        y += label_h + gap;
    }

    const int buttons_y = height - bottom_margin - button_h;
    const int check_y = buttons_y - button_h - block_gap;
    const int tree_height = std::max(Scaled(80, dpi), check_y - y - block_gap);
    appearance::Place(state->tree, padding, y, content_w, tree_height);

    const int select_all_w = Scaled(135, dpi);
    const int recursive_w = Scaled(160, dpi);
    appearance::Place(state->select_all, padding, check_y, select_all_w, button_h);
    appearance::Place(state->recursive, padding + select_all_w + gap, check_y + (button_h - check_h) / 2, recursive_w, check_h);

    const int cancel_x = width - right_margin - button_w;
    const int ok_x = cancel_x - button_w - button_gap;
    appearance::Place(state->ok_button, ok_x, buttons_y, button_w, button_h);
    appearance::Place(state->cancel_button, cancel_x, buttons_y, button_w, button_h);

    if (font)
    {
        appearance::SetControlFont(hwnd, font);
    }
}

LRESULT CALLBACK TraceDialogProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    auto* state = appearance::DialogWindowState<TraceDialogState>(hwnd);
    switch (msg)
    {
    case WM_CREATE:
        {
            if (!state->prompt.empty())
            {
                state->label = CreateWindowExW(0, L"STATIC", state->prompt.c_str(), WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kTraceLabel), nullptr, nullptr);
            }
            state->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kTraceStatus), nullptr, nullptr);
            state->tree = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_CHECKBOXES, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kTraceTree), nullptr, nullptr);
            state->recursive =
                CreateWindowExW(0, L"BUTTON", L"Recursive", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kRecursiveCheck), nullptr, nullptr);
            state->select_all =
                CreateWindowExW(0, L"BUTTON", L"Select All Keys", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kSelectAllButton), nullptr, nullptr);
            state->ok_button =
                CreateWindowExW(0, L"BUTTON", L"Select", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kOkButton), nullptr, nullptr);
            state->cancel_button =
                CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kCancelButton), nullptr, nullptr);
            appearance::SetDialogFont(hwnd, state->font);
            Button_SetCheck(state->recursive, BST_CHECKED);
            SendMessageW(state->tree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
            state->focus = state->tree;
            UpdateStatus(state);
            LayoutDialog(hwnd, state, state->font);
            if (state->on_ready)
            {
                state->on_ready(hwnd, state->on_ready_context);
            }
            return 0;
        }
    case WM_DESTROY:
        {
            MSG pending = {};
            while (PeekMessageW(&pending, hwnd, kDialogAddEntriesMessage, kDialogAddEntriesMessage, PM_REMOVE))
            {
                delete reinterpret_cast<std::vector<KeyValueDialogEntry>*>(pending.lParam);
            }
            break;
        }
    case WM_SIZE:
        LayoutDialog(hwnd, state, state->font);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED && (LOWORD(wparam) == kSelectAllButton || LOWORD(wparam) == kOkButton))
        {
            AcceptSelection(hwnd, state, LOWORD(wparam) == kSelectAllButton);
            return 0;
        }
        break;
    case WM_NOTIFY:
        {
            auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (header && header->code == TVN_ITEMEXPANDINGW)
            {
                auto* info = reinterpret_cast<NMTREEVIEWW*>(lparam);
                if (info->action == TVE_EXPAND)
                {
                    TraceNodeData* data = GetNodeData(state->tree, info->itemNew.hItem);
                    if (data && !data->is_value)
                    {
                        EnsureValueNodes(state->tree, state, info->itemNew.hItem, data->key_path);
                    }
                }
            }
            if (header && header->code == TVN_ITEMCHANGEDW)
            {
                auto* change = reinterpret_cast<NMTVITEMCHANGE*>(lparam);
                if (change && (change->uChanged & TVIF_STATE) &&
                    ((change->uStateNew ^ change->uStateOld) & TVIS_STATEIMAGEMASK))
                {
                    if (!state->updating_checks)
                    {
                        TraceNodeData* data = GetNodeData(state->tree, change->hItem);
                        if (data && !data->is_value)
                        {
                            bool checked = TreeView_GetCheckState(state->tree, change->hItem) != FALSE;
                            state->updating_checks = true;
                            ApplyCheckStateToChildren(state->tree, change->hItem, checked);
                            state->updating_checks = false;
                        }
                    }
                }
            }
            break;
        }
    case kDialogAddEntriesMessage:
        {
            std::unique_ptr<std::vector<KeyValueDialogEntry>> owned(
                reinterpret_cast<std::vector<KeyValueDialogEntry>*>(lparam)
            );
            if (owned)
            {
                QueueEntries(hwnd, state, std::move(*owned));
            }
            return 0;
        }
    case kDialogDoneMessage:
        state->loading_done = wparam != 0;
        UpdateStatus(state);
        return 0;
    case kDialogProcessEntriesMessage:
        ProcessPendingEntries(hwnd, state);
        return 0;
    default:
        break;
    }
    return appearance::DefDialogWindowProc(hwnd, msg, wparam, lparam);
}

} // namespace

bool ShowTraceDialog(HWND owner, const TraceDialogOptions& options, trace::Selection* selection, TraceDialogReadyCallback on_ready, void* context)
{
    TraceDialogState state;
    state.out = selection;
    state.owner = owner;
    state.show_values = options.show_values;
    state.on_ready = on_ready;
    state.on_ready_context = context;
    state.prompt = options.prompt;
    const UINT dpi = win32::DpiForWindow(owner);
    return selection &&
           appearance::RunDialogWindow(&state, kDialogClass, TraceDialogProc, options.title.empty() ? L"Trace" : options.title.c_str(), {appearance::metrics::Scaled(560, dpi), appearance::metrics::Scaled(552, dpi)});
}
void TraceDialogPostEntries(HWND dialog, std::vector<KeyValueDialogEntry>* entries)
{
    if (!dialog || !entries)
    {
        delete entries;
        return;
    }
    if (!PostMessageW(dialog, kDialogAddEntriesMessage, 0, reinterpret_cast<LPARAM>(entries)))
    {
        delete entries;
    }
}

void TraceDialogPostDone(HWND dialog, bool done)
{
    if (!dialog)
    {
        return;
    }
    PostMessageW(dialog, kDialogDoneMessage, done ? 1 : 0, 0);
}

} // namespace regkit
