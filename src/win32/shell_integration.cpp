// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "win32/text_transform.h"
#include "win32/shell_integration.h"

#include "win32/registry_native.h"

#include <shlobj.h>

#include <cwctype>

namespace regkit::win32 {
namespace {

// use regfile ProgID instead of SystemFileAssociations for W7 support
constexpr wchar_t kEditMenuKey[] = L"Software\\Classes\\regfile\\shell\\RegKit.Edit";
constexpr wchar_t kEditMenuCommandKey[] = L"Software\\Classes\\regfile\\shell\\RegKit.Edit\\command";
constexpr wchar_t kEditMenuLabel[] = L"Edit with RegKit";
constexpr wchar_t kRegEditImageOptionsKey[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\regedit.exe";

bool Missing(
    LONG result
) {
  return result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
}

std::wstring EditMenuCommand(
    const std::wstring& exe_path
) {
  return L"\"" + exe_path + L"\" --edit-reg \"%1\"";
}

bool RegistryStringEquals(
    const wchar_t* subkey,
    const wchar_t* value_name,
    const std::wstring& expected
) {
  std::wstring value;
  return util::ReadRegistryString(HKEY_CURRENT_USER, subkey, value_name, &value) == ERROR_SUCCESS &&
         util::EqualsInsensitive(value, expected);
}

LONG DeleteEditMenu() {
  LONG result = RegDeleteTreeW(HKEY_CURRENT_USER, kEditMenuKey);
  if (result == ERROR_SUCCESS) {
    result = RegDeleteKeyW(HKEY_CURRENT_USER, kEditMenuKey);
  }
  return Missing(result) ? ERROR_SUCCESS : result;
}

bool IsEditMenuCommandOwned(
    const std::wstring& exe_path
) {
  return !exe_path.empty() && RegistryStringEquals(kEditMenuCommandKey, nullptr, EditMenuCommand(exe_path));
}

bool OwnsRegEditDebugger(
    const std::wstring& debugger,
    const std::wstring& exe_path
) {
  // compare only the debugger executable and allow arguments after it
  const wchar_t* start = debugger.c_str();
  while (*start && iswspace(*start)) {
    ++start;
  }
  const bool quoted = *start == L'\"';
  start += quoted ? 1 : 0;
  const wchar_t* end = start;
  while (*end && (quoted ? *end != L'\"' : !iswspace(*end))) {
    ++end;
  }
  return end != start && exe_path.size() == static_cast<size_t>(end - start) &&
         util::StartsWithInsensitive(start, exe_path);
}

LONG ReadRegEditDebugger(
    std::wstring* debugger
) {
  return util::ReadRegistryString(HKEY_LOCAL_MACHINE, kRegEditImageOptionsKey, L"Debugger", debugger);
}

LONG WriteRegEditDebugger(
    const std::wstring& exe_path
) {
  return util::WriteRegistryString(HKEY_LOCAL_MACHINE, kRegEditImageOptionsKey, L"Debugger", L"\"" + exe_path + L"\"");
}

LONG DeleteOwnedRegEditDebugger(
    const std::wstring& exe_path
) {
  std::wstring debugger;
  LONG result = ReadRegEditDebugger(&debugger);
  if (result != ERROR_SUCCESS) {
    return Missing(result) ? ERROR_SUCCESS : result;
  }
  // leave debugger entries owned by other programs unchanged
  if (!OwnsRegEditDebugger(debugger, exe_path)) {
    return ERROR_SUCCESS;
  }
  result = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kRegEditImageOptionsKey, L"Debugger");
  if (result != ERROR_SUCCESS && !Missing(result)) {
    return result;
  }
  RegDeleteKeyW(HKEY_LOCAL_MACHINE, kRegEditImageOptionsKey);
  return ERROR_SUCCESS;
}

} // namespace

bool IsRegFileEditMenuRegistered(
    const std::wstring& exe_path
) {
  std::wstring label;
  return IsEditMenuCommandOwned(exe_path) &&
         util::ReadRegistryString(HKEY_CURRENT_USER, kEditMenuKey, nullptr, &label) == ERROR_SUCCESS && label == kEditMenuLabel &&
         RegistryStringEquals(kEditMenuKey, L"Icon", exe_path + L",0");
}

LONG SetRegFileEditMenu(
    const std::wstring& exe_path,
    bool enable,
    LONG* cleanup_error
) {
  if (cleanup_error) {
    *cleanup_error = ERROR_SUCCESS;
  }
  if (exe_path.empty()) {
    return ERROR_INVALID_PARAMETER;
  }
  LONG result = ERROR_SUCCESS;
  if (enable) {
    result = util::WriteRegistryString(HKEY_CURRENT_USER, kEditMenuKey, nullptr, kEditMenuLabel);
    if (result == ERROR_SUCCESS) {
      result = util::WriteRegistryString(HKEY_CURRENT_USER, kEditMenuKey, L"Icon", exe_path + L",0");
    }
    if (result == ERROR_SUCCESS) {
      result = util::WriteRegistryString(HKEY_CURRENT_USER, kEditMenuCommandKey, nullptr, EditMenuCommand(exe_path));
    }
    if (result != ERROR_SUCCESS) {
      // remove partial registration when any write fails
      const LONG cleanup = DeleteEditMenu();
      if (cleanup_error) {
        *cleanup_error = cleanup;
      }
    }
  } else {
    result = DeleteEditMenu();
  }
  // refresh explorer after changing file association
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return result;
}

LONG RemoveRegFileEditMenuIfOwned(
    const std::wstring& exe_path
) {
  // uninstall only the command that still points to this executable
  return IsEditMenuCommandOwned(exe_path) ? SetRegFileEditMenu(exe_path, false) : ERROR_SUCCESS;
}

bool IsRegEditReplacementRegistered(
    const std::wstring& exe_path
) {
  std::wstring debugger;
  return !exe_path.empty() && ReadRegEditDebugger(&debugger) == ERROR_SUCCESS && OwnsRegEditDebugger(debugger, exe_path);
}

LONG SetRegEditReplacement(
    const std::wstring& exe_path,
    bool enable,
    bool* conflict,
    bool overwrite_existing
) {
  if (conflict) {
    *conflict = false;
  }
  if (exe_path.empty()) {
    return ERROR_INVALID_PARAMETER;
  }
  if (!enable) {
    return DeleteOwnedRegEditDebugger(exe_path);
  }
  std::wstring debugger;
  const LONG result = overwrite_existing ? ERROR_FILE_NOT_FOUND : ReadRegEditDebugger(&debugger);
  if (Missing(result)) {
    return WriteRegEditDebugger(exe_path);
  }
  if (result == ERROR_SUCCESS && OwnsRegEditDebugger(debugger, exe_path)) {
    return ERROR_SUCCESS;
  }
  if (result != ERROR_SUCCESS && result != ERROR_UNSUPPORTED_TYPE && result != ERROR_INVALID_DATA) {
    return result;
  }
  if (conflict) {
    *conflict = true;
  }
  return ERROR_ALREADY_EXISTS;
}

} // namespace regkit::win32
