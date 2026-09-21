// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "appearance/icon_loader.h"

#include <commctrl.h>

namespace util {

namespace {

using LoadIconWithScaleDownFn = HRESULT(WINAPI*)(HINSTANCE, PCWSTR, int, int, HICON*);

LoadIconWithScaleDownFn ScaleDownLoader() {
  static LoadIconWithScaleDownFn fn = [] {
    HMODULE comctl = GetModuleHandleW(L"comctl32.dll");
    return comctl ? reinterpret_cast<LoadIconWithScaleDownFn>(GetProcAddress(comctl, "LoadIconWithScaleDown")) : nullptr;
  }();
  return fn;
}

HICON LoadScaledDown(
    HINSTANCE instance,
    PCWSTR name,
    int size
) {
  LoadIconWithScaleDownFn fn = ScaleDownLoader();
  HICON icon = nullptr;
  if (fn && SUCCEEDED(fn(instance, name, size, size, &icon))) {
    return icon;
  }
  return nullptr;
}

UINT ResolveDpi(
    UINT dpi
) {
  if (dpi != 0) {
    return dpi;
  }
  static UINT(WINAPI * get_system_dpi)() = [] {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 ? reinterpret_cast<UINT(WINAPI*)()>(GetProcAddress(user32, "GetDpiForSystem")) : nullptr;
  }();
  return get_system_dpi ? get_system_dpi() : 96;
}

} // namespace

int ScaleForDpi(
    int size,
    UINT dpi
) {
  if (size <= 0) {
    return size;
  }
  dpi = ResolveDpi(dpi);
  if (dpi <= 96) {
    return size;
  }
  int scaled = MulDiv(size, static_cast<int>(dpi), 96);
  return scaled > 0 ? scaled : size;
}

HICON LoadIconResource(
    int resource_id,
    int size,
    UINT dpi
) {
  if (resource_id == 0 || size <= 0) {
    return nullptr;
  }
  HINSTANCE instance = GetModuleHandleW(nullptr);
  int scaled = ScaleForDpi(size, dpi);
  HICON icon = LoadScaledDown(instance, MAKEINTRESOURCEW(resource_id), scaled);
  if (!icon) {
    icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(resource_id), IMAGE_ICON, scaled, scaled, LR_DEFAULTCOLOR));
  }
  return icon;
}

HICON LoadIconFromFile(
    const std::wstring& path,
    int size,
    UINT dpi
) {
  if (path.empty() || size <= 0) {
    return nullptr;
  }
  int scaled = ScaleForDpi(size, dpi);
  HICON icon = LoadScaledDown(nullptr, path.c_str(), scaled);
  if (!icon) {
    icon = static_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON, scaled, scaled, LR_LOADFROMFILE | LR_DEFAULTCOLOR));
  }
  return icon;
}

void ImageListAddOrBlank(
    HIMAGELIST list,
    HICON icon,
    int size
) {
  if (!list) {
    return;
  }
  if (icon) {
    ImageList_AddIcon(list, icon);
    return;
  }
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = size;
  info.bmiHeader.biHeight = -size;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  if (HBITMAP blank = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0)) {
    ImageList_Add(list, blank, nullptr);
    DeleteObject(blank);
  }
}

} // namespace util
