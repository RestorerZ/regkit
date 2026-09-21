// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "appearance/list_view_support.h"

#include "appearance/dialog_layout.h"
#include "appearance/icon_loader.h"
#include "appearance/list_header.h"
#include "appearance/theme.h"
#include "win32/window_metrics.h"

#include "resource.h"

#include <algorithm>
#include <vector>

#include <uxtheme.h>
#include <windowsx.h>

namespace regkit::appearance {

namespace {

constexpr UINT_PTR kListViewSubclassId = 91;
constexpr UINT_PTR kListHeaderSubclassId = 92;
constexpr int kGridGlyphSize = 16;
constexpr int kGridButtonWidth = 22;
constexpr int kMenuSizeToFit = 1;
constexpr int kMenuSizeAll = 2;
constexpr int kMenuColumnBase = 16;

struct ListRegistration {
  HWND owner = nullptr;
  HWND list = nullptr;
  HWND toolbar = nullptr;
  HIMAGELIST images = nullptr;
  int grid_command = 0;
  ListHeaderMenuCallback header_menu = nullptr;
  void* context = nullptr;
  std::vector<int> widths;
};

struct SortContext {
  ListItemCompareCallback compare = nullptr;
  void* context = nullptr;
  int column = 0;
  bool ascending = true;
};

std::vector<ListRegistration> registrations;
bool grid_enabled = false;
ListGridChangedCallback grid_changed = nullptr;
void* grid_changed_context = nullptr;

ListRegistration* FindByList(
    HWND list
) {
  const auto found = std::find_if(
      registrations.begin(),
      registrations.end(),
      [list](const ListRegistration& entry) { return entry.list == list; }
  );
  return found == registrations.end() ? nullptr : &*found;
}

ListRegistration* FindByToolbar(
    HWND toolbar
) {
  const auto found = std::find_if(
      registrations.begin(),
      registrations.end(),
      [toolbar](const ListRegistration& entry) { return entry.toolbar == toolbar; }
  );
  return found == registrations.end() ? nullptr : &*found;
}

int ColumnSubItem(
    HWND list,
    int display
) {
  LVCOLUMNW column = {};
  column.mask = LVCF_SUBITEM;
  return list && display >= 0 && ListView_GetColumn(list, display, &column)
             ? column.iSubItem
             : -1;
}

void CaptureWidths(
    ListRegistration* entry
) {
  if (!entry || !entry->list) {
    return;
  }
  HWND header = ListView_GetHeader(entry->list);
  const int count = header ? Header_GetItemCount(header) : 0;
  for (int display = 0; display < count; ++display) {
    const int subitem = ColumnSubItem(entry->list, display);
    if (subitem < 0) {
      continue;
    }
    if (entry->widths.size() <= static_cast<size_t>(subitem)) {
      entry->widths.resize(static_cast<size_t>(subitem) + 1, 100);
    }
    const int width = ListView_GetColumnWidth(entry->list, display);
    if (width > 0) {
      entry->widths[static_cast<size_t>(subitem)] = width;
    }
  }
}

void ApplyGridIcon(
    ListRegistration* entry
) {
  if (!entry || !entry->toolbar) {
    return;
  }
  const UINT dpi = win32::DpiForWindow(entry->toolbar);
  const int size = util::ScaleForDpi(kGridGlyphSize, dpi);
  HICON icon = util::LoadIconResource(Theme::UseDarkMode() ? IDI_ICON_LIGHT_GRID : IDI_ICON_DARK_GRID, kGridGlyphSize, dpi);
  HIMAGELIST images = ImageList_Create(size, size, ILC_COLOR32, 1, 1);
  if (images) {
    ImageList_SetBkColor(images, CLR_NONE);
    util::ImageListAddOrBlank(images, icon, size);
    SendMessageW(entry->toolbar, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images));
    if (entry->images) {
      ImageList_Destroy(entry->images);
    }
    entry->images = images;
  }
  if (icon) {
    DestroyIcon(icon);
  }
}

void ApplyGridToolbarTheme(
    HWND toolbar
) {
  if (!toolbar) {
    return;
  }
  Theme::Current().ApplyToToolbar(toolbar);
  SetDarkWindowTheme(reinterpret_cast<HWND>(SendMessageW(toolbar, TB_GETTOOLTIPS, 0, 0)), Theme::UseDarkMode());
}

void LayoutRegistration(
    ListRegistration* entry
) {
  HWND header = entry && entry->list ? ListView_GetHeader(entry->list) : nullptr;
  const bool shown = entry && entry->list &&
                     (GetWindowLongPtrW(entry->list, GWL_STYLE) & WS_VISIBLE) != 0;
  if (!entry || !entry->toolbar || !header || !shown) {
    if (entry && entry->toolbar) {
      ShowWindow(entry->toolbar, SW_HIDE);
    }
    return;
  }
  RECT header_rect = {};
  RECT client = {};
  if (!GetWindowRect(header, &header_rect) || !GetClientRect(header, &client)) {
    return;
  }
  MapWindowPoints(nullptr, entry->owner, reinterpret_cast<POINT*>(&header_rect), 2);
  const int width = std::min<int>(
      client.right - client.left,
      util::ScaleForDpi(kGridButtonWidth, win32::DpiForWindow(header))
  );
  const int height = header_rect.bottom - header_rect.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  SendMessageW(entry->toolbar, TB_SETBUTTONSIZE, 0, MAKELPARAM(width, height));
  SetWindowPos(
      entry->toolbar,
      HWND_TOP,
      header_rect.right - width,
      header_rect.top,
      width,
      height,
      SWP_NOACTIVATE | SWP_SHOWWINDOW
  );
}

HWND CreateGridToolbar(
    HWND owner,
    int command
) {
  HWND toolbar = CreateWindowExW(
      0,
      TOOLBARCLASSNAMEW,
      L"Grid lines",
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
          CCS_NODIVIDER | CCS_NOPARENTALIGN | CCS_NORESIZE,
      0,
      0,
      0,
      0,
      owner,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(command)),
      GetModuleHandleW(nullptr),
      nullptr
  );
  if (!toolbar) {
    return nullptr;
  }
  SendMessageW(toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
  SendMessageW(toolbar, TB_SETMAXTEXTROWS, 0, 0);
  SendMessageW(toolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);
  const LRESULT label = SendMessageW(
      toolbar,
      TB_ADDSTRINGW,
      0,
      reinterpret_cast<LPARAM>(L"Grid lines")
  );
  TBBUTTON button = {};
  button.iBitmap = 0;
  button.idCommand = command;
  button.fsState = TBSTATE_ENABLED;
  button.fsStyle = BTNS_CHECK;
  button.iString = static_cast<INT_PTR>(label);
  SendMessageW(toolbar, TB_ADDBUTTONSW, 1, reinterpret_cast<LPARAM>(&button));
  ApplyGridToolbarTheme(toolbar);
  SendMessageW(
      toolbar,
      TB_CHECKBUTTON,
      command,
      MAKELPARAM(grid_enabled ? TRUE : FALSE, 0)
  );
  return toolbar;
}

LRESULT CALLBACK HeaderProc(
    HWND header,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR
) {
  HWND list = GetParent(header);
  if (message == WM_ERASEBKGND) {
    return 1;
  }
  if (message == WM_PAINT) {
    PaintListHeader(header, nullptr);
    return 0;
  }
  if (message == WM_SIZE) {
    LayoutRegistration(FindByList(list));
  }
  if (message == WM_CONTEXTMENU) {
    POINT screen = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    if (screen.x == -1 && screen.y == -1) {
      RECT rect = {};
      GetWindowRect(header, &rect);
      screen.x = rect.left + 12;
      screen.y = rect.bottom - 4;
    }
    ListRegistration* entry = FindByList(list);
    if (entry && entry->header_menu) {
      entry->header_menu(list, screen, entry->context);
    } else {
      ShowListColumnMenu(list, screen);
    }
    return 0;
  }
  if (message == WM_THEMECHANGED) {
    ReleaseListHeaderTheme(header);
    InvalidateRect(header, nullptr, TRUE);
  }
  if (message == WM_NCDESTROY) {
    ReleaseListHeaderTheme(header);
    RemoveWindowSubclass(header, HeaderProc, kListHeaderSubclassId);
  }
  return DefSubclassProc(header, message, wparam, lparam);
}

LRESULT CALLBACK ListProc(
    HWND list,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR
) {
  if (grid_enabled &&
      (message == WM_HSCROLL || message == WM_MOUSEWHEEL ||
       message == WM_MOUSEHWHEEL || message == LVM_SCROLL ||
       message == LVM_ENSUREVISIBLE || message == WM_KEYDOWN)) {
    const int before = GetScrollPos(list, SB_HORZ);
    const LRESULT result = DefSubclassProc(list, message, wparam, lparam);
    if (before != GetScrollPos(list, SB_HORZ)) {
      InvalidateRect(list, nullptr, TRUE);
    }
    return result;
  }
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(list, ListProc, kListViewSubclassId);
  }
  return DefSubclassProc(list, message, wparam, lparam);
}

int CALLBACK CompareItems(
    LPARAM left,
    LPARAM right,
    LPARAM parameter
) {
  auto* sort = reinterpret_cast<SortContext*>(parameter);
  if (!sort || !sort->compare) {
    return 0;
  }
  const int result = sort->compare(left, right, sort->column, sort->context);
  return sort->ascending ? result : -result;
}

} // namespace

void ConfigureListView(
    HWND list,
    DWORD extra_styles
) {
  if (!list) {
    return;
  }
  const DWORD mask = LVS_EX_INFOTIP | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES |
                     LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                     LVS_EX_BORDERSELECT | LVS_EX_TRACKSELECT |
                     LVS_EX_ONECLICKACTIVATE | LVS_EX_TWOCLICKACTIVATE |
                     LVS_EX_UNDERLINEHOT;
  ListView_SetExtendedListViewStyleEx(
      list,
      mask,
      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | extra_styles
  );
  AttachThemedBorder(list);
  SendMessageW(list, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
}

void RegisterListView(
    HWND owner,
    HWND list,
    int grid_command,
    ListHeaderMenuCallback header_menu,
    void* context
) {
  if (!owner || !list || !ListView_GetHeader(list)) {
    return;
  }
  ListRegistration* entry = FindByList(list);
  if (!entry) {
    registrations.push_back({});
    entry = &registrations.back();
    entry->owner = owner;
    entry->list = list;
  }
  entry->grid_command = grid_command;
  entry->header_menu = header_menu;
  entry->context = context;
  CaptureWidths(entry);
  if (!entry->toolbar || !IsWindow(entry->toolbar)) {
    entry->toolbar = CreateGridToolbar(owner, grid_command);
    ApplyGridIcon(entry);
  }
  HWND header = ListView_GetHeader(list);
  EnsureSubclass(header, HeaderProc, kListHeaderSubclassId);
  EnsureSubclass(list, ListProc, kListViewSubclassId);
  LayoutRegistration(entry);
}

void ReleaseListViews(
    HWND owner
) {
  registrations.erase(
      std::remove_if(
          registrations.begin(),
          registrations.end(),
          [owner](ListRegistration& entry) {
            if (entry.owner != owner) {
              return false;
            }
            if (entry.images) {
              if (IsWindow(entry.toolbar)) {
                SendMessageW(entry.toolbar, TB_SETIMAGELIST, 0, 0);
              }
              ImageList_Destroy(entry.images);
            }
            return true;
          }
      ),
      registrations.end()
  );
}

void LayoutListViews(
    HWND owner
) {
  for (ListRegistration& entry : registrations) {
    if (entry.owner == owner) {
      LayoutRegistration(&entry);
    }
  }
}

void RefreshListView(
    HWND list
) {
  if (!list) {
    return;
  }
  Theme::Current().ApplyToListView(list);
  ListRegistration* entry = FindByList(list);
  if (entry) {
    ApplyGridIcon(entry);
    ApplyGridToolbarTheme(entry->toolbar);
  }
  HWND header = ListView_GetHeader(list);
  if (header) {
    ReleaseListHeaderTheme(header);
    InvalidateRect(header, nullptr, TRUE);
  }
  InvalidateRect(list, nullptr, TRUE);
}

void SetListGridEnabled(
    bool enabled
) {
  grid_enabled = enabled;
  for (const ListRegistration& entry : registrations) {
    if (entry.toolbar) {
      SendMessageW(
          entry.toolbar,
          TB_CHECKBUTTON,
          entry.grid_command,
          MAKELPARAM(enabled ? TRUE : FALSE, 0)
      );
    }
    InvalidateRect(entry.list, nullptr, TRUE);
  }
}

void ReloadListGridIcons() {
  for (ListRegistration& entry : registrations) {
    ApplyGridIcon(&entry);
  }
}

void SetListGridChangedCallback(
    ListGridChangedCallback callback,
    void* context
) {
  grid_changed = callback;
  grid_changed_context = context;
}

bool HandleListViewCommand(
    HWND owner,
    int command
) {
  const auto found = std::find_if(
      registrations.begin(),
      registrations.end(),
      [owner, command](const ListRegistration& entry) {
        return entry.owner == owner && entry.grid_command == command;
      }
  );
  if (found == registrations.end()) {
    return false;
  }
  const bool enabled = !grid_enabled;
  if (grid_changed) {
    grid_changed(grid_changed_context, enabled);
  } else {
    SetListGridEnabled(enabled);
  }
  return true;
}

bool HandleListViewNotify(
    HWND owner,
    const NMHDR* header,
    LRESULT* result
) {
  if (!header || !result || header->code != NM_CUSTOMDRAW) {
    return false;
  }
  ListRegistration* entry = FindByToolbar(header->hwndFrom);
  if (!entry || entry->owner != owner) {
    return false;
  }
  *result = PaintGridToolbar(
      entry->toolbar,
      reinterpret_cast<NMTBCUSTOMDRAW*>(const_cast<NMHDR*>(header)),
      ListSurfaceBrush(entry->list)
  );
  return true;
}

LRESULT HandleListGridCustomDraw(
    HWND list,
    NMLVCUSTOMDRAW* draw,
    LRESULT result
) {
  if (!list || !draw || !grid_enabled) {
    return result;
  }
  const COLORREF grid_color = Theme::Current().BorderColor();
  switch (draw->nmcd.dwDrawStage) {
  case CDDS_PREPAINT:
    return result | CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
  case CDDS_ITEMPREPAINT:
    return result | CDRF_NOTIFYPOSTPAINT;
  case CDDS_ITEMPOSTPAINT:
    {
      RECT row = {};
      if (ListView_GetItemRect(
              list,
              static_cast<int>(draw->nmcd.dwItemSpec),
              &row,
              LVIR_BOUNDS
          )) {
        PaintListGrid(
            list,
            draw->nmcd.hdc,
            row,
            row.bottom - 1,
            row.bottom - row.top,
            grid_color
        );
      }
      return result;
    }
  case CDDS_POSTPAINT:
    PaintListGridTail(list, draw->nmcd.hdc, grid_color);
    return result;
  default:
    return result;
  }
}

bool ShowListColumnMenu(
    HWND list,
    POINT screen
) {
  HWND header = list ? ListView_GetHeader(list) : nullptr;
  const int count = header ? Header_GetItemCount(header) : 0;
  if (count <= 0) {
    return false;
  }
  ListRegistration* entry = FindByList(list);
  CaptureWidths(entry);
  POINT client = screen;
  ScreenToClient(header, &client);
  HDHITTESTINFO hit = {};
  hit.pt = client;
  const int column = static_cast<int>(
      SendMessageW(header, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit))
  );
  HMENU menu = CreatePopupMenu();
  if (!menu) {
    return false;
  }
  AppendMenuW(
      menu,
      MF_STRING | (column >= 0 ? 0 : MF_GRAYED),
      kMenuSizeToFit,
      L"Size column to fit"
  );
  AppendMenuW(menu, MF_STRING, kMenuSizeAll, L"Size all columns to fit");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  std::vector<int> widths(static_cast<size_t>(count));
  for (int display = 0; display < count; ++display) {
    wchar_t title[128] = {};
    HDITEMW item = {};
    item.mask = HDI_TEXT;
    item.pszText = title;
    item.cchTextMax = static_cast<int>(_countof(title));
    Header_GetItem(header, display, &item);
    widths[static_cast<size_t>(display)] = ListView_GetColumnWidth(list, display);
    AppendMenuW(
        menu,
        MF_STRING | (widths[static_cast<size_t>(display)] > 0 ? MF_CHECKED : 0),
        static_cast<UINT_PTR>(kMenuColumnBase + display),
        title
    );
  }
  const int chosen = TrackPopupMenu(
      menu,
      TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
      screen.x,
      screen.y,
      0,
      list,
      nullptr
  );
  DestroyMenu(menu);
  if (chosen == kMenuSizeToFit && column >= 0) {
    ListView_SetColumnWidth(list, column, LVSCW_AUTOSIZE_USEHEADER);
    CaptureWidths(entry);
    return true;
  }
  if (chosen == kMenuSizeAll) {
    for (int display = 0; display < count; ++display) {
      if (widths[static_cast<size_t>(display)] > 0) {
        ListView_SetColumnWidth(list, display, LVSCW_AUTOSIZE_USEHEADER);
      }
    }
    CaptureWidths(entry);
    return true;
  }
  if (chosen >= kMenuColumnBase && chosen < kMenuColumnBase + count) {
    const int display = chosen - kMenuColumnBase;
    const int subitem = ColumnSubItem(list, display);
    if (widths[static_cast<size_t>(display)] > 0) {
      ListView_SetColumnWidth(list, display, 0);
    } else {
      int width = 100;
      if (entry && subitem >= 0 &&
          static_cast<size_t>(subitem) < entry->widths.size()) {
        width = entry->widths[static_cast<size_t>(subitem)];
      }
      ListView_SetColumnWidth(list, display, std::max(width, 1));
    }
    return true;
  }
  return true;
}

void UpdateListViewSort(
    HWND list,
    int column,
    bool ascending
) {
  HWND header = list ? ListView_GetHeader(list) : nullptr;
  const int count = header ? Header_GetItemCount(header) : 0;
  bool changed = false;
  for (int display = 0; display < count; ++display) {
    HDITEMW item = {};
    item.mask = HDI_FORMAT;
    if (!Header_GetItem(header, display, &item)) {
      continue;
    }
    const int current = item.fmt & (HDF_SORTUP | HDF_SORTDOWN);
    int wanted = 0;
    if (column >= 0 && ColumnSubItem(list, display) == column) {
      wanted = ascending ? HDF_SORTUP : HDF_SORTDOWN;
    }
    if (current == wanted) {
      continue;
    }
    item.fmt = (item.fmt & ~(HDF_SORTUP | HDF_SORTDOWN)) | wanted;
    Header_SetItem(header, display, &item);
    changed = true;
  }
  if (changed) {
    InvalidateRect(header, nullptr, TRUE);
  }
}

void UpdateListSortState(
    int column,
    bool toggle,
    int* sort_column,
    bool* ascending
) {
  if (!sort_column || !ascending || column < 0) {
    return;
  }
  if (toggle && *sort_column == column) {
    *ascending = !*ascending;
  } else {
    *sort_column = column;
    if (toggle) {
      *ascending = true;
    }
  }
}

void SortListViewItems(
    HWND list,
    int column,
    bool toggle,
    int* sort_column,
    bool* ascending,
    ListItemCompareCallback compare,
    void* context
) {
  if (!list || !sort_column || !ascending || !compare || column < 0) {
    return;
  }
  LPARAM selected = 0;
  LPARAM focused = 0;
  bool has_selected = false;
  bool has_focused = false;
  const int selected_row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
  const int focused_row = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
  has_selected = ListViewItemData(list, selected_row, &selected) >= 0;
  has_focused = ListViewItemData(list, focused_row, &focused) >= 0;
  UpdateListSortState(column, toggle, sort_column, ascending);
  SortContext sort = {compare, context, *sort_column, *ascending};
  ListView_SortItems(list, CompareItems, reinterpret_cast<LPARAM>(&sort));
  if (has_selected) {
    const int row = FindListViewItemByData(list, selected);
    if (row >= 0) {
      ListView_SetItemState(list, row, LVIS_SELECTED, LVIS_SELECTED);
    }
  }
  if (has_focused) {
    const int row = FindListViewItemByData(list, focused);
    if (row >= 0) {
      ListView_SetItemState(list, row, LVIS_FOCUSED, LVIS_FOCUSED);
    }
  }
  UpdateListViewSort(list, *sort_column, *ascending);
  InvalidateRect(list, nullptr, TRUE);
}

int ListViewItemData(
    HWND list,
    int row,
    LPARAM* data
) {
  if (!list || row < 0 || !data) {
    return -1;
  }
  LVITEMW item = {};
  item.mask = LVIF_PARAM;
  item.iItem = row;
  if (!ListView_GetItem(list, &item)) {
    return -1;
  }
  *data = item.lParam;
  return row;
}

int FindListViewItemByData(
    HWND list,
    LPARAM data
) {
  if (!list) {
    return -1;
  }
  LVFINDINFOW find = {};
  find.flags = LVFI_PARAM;
  find.lParam = data;
  return ListView_FindItem(list, -1, &find);
}

} // namespace regkit::appearance
