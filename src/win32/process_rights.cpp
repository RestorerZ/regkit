// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "win32/process_rights.h"

#include <vector>

#include <sddl.h>
#include <userenv.h>
#include <winsvc.h>
#include <wtsapi32.h>

namespace {

class ScopedHandle {
public:
  ScopedHandle() noexcept = default;
  explicit ScopedHandle(
      HANDLE handle
  ) noexcept
      : handle_(handle) {
  }
  ~ScopedHandle() {
    reset();
  }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(
      ScopedHandle&& other
  ) noexcept
      : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  ScopedHandle& operator=(
      ScopedHandle&& other
  ) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  HANDLE get() const noexcept {
    return handle_;
  }
  HANDLE* put() noexcept {
    reset();
    return &handle_;
  }
  HANDLE release() noexcept {
    HANDLE temp = handle_;
    handle_ = nullptr;
    return temp;
  }
  void reset(
      HANDLE handle = nullptr
  ) noexcept {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }
  explicit operator bool() const noexcept {
    return handle_ && handle_ != INVALID_HANDLE_VALUE;
  }

private:
  HANDLE handle_ = nullptr;
};

class ScopedEnvBlock {
public:
  ScopedEnvBlock() noexcept = default;
  ~ScopedEnvBlock() {
    reset();
  }
  ScopedEnvBlock(const ScopedEnvBlock&) = delete;
  ScopedEnvBlock& operator=(const ScopedEnvBlock&) = delete;

  LPVOID get() const noexcept {
    return block_;
  }
  LPVOID* put() noexcept {
    reset();
    return &block_;
  }
  void reset(
      LPVOID block = nullptr
  ) noexcept {
    if (block_) {
      DestroyEnvironmentBlock(block_);
    }
    block_ = block;
  }

private:
  LPVOID block_ = nullptr;
};

DWORD GetActiveSessionId() {
  DWORD current = 0;
  if (ProcessIdToSessionId(GetCurrentProcessId(), &current)) {
    return current;
  }
  DWORD count = 0;
  PWTS_SESSION_INFOW sessions = nullptr;
  if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
    return static_cast<DWORD>(-1);
  }
  DWORD active_session = static_cast<DWORD>(-1);
  for (DWORD i = 0; i < count; ++i) {
    if (sessions[i].State == WTS_CONNECTSTATE_CLASS::WTSActive) {
      active_session = sessions[i].SessionId;
      break;
    }
  }
  WTSFreeMemory(sessions);
  return active_session;
}

bool CreateSystemToken(
    DWORD desired_access,
    HANDLE* token_handle
) {
  if (!token_handle) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  *token_handle = nullptr;

  DWORD lsass_pid = 0;
  DWORD winlogon_pid = 0;
  DWORD process_count = 0;
  PWTS_PROCESS_INFOW processes = nullptr;
  DWORD session_id = GetActiveSessionId();

  if (WTSEnumerateProcessesW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &processes, &process_count)) {
    for (DWORD i = 0; i < process_count; ++i) {
      const auto& process = processes[i];
      if (!process.pProcessName || !process.pUserSid || !IsWellKnownSid(process.pUserSid, WELL_KNOWN_SID_TYPE::WinLocalSystemSid)) {
        continue;
      }
      if (lsass_pid == 0 && process.SessionId == 0 && _wcsicmp(process.pProcessName, L"lsass.exe") == 0) {
        lsass_pid = process.ProcessId;
        continue;
      }
      if (winlogon_pid == 0 && process.SessionId == session_id && _wcsicmp(process.pProcessName, L"winlogon.exe") == 0) {
        winlogon_pid = process.ProcessId;
        continue;
      }
    }
    WTSFreeMemory(processes);
  }

  ScopedHandle system_process;
  if (lsass_pid != 0) {
    system_process.reset(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, lsass_pid));
  }
  if (!system_process && winlogon_pid != 0) {
    system_process.reset(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, winlogon_pid));
  }
  if (!system_process) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }

  ScopedHandle system_token;
  if (!OpenProcessToken(system_process.get(), TOKEN_DUPLICATE, system_token.put())) {
    return false;
  }

  if (!DuplicateTokenEx(system_token.get(), desired_access, nullptr, SecurityIdentification, TokenPrimary, token_handle)) {
    return false;
  }

  return true;
}

bool EnablePrivilege(
    HANDLE token,
    const wchar_t* privilege
) {
  if (!token || !privilege) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  LUID luid = {};
  if (!LookupPrivilegeValueW(nullptr, privilege, &luid)) {
    return false;
  }
  TOKEN_PRIVILEGES tp = {};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
  return GetLastError() == ERROR_SUCCESS;
}

bool AdjustTokenAllPrivileges(
    HANDLE token,
    DWORD attributes
) {
  DWORD length = 0;
  GetTokenInformation(token, TokenPrivileges, nullptr, 0, &length);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) {
    return false;
  }
  std::vector<BYTE> buffer(length);
  if (!GetTokenInformation(token, TokenPrivileges, buffer.data(), length, &length)) {
    return false;
  }
  auto* privileges = reinterpret_cast<TOKEN_PRIVILEGES*>(buffer.data());
  for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
    privileges->Privileges[i].Attributes = attributes;
  }
  AdjustTokenPrivileges(token, FALSE, privileges, length, nullptr, nullptr);
  return GetLastError() == ERROR_SUCCESS;
}

bool QueryServiceProcess(
    SC_HANDLE service,
    SERVICE_STATUS_PROCESS* status
) {
  if (!status) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  DWORD bytes = 0;
  return QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(status), sizeof(SERVICE_STATUS_PROCESS), &bytes) != FALSE;
}

bool WaitWhileServicePending(
    SC_HANDLE service,
    DWORD pending_state,
    SERVICE_STATUS_PROCESS* status
) {
  constexpr ULONGLONG kMaxWaitMs = 60000;
  const ULONGLONG start = GetTickCount64();
  DWORD checkpoint = status->dwCheckPoint;
  ULONGLONG progress = start;
  while (status->dwCurrentState == pending_state) {
    DWORD wait = status->dwWaitHint / 10;
    if (wait < 100) {
      wait = 100;
    } else if (wait > 2000) {
      wait = 2000;
    }
    Sleep(wait);
    if (!QueryServiceProcess(service, status)) {
      return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (status->dwCheckPoint > checkpoint) {
      checkpoint = status->dwCheckPoint;
      progress = now;
      continue;
    }
    const ULONGLONG budget = status->dwWaitHint > 5000 ? status->dwWaitHint : 5000;
    if (now - progress > budget || now - start > kMaxWaitMs) {
      SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
      return false;
    }
  }
  return true;
}

bool StartServiceAndGetProcessId(
    const wchar_t* service_name,
    DWORD* process_id
) {
  if (!service_name || !process_id) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  *process_id = 0;

  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) {
    return false;
  }
  SC_HANDLE service = OpenServiceW(scm, service_name, SERVICE_QUERY_STATUS | SERVICE_START);
  if (!service) {
    CloseServiceHandle(scm);
    return false;
  }

  SERVICE_STATUS_PROCESS status = {};
  if (!QueryServiceProcess(service, &status)) {
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return false;
  }

  auto fail = [&](DWORD error) {
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    SetLastError(error);
    return false;
  };

  if (!WaitWhileServicePending(service, SERVICE_STOP_PENDING, &status)) {
    return fail(GetLastError());
  }
  if (status.dwCurrentState == SERVICE_STOPPED) {
    if (!StartServiceW(service, 0, nullptr) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
      return fail(GetLastError());
    }
    if (!QueryServiceProcess(service, &status)) {
      return fail(GetLastError());
    }
  }
  if (!WaitWhileServicePending(service, SERVICE_START_PENDING, &status)) {
    return fail(GetLastError());
  }
  if (status.dwCurrentState != SERVICE_RUNNING || status.dwProcessId == 0) {
    DWORD error = status.dwWin32ExitCode;
    if (error == ERROR_SERVICE_SPECIFIC_ERROR) {
      error = status.dwServiceSpecificExitCode;
    }
    return fail(error != ERROR_SUCCESS ? error : ERROR_SERVICE_NOT_ACTIVE);
  }

  *process_id = status.dwProcessId;
  CloseServiceHandle(service);
  CloseServiceHandle(scm);
  return true;
}

bool OpenServiceProcessToken(
    const wchar_t* service_name,
    DWORD desired_access,
    HANDLE* token_handle
) {
  if (!token_handle) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  *token_handle = nullptr;

  DWORD process_id = 0;
  if (!StartServiceAndGetProcessId(service_name, &process_id)) {
    return false;
  }
  ScopedHandle process(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, process_id));
  if (!process) {
    return false;
  }
  if (!OpenProcessToken(process.get(), desired_access, token_handle)) {
    return false;
  }
  return true;
}

} // namespace

namespace util {

std::wstring GetCurrentUserSidString() {
  static const std::wstring cached = []() -> std::wstring {
    std::wstring sid_string;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return sid_string;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
      CloseHandle(token);
      return sid_string;
    }
    std::vector<BYTE> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
      CloseHandle(token);
      return sid_string;
    }
    auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    LPWSTR sid = nullptr;
    if (ConvertSidToStringSidW(user->User.Sid, &sid) && sid) {
      sid_string.assign(sid);
      LocalFree(sid);
    }
    CloseHandle(token);
    return sid_string;
  }();
  return cached;
}

bool IsProcessElevated() {
  ScopedHandle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put())) {
    return false;
  }
  TOKEN_ELEVATION elevation = {};
  DWORD size = 0;
  bool elevated = false;
  if (GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size)) {
    elevated = (elevation.TokenIsElevated != 0);
  }
  return elevated;
}

bool IsUacEnabled() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return true;
  }
  DWORD value = 1;
  DWORD size = sizeof(value);
  DWORD type = 0;
  const LONG result = RegQueryValueExW(key, L"EnableLUA", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS || type != REG_DWORD) {
    return true;
  }
  return value != 0;
}

bool IsProcessSystem() {
  ScopedHandle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put())) {
    return false;
  }
  DWORD size = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
    return false;
  }
  std::vector<BYTE> buffer(size);
  if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) {
    return false;
  }
  auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
  return IsWellKnownSid(user->User.Sid, WELL_KNOWN_SID_TYPE::WinLocalSystemSid) != FALSE;
}

bool IsProcessTrustedInstaller() {
  static const std::vector<BYTE> ti_sid = []() -> std::vector<BYTE> {
    const wchar_t* account = L"NT SERVICE\\TrustedInstaller";
    DWORD sid_size = 0;
    DWORD domain_size = 0;
    SID_NAME_USE use = SidTypeUnknown;
    LookupAccountNameW(nullptr, account, nullptr, &sid_size, nullptr, &domain_size, &use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sid_size == 0) {
      return {};
    }
    std::vector<BYTE> sid_buffer(sid_size);
    std::wstring domain(domain_size, L'\0');
    if (!LookupAccountNameW(nullptr, account, sid_buffer.data(), &sid_size, domain.data(), &domain_size, &use)) {
      return {};
    }
    sid_buffer.resize(sid_size);
    return sid_buffer;
  }();

  if (ti_sid.empty()) {
    return false;
  }

  ScopedHandle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put())) {
    return false;
  }
  DWORD size = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
    return false;
  }
  std::vector<BYTE> buffer(size);
  if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) {
    return false;
  }
  auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
  PSID ti_sid_ptr = const_cast<PSID>(static_cast<const void*>(ti_sid.data()));
  if (EqualSid(user->User.Sid, ti_sid_ptr)) {
    return true;
  }

  DWORD group_size = 0;
  GetTokenInformation(token.get(), TokenGroups, nullptr, 0, &group_size);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || group_size == 0) {
    return false;
  }
  std::vector<BYTE> group_buffer(group_size);
  if (!GetTokenInformation(token.get(), TokenGroups, group_buffer.data(), group_size, &group_size)) {
    return false;
  }
  auto* groups = reinterpret_cast<TOKEN_GROUPS*>(group_buffer.data());
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    if (EqualSid(groups->Groups[i].Sid, ti_sid_ptr)) {
      return true;
    }
  }
  return false;
}

bool LaunchProcessAsShellUser(
    const std::wstring& command_line,
    const std::wstring& work_dir,
    DWORD* error_code,
    bool* impersonation_lost
) {
  if (error_code) {
    *error_code = ERROR_SUCCESS;
  }
  if (impersonation_lost) {
    *impersonation_lost = false;
  }
  if (command_line.empty()) {
    SetLastError(ERROR_INVALID_PARAMETER);
    if (error_code) {
      *error_code = ERROR_INVALID_PARAMETER;
    }
    return false;
  }

  bool result = false;
  DWORD error = ERROR_SUCCESS;
  DWORD shell_pid = 0;
  const HWND shell = GetShellWindow();
  ScopedHandle shell_process;
  ScopedHandle shell_token;
  ScopedHandle target_token;
  ScopedEnvBlock env;
  STARTUPINFOW startup = {};
  PROCESS_INFORMATION process = {};
  std::wstring mutable_command;

  if (!shell || !GetWindowThreadProcessId(shell, &shell_pid) || shell_pid == 0) {
    error = ERROR_NOT_FOUND;
    goto Cleanup;
  }
  shell_process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shell_pid));
  if (!shell_process) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!OpenProcessToken(shell_process.get(), TOKEN_DUPLICATE, shell_token.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!DuplicateTokenEx(shell_token.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, target_token.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!CreateEnvironmentBlock(env.put(), target_token.get(), FALSE)) {
    error = GetLastError();
    goto Cleanup;
  }

  startup.cb = sizeof(startup);
  startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
  mutable_command = command_line;
  result = CreateProcessWithTokenW(target_token.get(), 0, nullptr, mutable_command.data(), CREATE_UNICODE_ENVIRONMENT, env.get(), work_dir.empty() ? nullptr : work_dir.c_str(), &startup, &process);
  if (!result) {
    error = GetLastError();
    goto Cleanup;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

Cleanup:
  if (!result) {
    if (error_code) {
      *error_code = error;
    }
    SetLastError(error);
  }
  return result;
}

bool LaunchProcessAsSystem(
    const std::wstring& command_line,
    const std::wstring& work_dir,
    DWORD* error_code,
    bool* impersonation_lost
) {
  if (error_code) {
    *error_code = ERROR_SUCCESS;
  }
  if (impersonation_lost) {
    *impersonation_lost = false;
  }
  if (command_line.empty()) {
    if (error_code) {
      *error_code = ERROR_INVALID_PARAMETER;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }

  bool result = false;
  DWORD error = ERROR_SUCCESS;

  ScopedHandle current_token;
  ScopedHandle current_impersonation;
  ScopedHandle system_token;
  ScopedHandle system_impersonation;
  ScopedHandle target_token;
  ScopedHandle previous_thread_token;
  ScopedEnvBlock env;
  bool had_thread_token = false;
  DWORD session_id = static_cast<DWORD>(-1);
  STARTUPINFOW startup = {};
  PROCESS_INFORMATION process = {};
  std::wstring mutable_command;

  if (OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE | TOKEN_QUERY, TRUE, previous_thread_token.put())) {
    had_thread_token = true;
  } else if (GetLastError() != ERROR_NO_TOKEN) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!OpenProcessToken(GetCurrentProcess(), MAXIMUM_ALLOWED, current_token.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!DuplicateTokenEx(current_token.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenImpersonation, current_impersonation.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!EnablePrivilege(current_impersonation.get(), SE_DEBUG_NAME)) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!SetThreadToken(nullptr, current_impersonation.get())) {
    error = GetLastError();
    goto Cleanup;
  }
  session_id = GetActiveSessionId();
  if (session_id == static_cast<DWORD>(-1)) {
    error = ERROR_NO_TOKEN;
    goto Cleanup;
  }
  if (!CreateSystemToken(MAXIMUM_ALLOWED, system_token.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!DuplicateTokenEx(system_token.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenImpersonation, system_impersonation.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!AdjustTokenAllPrivileges(system_impersonation.get(), SE_PRIVILEGE_ENABLED)) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!SetThreadToken(nullptr, system_impersonation.get())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!DuplicateTokenEx(system_token.get(), MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, target_token.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!SetTokenInformation(target_token.get(), TokenSessionId, &session_id, sizeof(session_id))) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!AdjustTokenAllPrivileges(target_token.get(), SE_PRIVILEGE_ENABLED)) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!CreateEnvironmentBlock(env.put(), target_token.get(), FALSE)) {
    error = GetLastError();
    goto Cleanup;
  }

  startup.cb = sizeof(startup);
  startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
  mutable_command = command_line;
  result = CreateProcessAsUserW(target_token.get(), nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.get(), work_dir.empty() ? nullptr : work_dir.c_str(), &startup, &process);
  if (!result) {
    error = GetLastError();
    goto Cleanup;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

Cleanup:
  {
    const bool restored = had_thread_token
                              ? SetThreadToken(nullptr, previous_thread_token.get()) != 0
                              : RevertToSelf() != 0;
    if (!restored) {
      const DWORD restore_error = GetLastError();
      if (impersonation_lost) {
        *impersonation_lost = true;
      }
      error = restore_error;
    }
  }
  if (!result || (impersonation_lost && *impersonation_lost)) {
    if (error_code) {
      *error_code = error;
    }
    SetLastError(error);
  }
  return result;
}

bool LaunchProcessAsTrustedInstaller(
    const std::wstring& command_line,
    const std::wstring& work_dir,
    DWORD* error_code,
    bool* impersonation_lost
) {
  if (error_code) {
    *error_code = ERROR_SUCCESS;
  }
  if (impersonation_lost) {
    *impersonation_lost = false;
  }
  if (command_line.empty()) {
    if (error_code) {
      *error_code = ERROR_INVALID_PARAMETER;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }

  bool result = false;
  DWORD error = ERROR_SUCCESS;

  ScopedHandle current_token;
  ScopedHandle current_impersonation;
  ScopedHandle system_token;
  ScopedHandle system_impersonation;
  ScopedHandle ti_token;
  ScopedHandle target_token;
  ScopedHandle previous_thread_token;
  ScopedEnvBlock env;
  bool had_thread_token = false;
  DWORD session_id = static_cast<DWORD>(-1);
  STARTUPINFOW startup = {};
  PROCESS_INFORMATION process = {};
  std::wstring mutable_command;

  if (OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE | TOKEN_QUERY, TRUE, previous_thread_token.put())) {
    had_thread_token = true;
  } else if (GetLastError() != ERROR_NO_TOKEN) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!OpenProcessToken(GetCurrentProcess(), MAXIMUM_ALLOWED, current_token.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!DuplicateTokenEx(current_token.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenImpersonation, current_impersonation.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!EnablePrivilege(current_impersonation.get(), SE_DEBUG_NAME)) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!SetThreadToken(nullptr, current_impersonation.get())) {
    error = GetLastError();
    goto Cleanup;
  }
  session_id = GetActiveSessionId();
  if (session_id == static_cast<DWORD>(-1)) {
    error = ERROR_NO_TOKEN;
    goto Cleanup;
  }
  if (!CreateSystemToken(MAXIMUM_ALLOWED, system_token.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!DuplicateTokenEx(system_token.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenImpersonation, system_impersonation.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!AdjustTokenAllPrivileges(system_impersonation.get(), SE_PRIVILEGE_ENABLED)) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!SetThreadToken(nullptr, system_impersonation.get())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!OpenServiceProcessToken(L"TrustedInstaller", MAXIMUM_ALLOWED, ti_token.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!DuplicateTokenEx(ti_token.get(), MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, target_token.put())) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!SetTokenInformation(target_token.get(), TokenSessionId, &session_id, sizeof(session_id))) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!AdjustTokenAllPrivileges(target_token.get(), SE_PRIVILEGE_ENABLED)) {
    error = GetLastError();
    goto Cleanup;
  }
  if (!CreateEnvironmentBlock(env.put(), target_token.get(), FALSE)) {
    error = GetLastError();
    goto Cleanup;
  }

  startup.cb = sizeof(startup);
  startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
  mutable_command = command_line;
  result = CreateProcessAsUserW(target_token.get(), nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.get(), work_dir.empty() ? nullptr : work_dir.c_str(), &startup, &process);
  if (!result) {
    error = GetLastError();
    goto Cleanup;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

Cleanup:
  {
    const bool restored = had_thread_token
                              ? SetThreadToken(nullptr, previous_thread_token.get()) != 0
                              : RevertToSelf() != 0;
    if (!restored) {
      const DWORD restore_error = GetLastError();
      if (impersonation_lost) {
        *impersonation_lost = true;
      }
      error = restore_error;
    }
  }
  if (!result || (impersonation_lost && *impersonation_lost)) {
    if (error_code) {
      *error_code = error;
    }
    SetLastError(error);
  }
  return result;
}

} // namespace util
