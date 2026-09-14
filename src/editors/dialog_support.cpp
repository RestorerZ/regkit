// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "editors/dialog_support.h"
#include "win32/text_transform.h"
#include "win32/window_metrics.h"

#include "appearance/dialog_layout.h"
#include "appearance/icon_loader.h"
#include "appearance/list_header.h"
#include "appearance/theme.h"
#include "appearance/default_font.h"
#include "appearance/feedback.h"

#include "resource.h"

#include <algorithm>
#include <string>

#include <commctrl.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <windowsx.h>

namespace regkit::editors::dialog_support {

namespace {

constexpr UINT_PTR kSingleLineSubclassId = 2;
constexpr UINT_PTR kListViewSubclassId = 3;
constexpr UINT_PTR kListHeaderSubclassId = 4;
constexpr int kTooltipMaxWidth = 600;
constexpr int kMenuSizeToFit = 1;
constexpr int kMenuSizeAll = 2;
constexpr int kMenuColumnBase = 16;

constexpr int kGridToggleId = 4200;
constexpr int kGridGlyphSize = 16;
constexpr int kGridButtonWidth = 22;

bool grid_lines = true;
COLORREF grid_color = CLR_INVALID;
std::wstring grid_icon_path;
GridLinesSink grid_sink = nullptr;
void* grid_sink_context = nullptr;

struct GridList {
  HWND dialog = nullptr;
  HWND list = nullptr;
  HWND toolbar = nullptr;
};

std::vector<GridList> grid_lists;

void PlaceGridToggle(
    const GridList& entry
) {
  const HWND header = entry.list ? ListView_GetHeader(entry.list) : nullptr;
  if (!entry.toolbar || !header) {
    return;
  }
  RECT header_rect = {};
  RECT client = {};
  if (!GetWindowRect(header, &header_rect) || !GetClientRect(header, &client)) {
    return;
  }
  MapWindowPoints(nullptr, entry.dialog, reinterpret_cast<POINT*>(&header_rect), 2);
  const int width = std::min<int>(client.right - client.left, MulDiv(kGridButtonWidth, static_cast<int>(win32::DpiForWindow(header)), 96));
  const int height = header_rect.bottom - header_rect.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  SendMessageW(entry.toolbar, TB_SETBUTTONSIZE, 0, MAKELPARAM(width, height));
  SetWindowPos(entry.toolbar, HWND_TOP, header_rect.right - width, header_rect.top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

HWND CreateGridToggle(
    HWND dialog,
    HWND list
) {
  const UINT dpi = win32::DpiForWindow(list);
  const HWND toolbar = CreateWindowExW(
      0,
      TOOLBARCLASSNAMEW,
      L"Grid lines",
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
          CCS_NODIVIDER | CCS_NOPARENTALIGN | CCS_NORESIZE,
      0,
      0,
      0,
      0,
      dialog,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGridToggleId)),
      GetModuleHandleW(nullptr),
      nullptr
  );
  if (!toolbar) {
    return nullptr;
  }
  SendMessageW(toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
  SendMessageW(toolbar, TB_SETMAXTEXTROWS, 0, 0);
  SendMessageW(toolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);

  const int size = util::ScaleForDpi(kGridGlyphSize, dpi);
  HICON icon = grid_icon_path.empty() ? nullptr : util::LoadIconFromFile(grid_icon_path, kGridGlyphSize, dpi);
  if (!icon) {
    icon = util::LoadIconResource(Theme::UseDarkMode() ? IDI_ICON_LIGHT_GRID : IDI_ICON_DARK_GRID, kGridGlyphSize, dpi);
  }
  if (HIMAGELIST images = ImageList_Create(size, size, ILC_COLOR32, 1, 1)) {
    ImageList_SetBkColor(images, CLR_NONE);
    util::ImageListAddOrBlank(images, icon, size);
    auto* previous = reinterpret_cast<HIMAGELIST>(SendMessageW(toolbar, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images)));
    if (previous) {
      ImageList_Destroy(previous);
    }
  }
  if (icon) {
    DestroyIcon(icon);
  }

  const LRESULT label = SendMessageW(toolbar, TB_ADDSTRINGW, 0, reinterpret_cast<LPARAM>(L"Grid lines"));
  TBBUTTON button = {};
  button.iBitmap = 0;
  button.idCommand = kGridToggleId;
  button.fsState = TBSTATE_ENABLED;
  button.fsStyle = BTNS_CHECK;
  button.iString = static_cast<INT_PTR>(label);
  SendMessageW(toolbar, TB_ADDBUTTONSW, 1, reinterpret_cast<LPARAM>(&button));
  Theme::Current().ApplyToToolbar(toolbar);
  SendMessageW(toolbar, TB_CHECKBUTTON, kGridToggleId, MAKELPARAM(grid_lines ? TRUE : FALSE, 0));
  return toolbar;
}

LRESULT CALLBACK SingleLineProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR
) {
  if (message == WM_CHAR && (wparam == L'\r' || wparam == L'\n')) {
    return 0;
  }
  if (message == WM_PASTE) {
    std::wstring text;
    if (OpenClipboard(window)) {
      HANDLE handle = GetClipboardData(CF_UNICODETEXT);
      const wchar_t* data = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
      if (data) {
        text = data;
        GlobalUnlock(handle);
      }
      CloseClipboard();
    }
    if (text.find_first_of(L"\r\n") == std::wstring::npos) {
      return DefSubclassProc(window, message, wparam, lparam);
    }
    for (wchar_t& character : text) {
      if (character == L'\r' || character == L'\n') {
        character = L' ';
      }
    }
    SendMessageW(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
    return 0;
  }
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, SingleLineProc, kSingleLineSubclassId);
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK ListHeaderProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR
) {
  if (message == WM_ERASEBKGND) {
    return 1;
  }
  if (message == WM_PAINT) {
    appearance::PaintListHeader(window, nullptr);
    return 0;
  }
  if (message == WM_CONTEXTMENU) {
    POINT screen = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    if (screen.x == -1 && screen.y == -1) {
      RECT rect = {};
      GetWindowRect(window, &rect);
      screen.x = rect.left + 20;
      screen.y = rect.bottom;
    }
    ShowColumnMenu(GetParent(window), screen);
    return 0;
  }
  if (message == WM_THEMECHANGED) {
    appearance::ReleaseListHeaderTheme(window);
    InvalidateRect(window, nullptr, TRUE);
  }
  if (message == WM_NCDESTROY) {
    appearance::ReleaseListHeaderTheme(window);
    RemoveWindowSubclass(window, ListHeaderProc, kListHeaderSubclassId);
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK ListViewProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR
) {
  if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
    SendMessageW(window, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
  }
  if (message == WM_UPDATEUISTATE) {
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    SendMessageW(window, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    return result;
  }
  if (message == WM_THEMECHANGED) {
    InvalidateRect(window, nullptr, TRUE);
  }
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, ListViewProc, kListViewSubclassId);
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

bool SizeGripRect(
    HWND dialog,
    RECT* rect
) {
  if (!rect || (GetWindowLongPtrW(dialog, GWL_STYLE) & WS_THICKFRAME) == 0) {
    return false;
  }
  RECT client = {};
  if (!GetClientRect(dialog, &client)) {
    return false;
  }
  SIZE grip = {};
  HTHEME theme = OpenThemeData(dialog, VSCLASS_STATUS);
  if (theme) {
    if (FAILED(GetThemePartSize(theme, nullptr, SP_GRIPPER, 0, nullptr, TS_TRUE, &grip))) {
      grip = {};
    }
    CloseThemeData(theme);
  }
  if (grip.cx <= 0 || grip.cy <= 0) {
    grip.cx = grip.cy = GetSystemMetrics(SM_CXVSCROLL);
  }
  rect->left = client.right - grip.cx;
  rect->top = client.bottom - grip.cy;
  rect->right = client.right;
  rect->bottom = client.bottom;
  return true;
}

void DrawSizeGrip(
    HWND dialog,
    HDC hdc
) {
  RECT grip = {};
  if (!hdc || !SizeGripRect(dialog, &grip)) {
    return;
  }
  HTHEME theme = OpenThemeData(dialog, VSCLASS_STATUS);
  if (theme) {
    DrawThemeBackground(theme, hdc, SP_GRIPPER, 0, &grip, nullptr);
    CloseThemeData(theme);
    return;
  }
  DrawFrameControl(hdc, &grip, DFC_SCROLL, DFCS_SCROLLSIZEGRIP);
}

void ThinBorder(
    HWND dialog,
    int id
) {
  const HWND edit = GetDlgItem(dialog, id);
  if (!edit) {
    return;
  }
  SetWindowLongPtrW(edit, GWL_EXSTYLE, GetWindowLongPtrW(edit, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
  SetWindowLongPtrW(edit, GWL_STYLE, GetWindowLongPtrW(edit, GWL_STYLE) | WS_BORDER);
  SetWindowPos(edit, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

} // namespace

void Initialize(
    HWND dialog,
    HFONT* owned_font,
    std::initializer_list<int> bordered_edits
) {
  for (const int id : bordered_edits) {
    ThinBorder(dialog, id);
  }
  HFONT font = ui::DefaultUIFont(win32::DpiForWindow(dialog));
  if (owned_font) {
    *owned_font = font;
  }
  if (font) {
    SendMessageW(dialog, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    EnumChildWindows(
        dialog,
        [](HWND child, LPARAM value) {
          SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(value), TRUE);
          wchar_t class_name[16] = {};
          GetClassNameW(child, class_name, _countof(class_name));
          const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
          if (_wcsicmp(class_name, L"Edit") == 0 &&
              (style & ES_MULTILINE) && !(style & ES_READONLY)) {
            SetWindowSubclass(child, SingleLineProc, kSingleLineSubclassId, 0);
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(font)
    );
  }
  Theme::Current().ApplyToWindow(dialog);
  Theme::Current().ApplyToChildren(dialog);
  regkit::appearance::CenterWindow(dialog, GetWindow(dialog, GW_OWNER));
}

void AllowNewlines(
    HWND dialog,
    int control_id
) {
  if (HWND edit = GetDlgItem(dialog, control_id)) {
    RemoveWindowSubclass(edit, SingleLineProc, kSingleLineSubclassId);
  }
}

void ReleaseFont(
    HFONT* font
) {
  if (font && *font) {
    DeleteObject(*font);
    *font = nullptr;
  }
}

bool HandleThemeMessage(
    HWND dialog,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    INT_PTR* result
) {
  if (!result) {
    return false;
  }
  if (message == WM_SETTINGCHANGE) {
    grid_color = CLR_INVALID;
    if (Theme::UpdateFromSystem()) {
      Theme::Current().ApplyToWindow(dialog);
      Theme::Current().ApplyToChildren(dialog);
      InvalidateRect(dialog, nullptr, TRUE);
    }
    *result = TRUE;
    return true;
  }
  if (message == WM_ERASEBKGND) {
    RECT rect = {};
    GetClientRect(dialog, &rect);
    FillRect(reinterpret_cast<HDC>(wparam), &rect, Theme::Current().BackgroundBrush());
    DrawSizeGrip(dialog, reinterpret_cast<HDC>(wparam));
    *result = TRUE;
    return true;
  }
  if (message == WM_NCHITTEST) {
    RECT grip = {};
    if (SizeGripRect(dialog, &grip)) {
      MapWindowPoints(dialog, nullptr, reinterpret_cast<POINT*>(&grip), 2);
      const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (PtInRect(&grip, pt)) {
        SetWindowLongPtrW(dialog, DWLP_MSGRESULT, HTBOTTOMRIGHT);
        *result = TRUE;
        return true;
      }
    }
  }
  int color_type = 0;
  switch (message) {
  case WM_CTLCOLORDLG:
    color_type = CTLCOLOR_DLG;
    break;
  case WM_CTLCOLORSTATIC:
    color_type = CTLCOLOR_STATIC;
    break;
  case WM_CTLCOLOREDIT:
    color_type = CTLCOLOR_EDIT;
    break;
  case WM_CTLCOLORLISTBOX:
    color_type = CTLCOLOR_LISTBOX;
    break;
  case WM_CTLCOLORBTN:
    color_type = CTLCOLOR_BTN;
    break;
  default:
    return false;
  }
  *result = reinterpret_cast<INT_PTR>(Theme::Current().ControlColor(
      reinterpret_cast<HDC>(wparam),
      reinterpret_cast<HWND>(lparam),
      color_type
  ));
  return true;
}

std::wstring ReadText(
    HWND dialog,
    int control_id
) {
  return util::WindowText(GetDlgItem(dialog, control_id));
}

void SetupListView(
    HWND list,
    DWORD extra_styles,
    std::initializer_list<ListColumn> columns
) {
  if (!list) {
    return;
  }
  const DWORD mask = LVS_EX_INFOTIP | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                     LVS_EX_DOUBLEBUFFER | LVS_EX_BORDERSELECT | LVS_EX_TRACKSELECT |
                     LVS_EX_ONECLICKACTIVATE | LVS_EX_TWOCLICKACTIVATE | LVS_EX_UNDERLINEHOT;
  const DWORD style = LVS_EX_LABELTIP | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | extra_styles;
  ListView_SetExtendedListViewStyleEx(list, mask, style);

  const UINT dpi = win32::DpiForWindow(list);
  int index = 0;
  for (const ListColumn& column : columns) {
    LVCOLUMNW item = {};
    item.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    item.pszText = const_cast<wchar_t*>(column.title);
    item.cx = MulDiv(column.width, static_cast<int>(dpi), 96);
    item.iSubItem = index;
    ListView_InsertColumn(list, index, &item);
    ++index;
  }

  SendMessageW(list, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
  if (HWND tooltip = ListView_GetToolTips(list)) {
    SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, MulDiv(kTooltipMaxWidth, static_cast<int>(dpi), 96));
  }
  if (!GetWindowSubclass(list, ListViewProc, kListViewSubclassId, nullptr)) {
    SetWindowSubclass(list, ListViewProc, kListViewSubclassId, 0);
  }
  GridList entry;
  entry.dialog = GetParent(list);
  entry.list = list;
  entry.toolbar = CreateGridToggle(entry.dialog, list);
  grid_lists.push_back(entry);
  PlaceGridToggle(entry);
  RefreshListViewTheme(list);
}

void SetGridLines(
    bool enabled
) {
  grid_lines = enabled;
  grid_lists.erase(
      std::remove_if(grid_lists.begin(), grid_lists.end(), [](const GridList& entry) { return !IsWindow(entry.list); }),
      grid_lists.end()
  );
  for (const GridList& entry : grid_lists) {
    if (entry.toolbar) {
      SendMessageW(entry.toolbar, TB_CHECKBUTTON, kGridToggleId, MAKELPARAM(enabled ? TRUE : FALSE, 0));
    }
    InvalidateRect(entry.list, nullptr, TRUE);
  }
}

void SetGridIcon(
    const std::wstring& path
) {
  grid_icon_path = path;
}

void SetGridLinesSink(
    GridLinesSink sink,
    void* context
) {
  grid_sink = sink;
  grid_sink_context = context;
}

void LayoutGridToggles(
    HWND dialog
) {
  for (const GridList& entry : grid_lists) {
    if (entry.dialog == dialog) {
      PlaceGridToggle(entry);
    }
  }
}

bool HandleGridToggle(
    HWND,
    int command_id
) {
  if (command_id != kGridToggleId) {
    return false;
  }
  const bool enabled = !grid_lines;
  if (grid_sink) {
    grid_sink(grid_sink_context, enabled);
  } else {
    SetGridLines(enabled);
  }
  return true;
}

void ReleaseDialogLists(
    HWND dialog
) {
  grid_lists.erase(
      std::remove_if(grid_lists.begin(), grid_lists.end(), [dialog](const GridList& entry) { return entry.dialog == dialog; }),
      grid_lists.end()
  );
}

bool GridLines() {
  return grid_lines;
}

void RefreshListViewTheme(
    HWND list
) {
  if (!list) {
    return;
  }
  Theme::Current().ApplyToListView(list);
  HWND header = ListView_GetHeader(list);
  if (header && !GetWindowSubclass(header, ListHeaderProc, kListHeaderSubclassId, nullptr)) {
    SetWindowSubclass(header, ListHeaderProc, kListHeaderSubclassId, 0);
  }
  if (header) {
    appearance::ReleaseListHeaderTheme(header);
    InvalidateRect(header, nullptr, TRUE);
  }
  InvalidateRect(list, nullptr, TRUE);
}

bool HandleListViewNotify(
    HWND dialog,
    const NMHDR* header,
    INT_PTR* result
) {
  if (!header || !result || header->code != NM_CUSTOMDRAW) {
    return false;
  }
  for (const GridList& entry : grid_lists) {
    if (entry.toolbar == header->hwndFrom) {
      SetWindowLongPtrW(
          dialog,
          DWLP_MSGRESULT,
          appearance::PaintGridToolbar(
              entry.toolbar,
              reinterpret_cast<NMTBCUSTOMDRAW*>(const_cast<NMHDR*>(header)),
              appearance::ListSurfaceBrush(entry.list)
          )
      );
      *result = TRUE;
      return true;
    }
  }
  wchar_t class_name[32] = {};
  GetClassNameW(header->hwndFrom, class_name, static_cast<int>(_countof(class_name)));
  if (_wcsicmp(class_name, WC_LISTVIEWW) != 0) {
    return false;
  }
  const HWND list = header->hwndFrom;
  auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMHDR*>(header));
  LRESULT drawn = ui::HandleThemedListViewCustomDraw(list, draw);
  if (grid_lines) {
    if (grid_color == CLR_INVALID) {
      grid_color = appearance::HeaderDividerColor(ListView_GetHeader(list));
    }
    switch (draw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      drawn |= CDRF_NOTIFYPOSTPAINT;
      break;
    case CDDS_ITEMPREPAINT:
      drawn |= CDRF_NOTIFYPOSTPAINT;
      break;
    case CDDS_ITEMPOSTPAINT:
      {
        RECT row = {};
        if (ListView_GetItemRect(list, static_cast<int>(draw->nmcd.dwItemSpec), &row, LVIR_BOUNDS)) {
          appearance::PaintListGrid(list, draw->nmcd.hdc, row, row.bottom - 1, row.bottom - row.top, grid_color);
        }
        break;
      }
    case CDDS_POSTPAINT:
      appearance::PaintListGridTail(list, draw->nmcd.hdc, grid_color);
      break;
    default:
      break;
    }
  }
  SetWindowLongPtrW(dialog, DWLP_MSGRESULT, drawn);
  *result = TRUE;
  return true;
}

bool ShowColumnMenu(
    HWND list,
    POINT screen
) {
  HWND header = list ? ListView_GetHeader(list) : nullptr;
  const int count = header ? Header_GetItemCount(header) : 0;
  if (count <= 0) {
    return false;
  }
  POINT client = screen;
  ScreenToClient(header, &client);
  HDHITTESTINFO hit = {};
  hit.pt = client;
  const int column = static_cast<int>(SendMessageW(header, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit)));

  HMENU menu = CreatePopupMenu();
  if (!menu) {
    return false;
  }
  AppendMenuW(menu, MF_STRING | (column >= 0 ? 0 : MF_GRAYED), kMenuSizeToFit, L"Size column to fit");
  AppendMenuW(menu, MF_STRING, kMenuSizeAll, L"Size all columns to fit");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  std::vector<int> widths(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    wchar_t title[128] = {};
    HDITEMW item = {};
    item.mask = HDI_TEXT;
    item.pszText = title;
    item.cchTextMax = static_cast<int>(_countof(title));
    Header_GetItem(header, i, &item);
    widths[static_cast<size_t>(i)] = ListView_GetColumnWidth(list, i);
    const bool visible = widths[static_cast<size_t>(i)] > 0;
    AppendMenuW(menu, MF_STRING | (visible ? MF_CHECKED : MF_UNCHECKED) | (i == 0 ? MF_GRAYED : 0), static_cast<UINT_PTR>(kMenuColumnBase + i), title);
  }

  const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0, list, nullptr);
  DestroyMenu(menu);
  if (chosen == kMenuSizeToFit && column >= 0) {
    ListView_SetColumnWidth(list, column, LVSCW_AUTOSIZE_USEHEADER);
    return true;
  }
  if (chosen == kMenuSizeAll) {
    for (int i = 0; i < count; ++i) {
      if (widths[static_cast<size_t>(i)] > 0) {
        ListView_SetColumnWidth(list, i, LVSCW_AUTOSIZE_USEHEADER);
      }
    }
    return true;
  }
  if (chosen >= kMenuColumnBase) {
    const int index = chosen - kMenuColumnBase;
    ListView_SetColumnWidth(list, index, widths[static_cast<size_t>(index)] > 0 ? 0 : LVSCW_AUTOSIZE_USEHEADER);
  }
  return true;
}

std::wstring ListViewText(
    HWND list,
    int item,
    int subitem
) {
  wchar_t buffer[1024] = {};
  ListView_GetItemText(list, item, subitem, buffer, static_cast<int>(_countof(buffer)));
  return buffer;
}

std::wstring ToDisplayText(
    const std::wstring& text
) {
  std::wstring out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) {
      out.push_back(L'\r');
    }
    out.push_back(text[i]);
  }
  return out;
}

std::wstring FromDisplayText(
    const std::wstring& text
) {
  std::wstring out;
  out.reserve(text.size());
  for (const wchar_t c : text) {
    if (c != L'\r') {
      out.push_back(c);
    }
  }
  return out;
}

std::wstring SingleLine(
    const std::wstring& text
) {
  std::wstring out;
  out.reserve(text.size());
  bool space = false;
  for (const wchar_t c : text) {
    if (c == L'\r' || c == L'\n' || c == L'\t') {
      space = !out.empty();
      continue;
    }
    if (space) {
      out.push_back(L' ');
      space = false;
    }
    out.push_back(c);
  }
  return out;
}

bool Matches(
    const std::wstring& text,
    const std::wstring& filter
) {
  if (filter.empty()) {
    return true;
  }
  if (filter.size() > text.size()) {
    return false;
  }
  const size_t last = text.size() - filter.size();
  for (size_t start = 0; start <= last; ++start) {
    if (_wcsnicmp(text.c_str() + start, filter.c_str(), filter.size()) == 0) {
      return true;
    }
  }
  return false;
}

void FitDroppedWidth(
    HWND combo
) {
  if (!combo) {
    return;
  }
  const HDC dc = GetDC(combo);
  if (!dc) {
    return;
  }
  auto* font = reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0));
  HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
  const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
  int widest = 0;
  std::wstring text;
  for (int i = 0; i < count; ++i) {
    const int length = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0));
    if (length <= 0) {
      continue;
    }
    text.resize(static_cast<size_t>(length));
    SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data()));
    SIZE size = {};
    if (GetTextExtentPoint32W(dc, text.c_str(), length, &size) && size.cx > widest) {
      widest = size.cx;
    }
  }
  if (previous) {
    SelectObject(dc, previous);
  }
  ReleaseDC(combo, dc);
  RECT rect = {};
  GetWindowRect(combo, &rect);
  const int minimum = rect.right - rect.left;
  const int padding = GetSystemMetrics(SM_CXVSCROLL) + 8;
  SendMessageW(combo, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(std::max(minimum, widest + padding)), 0);
}

} // namespace regkit::editors::dialog_support