// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/key_handles_window.h"

#include "appearance/dialog_layout.h"
#include "appearance/feedback.h"
#include "appearance/list_view_support.h"
#include "appearance/theme.h"
#include "editors/dialog_support.h"
#include "registry/registry_path.h"
#include "resource.h"
#include "win32/key_handle_snapshot.h"
#include "win32/process_rights.h"
#include "win32/system_error.h"
#include "win32/text_transform.h"
#include "win32/window_metrics.h"
#include "work/session.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace regkit {

namespace {

namespace support = editors::dialog_support;

constexpr UINT kSnapshotReady = WM_APP + 1;

enum Column {
  kProcess,
  kPid,
  kHandle,
  kObject,
  kKey,
  kAccess,
  kAttributes,
  kNative,
  kColumnCount,
};

enum MenuCommand {
  kMenuOpen = 1,
  kMenuOpenNewTab,
  kMenuCopyKey,
  kMenuCopyNative,
  kMenuCopyProcess,
  kMenuCopyPid,
  kMenuCopyHandle,
  kMenuCopyAccess,
  kMenuCopyRows,
  kMenuRefresh,
  kMenuClose,
};

struct Row {
  std::array<std::wstring, kColumnCount> text;
  ::win32::KeyHandle handle;
};

struct Payload {
  uint64_t generation = 0;
  std::vector<Row> rows;
  size_t inaccessible = 0;
  DWORD error = ERROR_SUCCESS;
};

struct State {
  KeyHandlesNavigate navigate;
  HFONT font = nullptr;
  HWND list = nullptr;
  HWND status = nullptr;
  appearance::DialogResizer resizer;
  work::Session session;
  std::vector<Row> rows;
  std::vector<size_t> view;
  size_t inaccessible = 0;
  DWORD error = ERROR_SUCCESS;
  bool scanning = false;
  int sort_column = kPid;
  bool ascending = true;
};

std::wstring DecodeAccess(
    ACCESS_MASK mask
) {
  static constexpr std::pair<ACCESS_MASK, const wchar_t*> kRights[] = {
      {KEY_ALL_ACCESS, L"KEY_ALL_ACCESS"},
      {KEY_READ, L"KEY_READ"},
      {KEY_WRITE, L"KEY_WRITE"},
      {KEY_QUERY_VALUE, L"KEY_QUERY_VALUE"},
      {KEY_SET_VALUE, L"KEY_SET_VALUE"},
      {KEY_CREATE_SUB_KEY, L"KEY_CREATE_SUB_KEY"},
      {KEY_ENUMERATE_SUB_KEYS, L"KEY_ENUMERATE_SUB_KEYS"},
      {KEY_NOTIFY, L"KEY_NOTIFY"},
      {KEY_CREATE_LINK, L"KEY_CREATE_LINK"},
      {DELETE, L"DELETE"},
      {READ_CONTROL, L"READ_CONTROL"},
      {WRITE_DAC, L"WRITE_DAC"},
      {WRITE_OWNER, L"WRITE_OWNER"},
  };
  std::wstring text;
  ACCESS_MASK rest = mask;
  for (const auto& [bits, name] : kRights) {
    if ((rest & bits) == bits) {
      text.append(text.empty() ? L"" : L", ").append(name);
      rest &= ~bits;
    }
  }
  wchar_t number[32] = {};
  if (rest != 0) {
    swprintf_s(number, L"0x%lX", rest);
    text.append(text.empty() ? L"" : L", ").append(number);
  }
  swprintf_s(number, L"%s(0x%08lX)", text.empty() ? L"" : L" ", mask);
  return text.append(number);
}

std::wstring DecodeAttributes(
    ULONG attributes
) {
  std::wstring text;
  for (const auto& [bit, name] : {std::pair<ULONG, const wchar_t*>{0x2, L"Inherit"}, {0x1, L"Protect"}, {0x4, L"Audit"}}) {
    if (attributes & bit) {
      text.append(text.empty() ? L"" : L", ").append(name);
    }
  }
  return text;
}

std::vector<Row> BuildRows(
    std::vector<::win32::KeyHandle>&& handles
) {
  const std::wstring sid = util::GetCurrentUserSidString();
  std::vector<Row> rows;
  rows.reserve(handles.size());
  for (::win32::KeyHandle& handle : handles) {
    Row row;
    wchar_t number[32] = {};
    row.text[kProcess] = std::move(handle.process);
    swprintf_s(number, L"%Iu", handle.process_id);
    row.text[kPid] = number;
    swprintf_s(number, L"0x%IX", handle.handle);
    row.text[kHandle] = number;
    if (handle.object != 0) {
      swprintf_s(number, L"0x%IX", handle.object);
      row.text[kObject] = number;
    }
    if (!handle.name.empty()) {
      row.text[kKey] = registry_path::Normalize(handle.name, sid);
    }
    row.text[kAccess] = DecodeAccess(handle.access);
    row.text[kAttributes] = DecodeAttributes(handle.attributes);
    row.text[kNative] = std::move(handle.name);
    row.handle = std::move(handle);
    rows.push_back(std::move(row));
  }
  return rows;
}

template <typename T>
int Compare(
    T left,
    T right
) {
  return left < right ? -1 : (right < left ? 1 : 0);
}

int CompareRows(
    const Row& left,
    const Row& right,
    int column
) {
  switch (column) {
  case kPid:
    return Compare(left.handle.process_id, right.handle.process_id);
  case kHandle:
    return Compare(left.handle.handle, right.handle.handle);
  case kObject:
    return Compare(left.handle.object, right.handle.object);
  case kAccess:
    return Compare(left.handle.access, right.handle.access);
  default:
    return _wcsicmp(left.text[column].c_str(), right.text[column].c_str());
  }
}

std::vector<const Row*> SelectedRows(
    const State* state
) {
  std::vector<const Row*> rows;
  for (int item = ListView_GetNextItem(state->list, -1, LVNI_SELECTED); item >= 0; item = ListView_GetNextItem(state->list, item, LVNI_SELECTED)) {
    if (static_cast<size_t>(item) < state->view.size()) {
      rows.push_back(&state->rows[state->view[static_cast<size_t>(item)]]);
    }
  }
  return rows;
}

void UpdateStatus(
    const State* state
) {
  std::wstring parts[3];
  if (state->scanning) {
    parts[0] = L"Scanning handles...";
  } else if (state->error != ERROR_SUCCESS) {
    parts[0] = L"Handles couldn't be enumerated. " + util::FormatWin32Error(state->error);
  } else {
    parts[0] = std::to_wstring(state->rows.size()) + L" handles loaded";
    parts[1] = std::to_wstring(state->view.size()) + L" shown";
    parts[2] = std::to_wstring(state->inaccessible) + (state->inaccessible == 1 ? L" process" : L" processes") + L" couldn't be opened";
  }
  for (int part = 0; part < 3; ++part) {
    SendMessageW(state->status, SB_SETTEXTW, part, reinterpret_cast<LPARAM>(parts[part].c_str()));
  }
}

void Layout(
    HWND dialog,
    const State* state
) {
  SendMessageW(state->status, WM_SIZE, 0, 0);
  const int part = MulDiv(170, static_cast<int>(win32::DpiForWindow(state->status)), 96);
  const int edges[3] = {part, part * 2, -1};
  SendMessageW(state->status, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(edges));
  RECT client = {};
  RECT bar = {};
  RECT list = {};
  GetClientRect(dialog, &client);
  GetWindowRect(state->status, &bar);
  GetWindowRect(state->list, &list);
  MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&bar), 2);
  MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&list), 2);
  SetWindowPos(state->list, nullptr, 0, list.top, client.right, bar.top - list.top, SWP_NOZORDER | SWP_NOACTIVATE);
  support::LayoutGridToggles(dialog);
}

void RebuildView(
    HWND dialog,
    State* state
) {
  const std::wstring filter = support::ReadText(dialog, IDC_KH_FILTER);
  state->view.clear();
  for (size_t index = 0; index < state->rows.size(); ++index) {
    const Row& row = state->rows[index];
    if (filter.empty() || std::any_of(row.text.begin(), row.text.end(), [&](const std::wstring& text) { return util::ContainsInsensitive(text, filter); })) {
      state->view.push_back(index);
    }
  }
  std::stable_sort(state->view.begin(), state->view.end(), [state](size_t left, size_t right) {
    const int order = CompareRows(state->rows[left], state->rows[right], state->sort_column);
    return state->ascending ? order < 0 : order > 0;
  });
  appearance::UpdateListViewSort(state->list, state->sort_column, state->ascending);
  ListView_SetItemState(state->list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_SetItemCountEx(state->list, static_cast<int>(state->view.size()), 0);
  InvalidateRect(state->list, nullptr, FALSE);
  UpdateStatus(state);
}

void Refresh(
    HWND dialog,
    State* state
) {
  state->scanning = true;
  UpdateStatus(state);
  state->session.Start([dialog](uint64_t generation, std::atomic_bool& cancel) {
    ::win32::KeyHandleSnapshot snapshot = ::win32::SnapshotKeyHandles(cancel);
    if (cancel.load()) {
      return;
    }
    auto payload = std::make_unique<Payload>();
    payload->generation = generation;
    payload->rows = BuildRows(std::move(snapshot.handles));
    payload->inaccessible = snapshot.inaccessible_processes;
    payload->error = snapshot.error;
    if (PostMessageW(dialog, kSnapshotReady, 0, reinterpret_cast<LPARAM>(payload.get()))) {
      payload.release();
    }
  });
}

void Copy(
    HWND dialog,
    const State* state,
    int column
) {
  const int first = column < 0 ? 0 : column;
  const int last = column < 0 ? kColumnCount : column + 1;
  std::wstring text;
  for (const Row* row : SelectedRows(state)) {
    text.append(text.empty() ? L"" : L"\r\n");
    for (int part = first; part < last; ++part) {
      text.append(part == first ? L"" : L"\t").append(row->text[part]);
    }
  }
  if (!text.empty()) {
    ui::CopyTextToClipboard(dialog, text);
  }
}

void OpenKey(
    const State* state,
    bool new_tab
) {
  const std::vector<const Row*> rows = SelectedRows(state);
  if (rows.size() == 1 && !rows.front()->text[kKey].empty() && state->navigate) {
    state->navigate(rows.front()->text[kKey], new_tab);
  }
}

void CloseHandles(
    HWND dialog,
    State* state
) {
  std::vector<::win32::KeyHandle> targets;
  std::wstring handles;
  for (const Row* row : SelectedRows(state)) {
    targets.push_back(row->handle);
    handles.append(handles.empty() ? L"" : L"\r\n").append(row->text[kProcess]).append(L" (").append(row->text[kPid]).append(L") ").append(row->text[kHandle]).append(L"  ").append(row->text[kKey]);
  }
  if (targets.empty()) {
    return;
  }
  const std::wstring message =
      L"Closing a handle owned by another process can make that process malfunction or crash.\n\n"
      L"Each handle is checked against a fresh snapshot first, but it can still be closed and reused between that check and the close.";
  if (ui::PromptKeyChoice(dialog, message, handles, targets.size() == 1 ? L"Close Handle" : L"Close Handles", L"Close", L"", L"Cancel", {70, 70, 70}) != IDYES) {
    return;
  }
  size_t closed = 0;
  const DWORD error = ::win32::CloseKeyHandles(targets, &closed);
  if (error != ERROR_SUCCESS) {
    ui::ShowError(dialog, std::to_wstring(closed) + L" of " + std::to_wstring(targets.size()) + L" handles were closed.\n" + util::FormatWin32Error(error));
  }
  Refresh(dialog, state);
}

void ShowRowMenu(
    HWND dialog,
    State* state,
    POINT screen
) {
  const std::vector<const Row*> rows = SelectedRows(state);
  if (screen.x == -1 && screen.y == -1) {
    RECT rect = {};
    const int focused = ListView_GetNextItem(state->list, -1, LVNI_FOCUSED | LVNI_SELECTED);
    if (focused < 0 || !ListView_GetItemRect(state->list, focused, &rect, LVIR_LABEL)) {
      GetClientRect(state->list, &rect);
    }
    screen = {rect.left, rect.bottom};
    ClientToScreen(state->list, &screen);
  }
  const UINT any = rows.empty() ? MF_GRAYED : 0;
  const UINT one = rows.size() == 1 && !rows.front()->text[kKey].empty() ? 0 : MF_GRAYED;
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING | one, kMenuOpen, L"Open Key\tEnter");
  AppendMenuW(menu, MF_STRING | one, kMenuOpenNewTab, L"Open Key in New Tab");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | any, kMenuCopyKey, L"Copy Key");
  AppendMenuW(menu, MF_STRING | any, kMenuCopyNative, L"Copy Native Name");
  AppendMenuW(menu, MF_STRING | any, kMenuCopyProcess, L"Copy Process");
  AppendMenuW(menu, MF_STRING | any, kMenuCopyPid, L"Copy PID");
  AppendMenuW(menu, MF_STRING | any, kMenuCopyHandle, L"Copy Handle");
  AppendMenuW(menu, MF_STRING | any, kMenuCopyAccess, L"Copy Access");
  AppendMenuW(menu, MF_STRING | any, kMenuCopyRows, L"Copy Rows\tCtrl+C");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuRefresh, L"Refresh\tF5");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | any, kMenuClose, L"Close Handle...\tDel");
  SetMenuDefaultItem(menu, kMenuOpen, FALSE);
  const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0, dialog, nullptr);
  DestroyMenu(menu);
  static constexpr int kCopyColumns[] = {kKey, kNative, kProcess, kPid, kHandle, kAccess, -1};
  switch (chosen) {
  case kMenuOpen:
  case kMenuOpenNewTab:
    OpenKey(state, chosen == kMenuOpenNewTab);
    break;
  case kMenuRefresh:
    Refresh(dialog, state);
    break;
  case kMenuClose:
    CloseHandles(dialog, state);
    break;
  default:
    if (chosen >= kMenuCopyKey && chosen <= kMenuCopyRows) {
      Copy(dialog, state, kCopyColumns[chosen - kMenuCopyKey]);
    }
    break;
  }
}

INT_PTR HandleNotify(
    HWND dialog,
    State* state,
    NMHDR* header
) {
  switch (header->idFrom == IDC_KH_LIST ? header->code : 0) {
  case LVN_GETDISPINFOW:
    {
      auto* info = reinterpret_cast<NMLVDISPINFOW*>(header);
      if ((info->item.mask & LVIF_TEXT) && info->item.iItem >= 0 && static_cast<size_t>(info->item.iItem) < state->view.size() && info->item.iSubItem < kColumnCount) {
        info->item.pszText = const_cast<wchar_t*>(state->rows[state->view[static_cast<size_t>(info->item.iItem)]].text[info->item.iSubItem].c_str());
      }
      return TRUE;
    }
  case LVN_COLUMNCLICK:
    {
      const int column = reinterpret_cast<NMLISTVIEW*>(header)->iSubItem;
      state->ascending = column == state->sort_column ? !state->ascending : true;
      state->sort_column = column;
      RebuildView(dialog, state);
      return TRUE;
    }
  case LVN_ITEMACTIVATE:
    OpenKey(state, false);
    return TRUE;
  case LVN_KEYDOWN:
    {
      const WORD key = reinterpret_cast<NMLVKEYDOWN*>(header)->wVKey;
      if (key == VK_F5) {
        Refresh(dialog, state);
      } else if (key == VK_DELETE) {
        CloseHandles(dialog, state);
      } else if (key == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        Copy(dialog, state, -1);
      }
      return TRUE;
    }
  default:
    break;
  }
  INT_PTR result = FALSE;
  return support::HandleListViewNotify(dialog, header, &result) ? result : FALSE;
}

INT_PTR CALLBACK DialogProc(
    HWND dialog,
    UINT message,
    WPARAM wparam,
    LPARAM lparam
) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(dialog, DWLP_USER));
  switch (message) {
  case WM_INITDIALOG:
    {
      state = reinterpret_cast<State*>(lparam);
      SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
      state->list = GetDlgItem(dialog, IDC_KH_LIST);
      state->status = GetDlgItem(dialog, IDC_KH_STATUS);
      support::Initialize(dialog, &state->font, {IDC_KH_FILTER});
      SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON))));
      SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED)));
      SendDlgItemMessageW(dialog, IDC_KH_FILTER, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter"));
      support::SetupListView(state->list, 0, {{L"Process", 150}, {L"PID", 60}, {L"Handle", 80}, {L"Object Address", 140}, {L"Key", 400}, {L"Access", 300}, {L"Attributes", 90}, {L"Native Name", 400}});
      using namespace appearance;
      state->resizer.Attach(dialog, {
                                        {IDC_KH_FILTER, kAnchorLeft | kAnchorTop | kAnchorRight},
                                    });
      appearance::ApplyDialogTheme(dialog);
      Theme::Current().ApplyToStatusBar(state->status);
      Layout(dialog, state);
      RebuildView(dialog, state);
      Refresh(dialog, state);
      return TRUE;
    }
  case kSnapshotReady:
    {
      std::unique_ptr<Payload> payload(reinterpret_cast<Payload*>(lparam));
      if (state && state->session.IsCurrent(payload->generation)) {
        state->rows = std::move(payload->rows);
        state->inaccessible = payload->inaccessible;
        state->error = payload->error;
        state->scanning = false;
        RebuildView(dialog, state);
      }
      return TRUE;
    }
  case WM_SIZE:
    if (state) {
      state->resizer.Apply(dialog);
      Layout(dialog, state);
    }
    return TRUE;
  case WM_GETMINMAXINFO:
    if (state) {
      state->resizer.ClampMinSize(reinterpret_cast<MINMAXINFO*>(lparam));
    }
    return TRUE;
  case WM_NOTIFY:
    return state ? HandleNotify(dialog, state, reinterpret_cast<NMHDR*>(lparam)) : FALSE;
  case WM_CONTEXTMENU:
    if (state && reinterpret_cast<HWND>(wparam) == state->list) {
      ShowRowMenu(dialog, state, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      return TRUE;
    }
    return FALSE;
  case WM_COMMAND:
    if (!state) {
      return FALSE;
    }
    switch (LOWORD(wparam)) {
    case IDC_KH_FILTER:
      if (HIWORD(wparam) == EN_CHANGE) {
        RebuildView(dialog, state);
      }
      return TRUE;
    case IDOK:
      OpenKey(state, false);
      return TRUE;
    case IDCANCEL:
      DestroyWindow(dialog);
      return TRUE;
    default:
      return support::HandleGridToggle(dialog, LOWORD(wparam));
    }
  case WM_DESTROY:
    if (state) {
      state->session.CancelAndJoin();
      MSG pending = {};
      while (PeekMessageW(&pending, dialog, kSnapshotReady, kSnapshotReady, PM_REMOVE)) {
        delete reinterpret_cast<Payload*>(pending.lParam);
      }
      support::ReleaseDialogLists(dialog);
      support::ReleaseFont(&state->font);
    }
    return TRUE;
  case WM_NCDESTROY:
    delete state;
    return TRUE;
  default:
    break;
  }
  INT_PTR themed = 0;
  return support::HandleThemeMessage(dialog, message, wparam, lparam, &themed) ? themed : FALSE;
}

} // namespace

HWND ShowKeyHandlesWindow(
    HWND owner,
    KeyHandlesNavigate navigate
) {
  auto* state = new State;
  state->navigate = std::move(navigate);
  HWND dialog = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_KEY_HANDLES), nullptr, DialogProc, reinterpret_cast<LPARAM>(state));
  if (!dialog) {
    return nullptr;
  }
  appearance::CenterWindow(dialog, owner);
  ShowWindow(dialog, SW_SHOW);
  return dialog;
}

} // namespace regkit
