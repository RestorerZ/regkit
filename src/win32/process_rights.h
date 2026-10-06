// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include "win32/handle_owner.h"

#include <windows.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace regkit::util
{

bool EnableTokenPrivilege(HANDLE token, const wchar_t* name, TOKEN_PRIVILEGES* previous = nullptr);

class PrivilegeScope
{
  public:
    explicit PrivilegeScope(std::initializer_list<const wchar_t*> names);
    ~PrivilegeScope();
    PrivilegeScope(const PrivilegeScope&) = delete;
    PrivilegeScope& operator=(const PrivilegeScope&) = delete;

    bool held() const noexcept
    {
        return held_;
    }

  private:
    UniqueHandle token_;
    std::vector<TOKEN_PRIVILEGES> previous_;
    bool held_ = false;
};

// the user HKEY_CURRENT_USER belongs to the signed in user while following it
std::wstring GetCurrentUserSidString();
std::wstring GetClassesUserSidString();
std::wstring GetShellUserSidString();
bool ShellUserDiffers();
void SetCurrentUserFollowsShell(bool enable);
bool CurrentUserFollowsShell();
HKEY MapCurrentUserRoot(HKEY root);
std::wstring AccountName(const std::wstring& sid);
std::wstring GetShellUserDocuments();
std::wstring GetProcessImagePath(DWORD process_id);
bool IsProcessElevated();
bool IsProcessPrivileged();
bool IsWritableByNonAdmins(const std::wstring& file_path);
bool IsProcessSystem();
bool IsUacEnabled();
bool IsProcessTrustedInstaller();
UniqueHandle OpenShellToken(DWORD access);
struct ServiceState
{
    DWORD state = 0;
    DWORD start_type = 0;
};
LONG QueryRemoteRegistryService(const std::wstring& machine, ServiceState* state);
LONG StartRemoteRegistryService(const std::wstring& machine, bool enable);
LONG StopRemoteRegistryService(const std::wstring& machine, DWORD start_type);
bool LaunchProcessAsSystem(const std::wstring& command_line, const std::wstring& work_dir, DWORD* error_code = nullptr, bool* impersonation_lost = nullptr);
bool LaunchProcessAsShellUser(const std::wstring& command_line, const std::wstring& work_dir, DWORD* error_code = nullptr, bool* impersonation_lost = nullptr);
bool LaunchProcessAsTrustedInstaller(const std::wstring& command_line, const std::wstring& work_dir, DWORD* error_code = nullptr, bool* impersonation_lost = nullptr);

} // namespace regkit::util
