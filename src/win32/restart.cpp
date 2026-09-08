// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "win32/restart.h"

#include "win32/shell_paths.h"

#include <cerrno>

#include <shellapi.h>

namespace regkit::win32 {

bool ArgTakesValue(const std::wstring& arg) {
  return _wcsicmp(arg.c_str(), kRestartParentArg) == 0 ||
         _wcsicmp(arg.c_str(), kRestartDataDirArg) == 0;
}

std::wstring RestartDataDir(const std::vector<std::wstring>& args) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (_wcsicmp(args[i].c_str(), kRestartDataDirArg) == 0) {
      return args[i + 1];
    }
  }
  return L"";
}

bool RestoreSessionRequested() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return false;
  }
  bool requested = false;
  for (int i = 1; i < argc && !requested; ++i) {
    requested = _wcsicmp(argv[i], kRestartSessionArg) == 0;
  }
  LocalFree(argv);
  return requested;
}

namespace {

std::wstring QuoteArgument(const std::wstring& arg);

} // namespace

std::wstring RestartArguments(const wchar_t* target_arg, DWORD parent_pid) {
  std::wstring arguments;
  if (target_arg && *target_arg) {
    arguments = target_arg;
  }
  if (parent_pid != 0) {
    if (!arguments.empty()) {
      arguments.push_back(L' ');
    }
    arguments += kRestartParentArg;
    arguments.push_back(L' ');
    arguments += std::to_wstring(parent_pid);
  }
  const std::wstring data_dir = util::GetAppDataFolder();
  if (!data_dir.empty()) {
    if (!arguments.empty()) {
      arguments.push_back(L' ');
    }
    arguments += kRestartDataDirArg;
    arguments.push_back(L' ');
    arguments += QuoteArgument(data_dir);
    arguments.push_back(L' ');
    arguments += kRestartSessionArg;
  }
  return arguments;
}

namespace {

bool IsInternalRestartArg(const std::wstring& arg) {
  return _wcsicmp(arg.c_str(), kRestartSystemArg) == 0 ||
         _wcsicmp(arg.c_str(), kRestartTiArg) == 0 ||
         _wcsicmp(arg.c_str(), kRestartUserArg) == 0 ||
         _wcsicmp(arg.c_str(), kRestartAdminArg) == 0 ||
         _wcsicmp(arg.c_str(), kRestartSessionArg) == 0 ||
         ArgTakesValue(arg);
}

std::wstring QuoteArgument(const std::wstring& arg) {
  if (!arg.empty() &&
      arg.find_first_of(L" \t\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring quoted = L"\"";
  size_t backslashes = 0;
  for (wchar_t character : arg) {
    if (character == L'\\') {
      ++backslashes;
      quoted.push_back(character);
      continue;
    }
    if (character == L'"') {
      quoted.append(backslashes + 1, L'\\');
      backslashes = 0;
      quoted.push_back(L'"');
      continue;
    }
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

} // namespace

std::wstring RestartArguments(const wchar_t* target_arg, DWORD parent_pid,
                              const std::vector<std::wstring>& original_args) {
  std::wstring arguments = RestartArguments(target_arg, parent_pid);
  for (size_t i = 0; i < original_args.size(); ++i) {
    const std::wstring& arg = original_args[i];
    if (IsInternalRestartArg(arg)) {
      if (ArgTakesValue(arg)) {
        ++i;
      }
      continue;
    }
    if (!arguments.empty()) {
      arguments.push_back(L' ');
    }
    arguments += QuoteArgument(arg);
  }
  return arguments;
}

HRESULT LaunchElevated(HWND owner, const std::wstring& exe, const std::wstring& arguments) {
  if (exe.empty()) {
    return E_INVALIDARG;
  }
  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  info.hwnd = owner;
  info.lpVerb = L"runas";
  info.lpFile = exe.c_str();
  info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
  info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&info)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  if (info.hProcess) {
    CloseHandle(info.hProcess);
  }
  return S_OK;
}

DWORD RestartParentPid(const std::vector<std::wstring>& args) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (_wcsicmp(args[i].c_str(), kRestartParentArg) != 0) {
      continue;
    }
    const std::wstring& text = args[i + 1];
    if (text.empty() ||
        text.find_first_not_of(L"0123456789") != std::wstring::npos) {
      continue;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(text.c_str(), &end, 10);
    if (!end || *end != L'\0' || errno == ERANGE || value == 0 ||
        value > MAXDWORD) {
      continue;
    }
    return static_cast<DWORD>(value);
  }
  return 0;
}

void WaitForParentExit(DWORD parent_pid) {
  if (parent_pid == 0 || parent_pid == GetCurrentProcessId()) {
    return;
  }
  HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
  if (!parent) {
    return;
  }
  WaitForSingleObject(parent, 30000);
  CloseHandle(parent);
}

} // namespace regkit::win32
