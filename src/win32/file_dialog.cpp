// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "win32/file_dialog.h"

#include "win32/handle_owner.h"
#include "win32/system_error.h"

#include <objsel.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <vector>

namespace regkit::win32 {

namespace {

template <typename T>
class ComPtr {
public:
  ComPtr() = default;
  ~ComPtr() {
    if (ptr_) {
      ptr_->Release();
    }
  }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  T** Receive() { return &ptr_; }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }

private:
  T* ptr_ = nullptr;
};

class CoTaskString {
public:
  ~CoTaskString() {
    if (text_) {
      CoTaskMemFree(text_);
    }
  }
  PWSTR* Receive() { return &text_; }
  PCWSTR Get() const { return text_; }

private:
  PWSTR text_ = nullptr;
};

std::vector<COMDLG_FILTERSPEC> ParseFilter(const wchar_t* filter) {
  std::vector<COMDLG_FILTERSPEC> specs;
  if (!filter) {
    return specs;
  }
  const wchar_t* cursor = filter;
  while (*cursor) {
    const wchar_t* name = cursor;
    cursor += wcslen(cursor) + 1;
    if (!*cursor) {
      break;
    }
    const wchar_t* spec = cursor;
    cursor += wcslen(cursor) + 1;
    specs.push_back({name, spec});
  }
  return specs;
}

HRESULT ShowDialog(HWND owner, REFCLSID clsid, const wchar_t* filter,
                   FILEOPENDIALOGOPTIONS extra_options,
                   const wchar_t* default_extension,
                   const wchar_t* suggested_name, std::wstring* path) {
  if (!path) {
    return E_POINTER;
  }
  ComPtr<IFileDialog> dialog;
  HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.Receive()));
  if (FAILED(hr)) {
    return hr;
  }

  FILEOPENDIALOGOPTIONS options = 0;
  hr = dialog->GetOptions(&options);
  if (FAILED(hr)) {
    return hr;
  }
  hr = dialog->SetOptions(options | FOS_FORCEFILESYSTEM | extra_options);
  if (FAILED(hr)) {
    return hr;
  }

  const std::vector<COMDLG_FILTERSPEC> specs = ParseFilter(filter);
  if (!specs.empty()) {
    hr = dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    if (SUCCEEDED(hr)) {
      dialog->SetFileTypeIndex(1);
    }
  }
  if (default_extension && *default_extension) {
    dialog->SetDefaultExtension(default_extension);
  }
  if (suggested_name && *suggested_name) {
    dialog->SetFileName(suggested_name);
  }

  hr = dialog->Show(owner);
  if (FAILED(hr)) {
    return hr;
  }

  ComPtr<IShellItem> item;
  hr = dialog->GetResult(item.Receive());
  if (FAILED(hr)) {
    return hr;
  }
  CoTaskString text;
  hr = item->GetDisplayName(SIGDN_FILESYSPATH, text.Receive());
  if (FAILED(hr)) {
    return hr;
  }
  *path = text.Get() ? text.Get() : L"";
  return S_OK;
}

} // namespace

HRESULT ChooseFileToOpen(HWND owner, const wchar_t* filter, std::wstring* path) {
  return ShowDialog(owner, CLSID_FileOpenDialog, filter, 0, nullptr, nullptr, path);
}

HRESULT ChooseFileToSave(HWND owner, const wchar_t* filter,
                         const wchar_t* default_extension,
                         const wchar_t* suggested_name, std::wstring* path) {
  return ShowDialog(owner, CLSID_FileSaveDialog, filter, 0, default_extension,
                    suggested_name, path);
}

HRESULT ChooseFolder(HWND owner, std::wstring* path) {
  return ShowDialog(owner, CLSID_FileOpenDialog, nullptr, FOS_PICKFOLDERS,
                    nullptr, nullptr, path);
}

HRESULT ChooseComputer(HWND owner, std::wstring* name) {
  if (!name) {
    return E_POINTER;
  }
  ComPtr<IDsObjectPicker> picker;
  HRESULT hr = CoCreateInstance(CLSID_DsObjectPicker, nullptr,
                                CLSCTX_INPROC_SERVER, IID_IDsObjectPicker,
                                reinterpret_cast<void**>(picker.Receive()));
  if (FAILED(hr)) {
    return hr;
  }

  DSOP_SCOPE_INIT_INFO scope = {};
  scope.cbSize = sizeof(scope);
  scope.flType = DSOP_SCOPE_TYPE_UPLEVEL_JOINED_DOMAIN |
                 DSOP_SCOPE_TYPE_DOWNLEVEL_JOINED_DOMAIN |
                 DSOP_SCOPE_TYPE_ENTERPRISE_DOMAIN |
                 DSOP_SCOPE_TYPE_GLOBAL_CATALOG |
                 DSOP_SCOPE_TYPE_EXTERNAL_UPLEVEL_DOMAIN |
                 DSOP_SCOPE_TYPE_EXTERNAL_DOWNLEVEL_DOMAIN |
                 DSOP_SCOPE_TYPE_WORKGROUP |
                 DSOP_SCOPE_TYPE_USER_ENTERED_UPLEVEL_SCOPE |
                 DSOP_SCOPE_TYPE_USER_ENTERED_DOWNLEVEL_SCOPE;
  scope.flScope = DSOP_SCOPE_FLAG_STARTING_SCOPE |
                  DSOP_SCOPE_FLAG_DEFAULT_FILTER_COMPUTERS;
  scope.FilterFlags.Uplevel.flBothModes = DSOP_FILTER_COMPUTERS;
  scope.FilterFlags.flDownlevel = DSOP_DOWNLEVEL_FILTER_COMPUTERS;

  DSOP_INIT_INFO init = {};
  init.cbSize = sizeof(init);
  init.cDsScopeInfos = 1;
  init.aDsScopeInfos = &scope;
  hr = picker->Initialize(&init);
  if (FAILED(hr)) {
    return hr;
  }

  ComPtr<IDataObject> selection;
  hr = picker->InvokeDialog(owner, selection.Receive());
  if (FAILED(hr)) {
    return hr;
  }
  if (hr == S_FALSE || !selection) {
    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
  }

  FORMATETC format = {};
  format.cfFormat = static_cast<CLIPFORMAT>(
      RegisterClipboardFormatW(CFSTR_DSOP_DS_SELECTION_LIST));
  format.dwAspect = DVASPECT_CONTENT;
  format.lindex = -1;
  format.tymed = TYMED_HGLOBAL;
  STGMEDIUM medium = {};
  hr = selection->GetData(&format, &medium);
  if (FAILED(hr)) {
    return hr;
  }
  if (auto* list =
          static_cast<PDS_SELECTION_LIST>(GlobalLock(medium.hGlobal))) {
    if (list->cItems > 0 && list->aDsSelection[0].pwzName) {
      *name = list->aDsSelection[0].pwzName;
    }
    GlobalUnlock(medium.hGlobal);
  }
  ReleaseStgMedium(&medium);
  if (!name->empty() && name->back() == L'$') {
    name->pop_back();
  }
  return name->empty() ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK;
}

bool DialogCancelled(HRESULT hr) {
  return hr == HRESULT_FROM_WIN32(ERROR_CANCELLED);
}

HRESULT ShellOpen(HWND owner, const wchar_t* target) {
  if (!target || !*target) {
    return E_INVALIDARG;
  }
  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  info.hwnd = owner;
  info.lpVerb = L"open";
  info.lpFile = target;
  info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&info)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  return S_OK;
}

HRESULT RevealInExplorer(const std::wstring& path) {
  if (path.empty()) {
    return E_INVALIDARG;
  }
  PIDLIST_ABSOLUTE item = nullptr;
  HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr);
  if (FAILED(hr)) {
    return hr;
  }
  hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
  CoTaskMemFree(item);
  return hr;
}

std::wstring FormatDialogError(HRESULT hr) {
  if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
    return util::FormatWin32Error(static_cast<DWORD>(HRESULT_CODE(hr)));
  }
  wchar_t code[32] = {};
  swprintf_s(code, L"0x%08X", static_cast<unsigned>(hr));
  return std::wstring(L"The file dialog failed (") + code + L").";
}

} // namespace regkit::win32
