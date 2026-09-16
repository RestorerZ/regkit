// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "win32/text_transform.h"
#include "win32/restart.h"

#include "win32/shell_paths.h"

#include <cerrno>

#include <shellapi.h>

namespace regkit::win32 {

bool ArgTakesValue(
    const std::wstring& arg
) {
  return util::EqualsInsensitive(arg, kRestartParentArg) ||
         util::EqualsInsensitive(arg, kRestartDataDirArg);
}

std::wstring RestartDataDir(
    const std::vector<std::wstring>& args
) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (util::EqualsInsensitive(args[i], kRestartDataDirArg)) {
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
    requested = util::EqualsInsensitive(argv[i], kRestartSessionArg);
  }
  LocalFree(argv);
  return requested;
}

namespace {

bool IsInternalRestartArg(
    const std::wstring& arg
) {
  for (const wchar_t* flag : {kRestartSystemArg, kRestartTiArg, kRestartUserArg, kRestartAdminArg, kRestartSessionArg}) {
    if (util::EqualsInsensitive(arg, flag)) {
      return true;
    }
  }
  return ArgTakesValue(arg);
}

std::wstring QuoteArgument(
    const std::wstring& arg
) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring quoted = L"\"";
  size_t backslashes = 0;
  for (wchar_t character : arg) {
    if (character == L'"') {
      quoted.append(backslashes + 1, L'\\');
    }
    backslashes = character == L'\\' ? backslashes + 1 : 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

void AppendArgument(
    std::wstring* arguments,
    const std::wstring& arg
) {
  if (!arguments->empty()) {
    arguments->push_back(L' ');
  }
  arguments->append(arg);
}

} // namespace

std::wstring RestartArguments(
    const wchar_t* target_arg,
    DWORD parent_pid,
    bool restore_session
) {
  std::wstring arguments = target_arg ? target_arg : L"";
  if (parent_pid != 0) {
    AppendArgument(&arguments, kRestartParentArg);
    AppendArgument(&arguments, std::to_wstring(parent_pid));
  }
  const std::wstring data_dir = util::GetAppDataFolder();
  if (!data_dir.empty()) {
    AppendArgument(&arguments, kRestartDataDirArg);
    AppendArgument(&arguments, QuoteArgument(data_dir));
    if (restore_session) {
      AppendArgument(&arguments, kRestartSessionArg);
    }
  }
  return arguments;
}
std::wstring RestartArguments(
    const wchar_t* target_arg,
    DWORD parent_pid,
    const std::vector<std::wstring>& original_args
) {
  std::wstring arguments = RestartArguments(target_arg, parent_pid);
  for (size_t i = 0; i < original_args.size(); ++i) {
    const std::wstring& arg = original_args[i];
    if (IsInternalRestartArg(arg)) {
      if (ArgTakesValue(arg)) {
        ++i;
      }
      continue;
    }
    AppendArgument(&arguments, QuoteArgument(arg));

  }
  return arguments;
}

HRESULT LaunchElevated(
    HWND owner,
    const std::wstring& exe,
    const std::wstring& arguments
) {
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

DWORD RestartParentPid(
    const std::vector<std::wstring>& args
) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (!util::EqualsInsensitive(args[i], kRestartParentArg)) {
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

void WaitForParentExit(
    DWORD parent_pid
) {
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
