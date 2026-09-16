// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "win32/text_transform.h"
#include "win32/process_rights.h"
#include "win32/shell_paths.h"

#include <algorithm>
#include <vector>

#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>
#include <winsvc.h>
#include <wtsapi32.h>

namespace {

using util::UniqueHandle;

struct DestroyEnvironment {
  void operator()(LPVOID block) const noexcept {
    DestroyEnvironmentBlock(block);
  }
};

struct CloseService {
  void operator()(SC_HANDLE service) const noexcept {
    CloseServiceHandle(service);
  }
};

using UniqueEnvironment = util::UniqueResource<LPVOID, DestroyEnvironment>;
using UniqueService = util::UniqueResource<SC_HANDLE, CloseService>;

constexpr DWORD kNoSession = static_cast<DWORD>(-1);

std::vector<BYTE> TokenInformation(
    HANDLE token,
    TOKEN_INFORMATION_CLASS type
) {
  DWORD size = 0;
  GetTokenInformation(token, type, nullptr, 0, &size);
  std::vector<BYTE> buffer(size);
  if (size == 0 || !GetTokenInformation(token, type, buffer.data(), size, &size)) {
    buffer.clear();
  }
  return buffer;
}

std::vector<BYTE> CurrentTokenInformation(
    TOKEN_INFORMATION_CLASS type
) {
  UniqueHandle token;
  return OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) ? TokenInformation(token.get(), type) : std::vector<BYTE>();
}

PSID CurrentUserSid(
    const std::vector<BYTE>& buffer
) {
  return buffer.empty() ? nullptr : reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid;
}

DWORD GetActiveSessionId() {
  DWORD current = 0;
  if (ProcessIdToSessionId(GetCurrentProcessId(), &current)) {
    return current;
  }
  DWORD count = 0;
  PWTS_SESSION_INFOW sessions = nullptr;
  if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
    return kNoSession;
  }
  DWORD active_session = kNoSession;
  for (DWORD i = 0; i < count; ++i) {
    if (sessions[i].State == WTS_CONNECTSTATE_CLASS::WTSActive) {
      active_session = sessions[i].SessionId;
      break;
    }
  }
  WTSFreeMemory(sessions);
  return active_session;
}

bool OpenSystemToken(
    DWORD session_id,
    HANDLE* token
) {
  DWORD lsass_pid = 0;
  DWORD winlogon_pid = 0;
  DWORD process_count = 0;
  PWTS_PROCESS_INFOW processes = nullptr;
  if (WTSEnumerateProcessesW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &processes, &process_count)) {
    for (DWORD i = 0; i < process_count; ++i) {
      const auto& process = processes[i];
      if (!process.pProcessName || !process.pUserSid || !IsWellKnownSid(process.pUserSid, WELL_KNOWN_SID_TYPE::WinLocalSystemSid)) {
        continue;
      }
      if (lsass_pid == 0 && process.SessionId == 0 && util::EqualsInsensitive(process.pProcessName, L"lsass.exe")) {
        lsass_pid = process.ProcessId;
      } else if (winlogon_pid == 0 && process.SessionId == session_id && util::EqualsInsensitive(process.pProcessName, L"winlogon.exe")) {
        winlogon_pid = process.ProcessId;
      }
    }
    WTSFreeMemory(processes);
  }

  UniqueHandle system_process;
  // prefer session zero SYSTEM token and fall back to the active session
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
  return OpenProcessToken(system_process.get(), TOKEN_DUPLICATE, token) != FALSE;
}

bool EnableAllPrivileges(
    HANDLE token
) {
  std::vector<BYTE> buffer = TokenInformation(token, TokenPrivileges);
  if (buffer.empty()) {
    return false;
  }
  auto* privileges = reinterpret_cast<TOKEN_PRIVILEGES*>(buffer.data());
  for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
    privileges->Privileges[i].Attributes = SE_PRIVILEGE_ENABLED;
  }
  AdjustTokenPrivileges(token, FALSE, privileges, static_cast<DWORD>(buffer.size()), nullptr, nullptr);
  return GetLastError() == ERROR_SUCCESS;
}

bool QueryServiceProcess(
    SC_HANDLE service,
    SERVICE_STATUS_PROCESS* status
) {
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
    Sleep(std::clamp<DWORD>(status->dwWaitHint / 10, 100, 2000));
    if (!QueryServiceProcess(service, status)) {
      return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (status->dwCheckPoint > checkpoint) {
      checkpoint = status->dwCheckPoint;
      progress = now;
      continue;
    }
    if (now - progress > std::max<ULONGLONG>(status->dwWaitHint, 5000) || now - start > kMaxWaitMs) {
      SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
      return false;
    }
  }
  return true;
}

bool OpenServiceProcessToken(
    const wchar_t* service_name,
    HANDLE* token
) {
  UniqueService scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  UniqueService service(scm ? OpenServiceW(scm.get(), service_name, SERVICE_QUERY_STATUS | SERVICE_START) : nullptr);
  SERVICE_STATUS_PROCESS status = {};
  if (!service || !QueryServiceProcess(service.get(), &status) ||
      !WaitWhileServicePending(service.get(), SERVICE_STOP_PENDING, &status)) {
    return false;
  }
  if (status.dwCurrentState == SERVICE_STOPPED) {
    if (!StartServiceW(service.get(), 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
      return false;
    }
    if (!QueryServiceProcess(service.get(), &status)) {
      return false;
    }
  }
  if (!WaitWhileServicePending(service.get(), SERVICE_START_PENDING, &status)) {
    return false;
  }
  if (status.dwCurrentState != SERVICE_RUNNING || status.dwProcessId == 0) {
    DWORD error = status.dwWin32ExitCode == ERROR_SERVICE_SPECIFIC_ERROR ? status.dwServiceSpecificExitCode : status.dwWin32ExitCode;
    SetLastError(error != ERROR_SUCCESS ? error : ERROR_SERVICE_NOT_ACTIVE);
    return false;
  }
  UniqueHandle process(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, status.dwProcessId));
  return process && OpenProcessToken(process.get(), MAXIMUM_ALLOWED, token);
}

bool LaunchWithToken(
    HANDLE token,
    const std::wstring& command_line,
    const std::wstring& work_dir,
    bool as_user
) {
  UniqueEnvironment env;
  if (!CreateEnvironmentBlock(env.put(), token, FALSE)) {
    return false;
  }
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
  PROCESS_INFORMATION process = {};
  std::wstring command = command_line;
  const wchar_t* directory = work_dir.empty() ? nullptr : work_dir.c_str();
  const BOOL launched = as_user
                            ? CreateProcessAsUserW(token, nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.get(), directory, &startup, &process)
                            : CreateProcessWithTokenW(token, 0, nullptr, command.data(), CREATE_UNICODE_ENVIRONMENT, env.get(), directory, &startup, &process);
  if (!launched) {
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

bool LaunchElevatedToken(
    const std::wstring& command_line,
    const std::wstring& work_dir,
    const wchar_t* service_name
) {
  UniqueHandle current_token;
  UniqueHandle current_impersonation;
  UniqueHandle system_token;
  UniqueHandle system_impersonation;
  UniqueHandle source_token;
  UniqueHandle target_token;
  DWORD session_id = kNoSession;
  // get debug access before opening a protected SYSTEM process
  if (!OpenProcessToken(GetCurrentProcess(), MAXIMUM_ALLOWED, current_token.put()) ||
      !DuplicateTokenEx(current_token.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenImpersonation, current_impersonation.put()) ||
      !util::EnableTokenPrivilege(current_impersonation.get(), SE_DEBUG_NAME) ||
      !SetThreadToken(nullptr, current_impersonation.get())) {
    return false;
  }
  session_id = GetActiveSessionId();
  if (session_id == kNoSession) {
    SetLastError(ERROR_NO_TOKEN);
    return false;
  }
  if (!OpenSystemToken(session_id, system_token.put()) ||
      !DuplicateTokenEx(system_token.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenImpersonation, system_impersonation.put()) ||
      !EnableAllPrivileges(system_impersonation.get()) ||
      !SetThreadToken(nullptr, system_impersonation.get())) {
    return false;
  }
  if (service_name && !OpenServiceProcessToken(service_name, source_token.put())) {
    return false;
  }
  // attach primary token to the active session before launching the UI
  return DuplicateTokenEx(service_name ? source_token.get() : system_token.get(), MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, target_token.put()) &&
         SetTokenInformation(target_token.get(), TokenSessionId, &session_id, sizeof(session_id)) &&
         EnableAllPrivileges(target_token.get()) &&
         LaunchWithToken(target_token.get(), command_line, work_dir, true);
}

bool ReportLaunch(
    bool launched,
    DWORD* error_code
) {
  const DWORD error = launched ? ERROR_SUCCESS : GetLastError();
  if (error_code) {
    *error_code = error;
  }
  SetLastError(error);
  return launched;
}

bool LaunchImpersonated(
    const std::wstring& command_line,
    const std::wstring& work_dir,
    const wchar_t* service_name,
    DWORD* error_code,
    bool* impersonation_lost
) {
  if (impersonation_lost) {
    *impersonation_lost = false;
  }
  if (command_line.empty()) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return ReportLaunch(false, error_code);
  }
  UniqueHandle previous_thread_token;
  const bool had_thread_token = OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE | TOKEN_QUERY, TRUE, previous_thread_token.put()) != FALSE;
  if (!had_thread_token && GetLastError() != ERROR_NO_TOKEN) {
    return ReportLaunch(false, error_code);
  }
  const bool launched = LaunchElevatedToken(command_line, work_dir, service_name);
  DWORD error = launched ? ERROR_SUCCESS : GetLastError();
  if (had_thread_token ? !SetThreadToken(nullptr, previous_thread_token.get()) : !RevertToSelf()) {
    error = GetLastError();
    if (impersonation_lost) {
      *impersonation_lost = true;
    }
  }
  if (error_code) {
    *error_code = error;
  }
  SetLastError(error);
  return launched;
}

} // namespace

namespace util {

bool EnableTokenPrivilege(
    HANDLE token,
    const wchar_t* name,
    TOKEN_PRIVILEGES* previous
) {
  TOKEN_PRIVILEGES privileges = {};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  DWORD previous_size = previous ? sizeof(TOKEN_PRIVILEGES) : 0;
  return LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid) &&
         AdjustTokenPrivileges(token, FALSE, &privileges, previous_size, previous, previous ? &previous_size : nullptr) &&
         GetLastError() == ERROR_SUCCESS;
}

PrivilegeScope::PrivilegeScope(
    std::initializer_list<const wchar_t*> names
) {
  held_ = OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, token_.put()) != FALSE;
  for (const wchar_t* name : names) {
    if (!held_) {
      break;
    }
    TOKEN_PRIVILEGES previous = {};
    held_ = EnableTokenPrivilege(token_.get(), name, &previous);
    previous_.push_back(previous);
  }
}

PrivilegeScope::~PrivilegeScope() {
  // restore each privilege to the state captured when the scope began
  for (auto previous = previous_.rbegin(); previous != previous_.rend(); ++previous) {
    if (previous->PrivilegeCount != 0) {
      AdjustTokenPrivileges(token_.get(), FALSE, &*previous, sizeof(*previous), nullptr, nullptr);
    }
  }
}

std::wstring GetProcessImagePath(
    DWORD process_id
) {
  UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
  if (!process) {
    return {};
  }
  for (DWORD capacity = MAX_PATH; capacity <= 32768; capacity *= 2) {
    std::wstring path(capacity, L'\0');
    DWORD length = capacity;
    if (QueryFullProcessImageNameW(process.get(), 0, path.data(), &length)) {
      path.resize(length);
      return path;
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      return {};
    }
  }
  return {};
}

std::wstring GetCurrentUserSidString() {
  static const std::wstring cached = [] {
    std::wstring sid_string;
    LPWSTR sid = nullptr;
    const std::vector<BYTE> user = CurrentTokenInformation(TokenUser);
    if (!user.empty() && ConvertSidToStringSidW(CurrentUserSid(user), &sid)) {
      sid_string.assign(sid);
      LocalFree(sid);
    }
    return sid_string;
  }();
  return cached;
}

bool IsProcessElevated() {
  const std::vector<BYTE> elevation = CurrentTokenInformation(TokenElevation);
  return elevation.size() >= sizeof(TOKEN_ELEVATION) && reinterpret_cast<const TOKEN_ELEVATION*>(elevation.data())->TokenIsElevated != 0;
}

bool IsUacEnabled() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"EnableLUA", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS || value != 0;
}

bool GrantsWriteToStandardUsers(
    const std::wstring& path,
    const std::vector<std::vector<BYTE>>& group_sids
) {
  constexpr ACCESS_MASK kWriteAccess = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PACL dacl = nullptr;
  if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) {
    return true;
  }
  bool writable = false;
  for (DWORD index = 0; dacl && !writable && index < dacl->AceCount; ++index) {
    void* entry = nullptr;
    if (!GetAce(dacl, index, &entry)) {
      continue;
    }
    const auto* header = static_cast<const ACE_HEADER*>(entry);
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE) != 0 || (ace->Mask & kWriteAccess) == 0) {
      continue;
    }
    PSID ace_sid = const_cast<PSID>(static_cast<const void*>(&ace->SidStart));
    for (const std::vector<BYTE>& sid : group_sids) {
      writable = writable || EqualSid(const_cast<PSID>(static_cast<const void*>(sid.data())), ace_sid) != FALSE;
    }
  }
  LocalFree(descriptor);
  return writable;
}

bool IsExecutableLocationWritableByOtherUsers() {
  static const bool writable = []() {
    const std::wstring module = GetModulePath();
    if (module.empty()) {
      return true;
    }
    const DWORD attributes = GetFileAttributesW(module.c_str());
    // reject reparse points before trusting the executable path
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return true;
    }
    std::vector<std::vector<BYTE>> group_sids;
    for (const wchar_t* text : {L"S-1-1-0", L"S-1-5-11", L"S-1-5-32-545", L"S-1-5-4"}) {
      PSID sid = nullptr;
      if (ConvertStringSidToSidW(text, &sid)) {
        group_sids.emplace_back(static_cast<BYTE*>(sid), static_cast<BYTE*>(sid) + GetLengthSid(sid));
        LocalFree(sid);
      }
    }
    // writable executable/parent dir lets another user replace the target
    for (std::wstring path = module; path.size() > 3;) {
      if (GrantsWriteToStandardUsers(path, group_sids)) {
        return true;
      }
      const size_t slash = path.find_last_of(L'\\');
      if (slash == std::wstring::npos) {
        break;
      }
      path.resize(slash <= 2 ? slash + 1 : slash);
    }
    return false;
  }();
  return writable;
}

bool IsProcessPrivileged() {
  return IsProcessElevated() || IsProcessSystem() || IsProcessTrustedInstaller();
}

bool IsProcessSystem() {
  const std::vector<BYTE> user = CurrentTokenInformation(TokenUser);
  return !user.empty() && IsWellKnownSid(CurrentUserSid(user), WELL_KNOWN_SID_TYPE::WinLocalSystemSid) != FALSE;
}

bool IsProcessTrustedInstaller() {
  static const std::vector<BYTE> ti_sid = [] {
    const wchar_t* account = L"NT SERVICE\\TrustedInstaller";
    DWORD sid_size = 0;
    DWORD domain_size = 0;
    SID_NAME_USE use = SidTypeUnknown;
    LookupAccountNameW(nullptr, account, nullptr, &sid_size, nullptr, &domain_size, &use);
    std::vector<BYTE> sid_buffer(sid_size);
    std::wstring domain(domain_size, L'\0');
    if (sid_size == 0 || !LookupAccountNameW(nullptr, account, sid_buffer.data(), &sid_size, domain.data(), &domain_size, &use)) {
      sid_buffer.clear();
    }
    return sid_buffer;
  }();
  if (ti_sid.empty()) {
    return false;
  }
  const PSID ti = const_cast<PSID>(static_cast<const void*>(ti_sid.data()));
  UniqueHandle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put())) {
    return false;
  }
  const std::vector<BYTE> user = TokenInformation(token.get(), TokenUser);
  if (!user.empty() && EqualSid(CurrentUserSid(user), ti)) {
    return true;
  }
  const std::vector<BYTE> group_buffer = TokenInformation(token.get(), TokenGroups);
  if (group_buffer.empty()) {
    return false;
  }
  const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(group_buffer.data());
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    if (EqualSid(groups->Groups[i].Sid, ti)) {
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
  if (impersonation_lost) {
    *impersonation_lost = false;
  }
  if (command_line.empty()) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return ReportLaunch(false, error_code);
  }
  DWORD shell_pid = 0;
  const HWND shell = GetShellWindow();
  if (!shell || !GetWindowThreadProcessId(shell, &shell_pid) || shell_pid == 0) {
    SetLastError(ERROR_NOT_FOUND);
    return ReportLaunch(false, error_code);
  }
  // shell token returns privileged restarts to the signed in user
  UniqueHandle shell_process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shell_pid));
  UniqueHandle shell_token;
  UniqueHandle target_token;
  return ReportLaunch(
      shell_process &&
          OpenProcessToken(shell_process.get(), TOKEN_DUPLICATE, shell_token.put()) &&
          DuplicateTokenEx(shell_token.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, target_token.put()) &&
          LaunchWithToken(target_token.get(), command_line, work_dir, false),
      error_code
  );
}

bool LaunchProcessAsSystem(
    const std::wstring& command_line,
    const std::wstring& work_dir,
    DWORD* error_code,
    bool* impersonation_lost
) {
  return LaunchImpersonated(command_line, work_dir, nullptr, error_code, impersonation_lost);
}

bool LaunchProcessAsTrustedInstaller(
    const std::wstring& command_line,
    const std::wstring& work_dir,
    DWORD* error_code,
    bool* impersonation_lost
) {
  return LaunchImpersonated(command_line, work_dir, L"TrustedInstaller", error_code, impersonation_lost);
}

} // namespace util
