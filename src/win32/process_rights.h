// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include "win32/handle_owner.h"

#include <windows.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace util {

bool EnableTokenPrivilege(HANDLE token, const wchar_t* name, TOKEN_PRIVILEGES* previous = nullptr);

class PrivilegeScope {
public:
  explicit PrivilegeScope(std::initializer_list<const wchar_t*> names);
  ~PrivilegeScope();
  PrivilegeScope(const PrivilegeScope&) = delete;
  PrivilegeScope& operator=(const PrivilegeScope&) = delete;

  bool held() const noexcept {
    return held_;
  }

private:
  UniqueHandle token_;
  std::vector<TOKEN_PRIVILEGES> previous_;
  bool held_ = false;
};

std::wstring GetCurrentUserSidString();
std::wstring GetProcessImagePath(DWORD process_id);
bool IsProcessElevated();
bool IsProcessPrivileged();
bool IsExecutableLocationWritableByOtherUsers();
bool IsProcessSystem();
bool IsUacEnabled();
bool IsProcessTrustedInstaller();
bool LaunchProcessAsSystem(const std::wstring& command_line, const std::wstring& work_dir, DWORD* error_code = nullptr, bool* impersonation_lost = nullptr);
bool LaunchProcessAsShellUser(const std::wstring& command_line, const std::wstring& work_dir, DWORD* error_code = nullptr, bool* impersonation_lost = nullptr);
bool LaunchProcessAsTrustedInstaller(const std::wstring& command_line, const std::wstring& work_dir, DWORD* error_code = nullptr, bool* impersonation_lost = nullptr);

} // namespace util
