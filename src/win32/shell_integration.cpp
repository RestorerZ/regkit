// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "win32/shell_integration.h"

#include "win32/registry_native.h"

#include <shlobj.h>

#include <cwctype>

namespace regkit::win32 {
namespace {

constexpr wchar_t kEditMenuKey[] =
    L"Software\\Classes\\regfile\\shell\\RegKit.Edit";
constexpr wchar_t kEditMenuCommandKey[] =
    L"Software\\Classes\\regfile\\shell\\RegKit.Edit\\command";
constexpr wchar_t kRegeditImageOptionsKey[] =
    L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution "
    L"Options\\regedit.exe";

std::wstring EditMenuCommand(
    const std::wstring& exe_path
) {
  return L"\"" + exe_path + L"\" --edit-reg \"%1\"";
}

std::wstring EditMenuIcon(
    const std::wstring& exe_path
) {
  return exe_path + L",0";
}

LONG DeleteEditMenu() {
  LONG result = RegDeleteTreeW(HKEY_CURRENT_USER, kEditMenuKey);
  if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) {
    return ERROR_SUCCESS;
  }
  if (result != ERROR_SUCCESS) {
    return result;
  }
  result = RegDeleteKeyW(HKEY_CURRENT_USER, kEditMenuKey);
  return result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND
             ? ERROR_SUCCESS
             : result;
}

bool IsEditMenuCommandOwned(
    const std::wstring& exe_path
) {
  std::wstring command;
  return !exe_path.empty() &&
         util::ReadRegistryString(
             HKEY_CURRENT_USER,
             kEditMenuCommandKey,
             nullptr,
             &command
         ) &&
         _wcsicmp(command.c_str(), EditMenuCommand(exe_path).c_str()) == 0;
}

std::wstring CommandExecutable(
    const std::wstring& command
) {
  const wchar_t* start = command.c_str();
  while (*start && iswspace(*start)) {
    ++start;
  }
  if (*start == L'\"') {
    ++start;
    const wchar_t* end = wcschr(start, L'\"');
    return end ? std::wstring(start, static_cast<size_t>(end - start))
               : std::wstring(start);
  }
  const wchar_t* end = start;
  while (*end && !iswspace(*end)) {
    ++end;
  }
  return std::wstring(start, static_cast<size_t>(end - start));
}

LONG ReadRegeditDebugger(
    std::wstring* debugger,
    bool* value_exists = nullptr
) {
  if (!debugger) {
    return ERROR_INVALID_PARAMETER;
  }
  debugger->clear();
  if (value_exists) {
    *value_exists = false;
  }
  util::UniqueHKey key;
  LONG result = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      kRegeditImageOptionsKey,
      0,
      KEY_QUERY_VALUE,
      key.put()
  );
  if (result != ERROR_SUCCESS) {
    return result;
  }
  DWORD type = 0;
  DWORD size = 0;
  result = RegQueryValueExW(
      key.get(),
      L"Debugger",
      nullptr,
      &type,
      nullptr,
      &size
  );
  if (result != ERROR_SUCCESS) {
    return result;
  }
  if (value_exists) {
    *value_exists = true;
  }
  if ((type != REG_SZ && type != REG_EXPAND_SZ) ||
      size < sizeof(wchar_t) || size % sizeof(wchar_t) != 0) {
    return ERROR_INVALID_DATA;
  }
  std::wstring value(size / sizeof(wchar_t), L'\0');
  result = RegQueryValueExW(
      key.get(),
      L"Debugger",
      nullptr,
      &type,
      reinterpret_cast<BYTE*>(value.data()),
      &size
  );
  if (result != ERROR_SUCCESS) {
    return result;
  }
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  *debugger = std::move(value);
  return ERROR_SUCCESS;
}

bool OwnsRegeditDebugger(
    const std::wstring& debugger,
    const std::wstring& exe_path
) {
  const std::wstring executable = CommandExecutable(debugger);
  return !executable.empty() &&
         _wcsicmp(executable.c_str(), exe_path.c_str()) == 0;
}

LONG WriteRegeditDebugger(
    const std::wstring& exe_path
) {
  util::UniqueHKey key;
  LONG result = RegCreateKeyExW(
      HKEY_LOCAL_MACHINE,
      kRegeditImageOptionsKey,
      0,
      nullptr,
      REG_OPTION_NON_VOLATILE,
      KEY_SET_VALUE,
      nullptr,
      key.put(),
      nullptr
  );
  if (result != ERROR_SUCCESS) {
    return result;
  }
  return util::WriteRegistryString(key.get(), L"Debugger", L"\"" + exe_path + L"\"");
}

LONG DeleteOwnedRegeditDebugger(
    const std::wstring& exe_path
) {
  std::wstring debugger;
  LONG result = ReadRegeditDebugger(&debugger);
  if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) {
    return ERROR_SUCCESS;
  }
  if (result != ERROR_SUCCESS) {
    return result;
  }
  if (!OwnsRegeditDebugger(debugger, exe_path)) {
    return ERROR_SUCCESS;
  }
  util::UniqueHKey key;
  result = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      kRegeditImageOptionsKey,
      0,
      KEY_SET_VALUE,
      key.put()
  );
  if (result != ERROR_SUCCESS) {
    return result;
  }
  result = RegDeleteValueW(key.get(), L"Debugger");
  key.reset();
  if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
    return result;
  }
  RegDeleteKeyW(HKEY_LOCAL_MACHINE, kRegeditImageOptionsKey);
  return ERROR_SUCCESS;
}

} // namespace

bool IsRegFileEditMenuRegistered(
    const std::wstring& exe_path
) {
  std::wstring label;
  std::wstring icon;
  return IsEditMenuCommandOwned(exe_path) &&
         util::ReadRegistryString(
             HKEY_CURRENT_USER,
             kEditMenuKey,
             nullptr,
             &label
         ) &&
         util::ReadRegistryString(
             HKEY_CURRENT_USER,
             kEditMenuKey,
             L"Icon",
             &icon
         ) &&
         label == L"Edit with RegKit" &&
         _wcsicmp(icon.c_str(), EditMenuIcon(exe_path).c_str()) == 0;
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
    util::UniqueHKey verb_key;
    result = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kEditMenuKey,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        verb_key.put(),
        nullptr
    );
    if (result == ERROR_SUCCESS) {
      result = util::WriteRegistryString(
          verb_key.get(),
          nullptr,
          L"Edit with RegKit"
      );
    }
    if (result == ERROR_SUCCESS) {
      result = util::WriteRegistryString(
          verb_key.get(),
          L"Icon",
          EditMenuIcon(exe_path)
      );
    }

    util::UniqueHKey command_key;
    if (result == ERROR_SUCCESS) {
      result = RegCreateKeyExW(
          HKEY_CURRENT_USER,
          kEditMenuCommandKey,
          0,
          nullptr,
          REG_OPTION_NON_VOLATILE,
          KEY_SET_VALUE,
          nullptr,
          command_key.put(),
          nullptr
      );
    }
    if (result == ERROR_SUCCESS) {
      result = util::WriteRegistryString(
          command_key.get(),
          nullptr,
          EditMenuCommand(exe_path)
      );
    }
    if (result != ERROR_SUCCESS) {
      command_key.reset();
      verb_key.reset();
      const LONG cleanup = DeleteEditMenu();
      if (cleanup_error) {
        *cleanup_error = cleanup;
      }
    }
  } else {
    result = DeleteEditMenu();
  }

  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return result;
}

LONG RemoveRegFileEditMenuIfOwned(
    const std::wstring& exe_path
) {
  if (!IsEditMenuCommandOwned(exe_path)) {
    return ERROR_SUCCESS;
  }
  return SetRegFileEditMenu(exe_path, false);
}

bool IsRegeditReplacementRegistered(
    const std::wstring& exe_path
) {
  if (exe_path.empty()) {
    return false;
  }
  std::wstring debugger;
  return ReadRegeditDebugger(&debugger) == ERROR_SUCCESS &&
         OwnsRegeditDebugger(debugger, exe_path);
}

LONG SetRegeditReplacement(
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
    return DeleteOwnedRegeditDebugger(exe_path);
  }
  if (overwrite_existing) {
    return WriteRegeditDebugger(exe_path);
  }

  std::wstring debugger;
  bool value_exists = false;
  const LONG result = ReadRegeditDebugger(&debugger, &value_exists);
  if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) {
    return WriteRegeditDebugger(exe_path);
  }
  if (result != ERROR_SUCCESS) {
    if (value_exists) {
      if (conflict) {
        *conflict = true;
      }
      return ERROR_ALREADY_EXISTS;
    }
    return result;
  }
  if (OwnsRegeditDebugger(debugger, exe_path)) {
    return ERROR_SUCCESS;
  }
  if (conflict) {
    *conflict = true;
  }
  return ERROR_ALREADY_EXISTS;
}

} // namespace regkit::win32
