// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "regfile/registry_transfer.h"

#include "registry/registry_path.h"
#include "win32/file_text.h"
#include "win32/handle_owner.h"
#include "win32/process_rights.h"
#include "win32/registry_native.h"
#include "win32/registry_view.h"
#include "win32/shell_paths.h"
#include "win32/system_error.h"
#include "win32/text_transform.h"
#include "win32/translation.h"

#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace regkit
{

namespace
{

bool RunRegCommand(const std::wstring& args, std::wstring* error)
{
    wchar_t system_dir[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system_dir, _countof(system_dir));
    if (length == 0 || length >= _countof(system_dir))
    {
        if (error)
        {
            *error = util::Tr(L"The system directory couldn't be resolved.");
        }
        return false;
    }
    const std::wstring reg = util::JoinPath(system_dir, L"reg.exe");
    std::wstring command_line = L"\"" + reg + L"\" " + args;

    SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
    util::UniqueHandle read_pipe;
    util::UniqueHandle write_pipe;
    util::UniqueHandle null_input(
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr)
    );
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<BYTE> attribute_storage(attribute_size);
    const auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    const bool attributes_ready =
        attribute_size != 0 && InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size);
    bool capture = attributes_ready && null_input && CreatePipe(read_pipe.put(), write_pipe.put(), &security, 0) &&
                   SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0);
    HANDLE inherited[2] = {write_pipe.get(), null_input.get()};
    capture = capture && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);

    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = capture ? sizeof(startup) : sizeof(startup.StartupInfo);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    if (capture)
    {
        startup.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = null_input.get();
        startup.StartupInfo.hStdOutput = write_pipe.get();
        startup.StartupInfo.hStdError = write_pipe.get();
        startup.lpAttributeList = attributes;
    }
    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessW(reg.c_str(), command_line.data(), nullptr, nullptr, capture, CREATE_NO_WINDOW | (capture ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, nullptr, &startup.StartupInfo, &process);
    const DWORD create_error = GetLastError();
    if (attributes_ready)
    {
        DeleteProcThreadAttributeList(attributes);
    }
    write_pipe.reset();
    null_input.reset();
    if (!created)
    {
        if (error)
        {
            *error = util::FormatWin32Error(create_error);
        }
        return false;
    }
    util::UniqueHandle process_handle(process.hProcess);
    CloseHandle(process.hThread);
    std::string output;
    char buffer[4096] = {};
    DWORD read = 0;
    while (capture && ReadFile(read_pipe.get(), buffer, sizeof(buffer), &read, nullptr) && read > 0)
    {
        output.append(buffer, read);
    }
    WaitForSingleObject(process_handle.get(), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process_handle.get(), &code);
    if (code != 0 && error)
    {
        const std::wstring detail = util::TrimWhitespace(util::NarrowToWide(output, CP_OEMCP));
        *error = detail.empty() ? util::TrDetail(L"reg.exe exited with an error.", std::to_wstring(code)) : detail;
    }
    return code == 0;
}

std::wstring SanitizeFileName(const std::wstring& name)
{
    std::wstring out;
    out.reserve(name.size());
    for (wchar_t ch : name)
    {
        out.push_back(ch < 32 || wcschr(L"<>:\"/\\|?*", ch) ? L'_' : ch);
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
    {
        out.pop_back();
    }
    out.erase(0, out.find_first_not_of(L' '));
    return out.empty() ? L"RegistryExport" : out;
}

} // namespace

std::wstring ExportFileName(const std::wstring& name, const wchar_t* extension)
{
    return util::EnsureFileExtension(SanitizeFileName(name), extension);
}

std::wstring DefaultExportPath(const std::wstring& key_path, const wchar_t* extension)
{
    const std::wstring file_name = ExportFileName(registry_path::Leaf(key_path), extension);
    const std::wstring desktop = util::GetShellUserDesktop();
    return desktop.empty() ? file_name : util::JoinPath(desktop, file_name);
}

bool ImportRegFileFromPath(const std::wstring& path, std::wstring* error)
{
    return !path.empty() &&
           RunRegCommand(L"import \"" + path + L"\" " + win32::RegExeViewSwitch(win32::kDefaultRegistryView), error);
}

bool IsHiveFile(const std::wstring& path)
{
    util::UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    char signature[4] = {};
    DWORD read = 0;
    return file.get() != INVALID_HANDLE_VALUE && ReadFile(file.get(), signature, sizeof(signature), &read, nullptr) && read == sizeof(signature) &&
           std::memcmp(signature, "regf", sizeof(signature)) == 0;
}

LONG SaveKeyToHive(HKEY root, const std::wstring& subkey, REGSAM view, const std::wstring& path)
{
    const util::PrivilegeScope privileges({SE_BACKUP_NAME});
    util::UniqueHKey handle;
    LONG status = util::OpenRegistryPath(root, subkey, KEY_READ | view, false, &handle);
    if (status != ERROR_SUCCESS)
    {
        return status;
    }
    // RegSaveKeyEx can't overwrite a file, so stage the save before replacing it
    std::wstring staged = path;
    const bool existed = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        if (existed)
        {
            staged = path + util::RandomFileSuffix(L".part");
        }
        status = RegSaveKeyExW(handle.get(), staged.c_str(), nullptr, REG_LATEST_FORMAT);
        if (!existed || status != ERROR_ALREADY_EXISTS)
        {
            break;
        }
    }
    handle.reset();
    if (status != ERROR_SUCCESS)
    {
        if (existed && status != ERROR_ALREADY_EXISTS)
        {
            DeleteFileW(staged.c_str());
        }
        return status;
    }
    if (existed && !MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        status = static_cast<LONG>(GetLastError());
        DeleteFileW(staged.c_str());
    }
    return status;
}

LONG RestoreKeyFromHive(HKEY root, const std::wstring& subkey, REGSAM view, const std::wstring& path)
{
    const util::PrivilegeScope privileges({SE_RESTORE_NAME, SE_BACKUP_NAME});
    if (!privileges.held())
    {
        return ERROR_PRIVILEGE_NOT_HELD;
    }
    util::UniqueHKey handle;
    const LONG status = util::OpenRegistryPath(root, subkey, KEY_WRITE | view, false, &handle);
    return status == ERROR_SUCCESS ? RegRestoreKeyW(handle.get(), path.c_str(), REG_FORCE_RESTORE) : status;
}

std::wstring HiveTransferError(LONG status, const std::wstring& path)
{
    if (status == ERROR_PRIVILEGE_NOT_HELD)
    {
        return util::Tr(L"Hive files need the backup and restore privileges. Run RegKit elevated.");
    }
    return util::FormatWin32Error(static_cast<DWORD>(status)) + L"\n" + path;
}

bool IsMountedHive(HKEY root, const std::wstring& subkey)
{
    if (subkey.empty() || subkey.find(L'\\') != std::wstring::npos ||
        (root != HKEY_LOCAL_MACHINE && root != HKEY_USERS))
    {
        return false;
    }
    const std::wstring native =
        (root == HKEY_LOCAL_MACHINE ? L"\\REGISTRY\\MACHINE\\" : L"\\REGISTRY\\USER\\") + subkey;
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\hivelist", native.c_str(), RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

} // namespace regkit
