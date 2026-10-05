// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "regfile/registry_transfer.h"

#include "appearance/feedback.h"
#include "editors/export_dialog.h"
#include "editors/hive_dialog.h"
#include "editors/value_editor.h"
#include "regfile/reg_file.h"
#include "registry/key_algorithms.h"
#include "registry/registry_path.h"
#include "registry/registry_store.h"
#include "win32/file_dialog.h"
#include "win32/file_text.h"
#include "win32/handle_owner.h"
#include "win32/process_rights.h"
#include "win32/registry_native.h"
#include "win32/registry_view.h"
#include "win32/shell_paths.h"
#include "win32/system_error.h"
#include "win32/text_transform.h"
#include "win32/translation.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <vector>

#include <shlobj.h>
namespace regkit
{

namespace
{

using util::FormatWin32Error;

constexpr wchar_t kRegFileFilter[] = L"Registry Files (*.reg)\0*.reg\0All Files (*.*)\0*.*\0";

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
            *error = FormatWin32Error(create_error);
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

bool ResolveExportKey(const std::wstring& key_path, RegistryNode* node, std::wstring* display, std::wstring* error)
{
    *display = registry_path::Normalize(key_path, util::GetCurrentUserSidString());
    if (!registry_path::ParseRoot(*display, node) || !node->root)
    {
        if (error)
        {
            *error = util::Tr(L"Export supports the standard root keys only.");
        }
        return false;
    }
    return true;
}

bool WriteRegFile(const std::wstring& path, regfile::Writer&& writer, std::wstring* error)
{
    if (util::WriteTextFile(path, std::move(writer).Finish(), true))
    {
        return true;
    }
    if (error)
    {
        *error = util::TrDetail(L"Failed to write the exported registry file.", path);
    }
    return false;
}

bool ReportUnreadableKey(LONG status, const std::wstring& display, std::wstring* error)
{
    if (error)
    {
        *error = util::FormatWin32Error(static_cast<DWORD>(status)) + L"\n" + display;
    }
    return false;
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

std::wstring DefaultExportPath(const std::wstring& key_path, const wchar_t* extension)
{
    const std::wstring file_name = util::EnsureFileExtension(SanitizeFileName(registry_path::Leaf(key_path)), extension);
    const std::wstring desktop = util::GetShellUserDesktop();
    return desktop.empty() ? file_name : util::JoinPath(desktop, file_name);
}

bool ImportRegFileFromPath(const std::wstring& path, std::wstring* error)
{
    return !path.empty() &&
           RunRegCommand(L"import \"" + path + L"\" " + win32::RegExeViewSwitch(win32::kDefaultRegistryView), error);
}

bool ExportRegFile(HWND owner, const std::wstring& key_path, bool allow_hive, std::wstring* error, std::wstring* saved_path, win32::OpenAfter* open_after)
{
    static win32::OpenAfter last_open_after = win32::OpenAfter::kNone;
    editors::ExportRequest request;
    request.path = DefaultExportPath(key_path, L".reg");
    request.open_after = last_open_after;
    request.allow_hive = allow_hive;
    editors::ExportResult options;
    if (!editors::ChooseExport(owner, request, &options))
    {
        return false;
    }
    RegistryNode node;
    std::wstring display;
    if (!ResolveExportKey(key_path, &node, &display, error))
    {
        return false;
    }
    if (options.hive)
    {
        const LONG saved = SaveKeyToHive(node.root, node.subkey, win32::kDefaultRegistryView, options.path);
        if (saved != ERROR_SUCCESS)
        {
            *error = HiveTransferError(saved, options.path);
            return false;
        }
        *saved_path = options.path;
        *open_after = win32::OpenAfter::kNone;
        return true;
    }
    last_open_after = options.open_after;
    regfile::Writer writer;
    std::vector<std::wstring> skipped;
    const LONG status = regfile::AppendRegistryTree(&writer, node.root, node.subkey, display, win32::kDefaultRegistryView, options.include_subkeys, &skipped);
    if (status != ERROR_SUCCESS)
    {
        return ReportUnreadableKey(status, display, error);
    }
    if ((!skipped.empty() && !ui::ConfirmConversionSkips(owner, skipped, util::Tr(L"Export"))) || !WriteRegFile(options.path, std::move(writer), error))
    {
        return false;
    }
    *saved_path = options.path;
    *open_after = options.open_after;
    return true;
}

bool ExportRegFileSelection(HWND owner, const std::wstring& base_key_path, const std::vector<std::wstring>& value_names, const std::vector<std::wstring>& subkey_names, std::wstring* error, std::wstring* saved_path, win32::OpenAfter* open_after)
{
    if (value_names.empty() && subkey_names.empty())
    {
        if (error)
        {
            *error = util::Tr(L"No data to export.");
        }
        return false;
    }
    const std::wstring first_name =
        !value_names.empty() ? (value_names.front().empty() ? L"Default" : value_names.front()) : subkey_names.front();
    std::wstring path;
    if (!ui::ReportFileDialogResult(
            owner,
            win32::ChooseFileToSave(owner, kRegFileFilter, util::EnsureFileExtension(SanitizeFileName(first_name), L".reg").c_str(), &path, open_after, true)
        ))
    {
        return false;
    }
    path = util::EnsureFileExtension(path, L".reg");

    regfile::Writer writer;
    std::vector<std::wstring> skipped;
    if (!value_names.empty())
    {
        RegistryNode base;
        std::wstring display;
        if (!ResolveExportKey(base_key_path, &base, &display, error))
        {
            return false;
        }
        registry_backend::KeyContents contents;
        const LONG status = registry_backend::ReadKeyContents(base.root, base.subkey, win32::kDefaultRegistryView, true, &contents);
        if (status != ERROR_SUCCESS)
        {
            return ReportUnreadableKey(status, display, error);
        }
        std::erase_if(contents.values, [&](const RegistryValue& value) {
            return std::none_of(value_names.begin(), value_names.end(), [&](const std::wstring& name) { return util::EqualsInsensitive(name, value.name); });
        });
        const bool found = !contents.values.empty();
        regfile::SkipNullNames(display, &contents.values, nullptr, &skipped);
        std::vector<const regfile::Value*> selected;
        for (const RegistryValue& value : contents.values)
        {
            selected.push_back(&value);
        }
        if (!found)
        {
            if (error)
            {
                *error = util::Tr(L"No selected values were found in the export.");
            }
            return false;
        }
        writer.AppendKey(display, std::move(selected), false);
    }
    for (const auto& subkey : subkey_names)
    {
        if (subkey.empty())
        {
            continue;
        }
        RegistryNode node;
        std::wstring display;
        if (!ResolveExportKey(base_key_path.empty() ? subkey : base_key_path + L"\\" + subkey, &node, &display, error))
        {
            return false;
        }
        const LONG status = regfile::AppendRegistryTree(&writer, node.root, node.subkey, display, win32::kDefaultRegistryView, true, &skipped);
        if (status != ERROR_SUCCESS)
        {
            return ReportUnreadableKey(status, display, error);
        }
    }
    if (!skipped.empty() && !ui::ConfirmConversionSkips(owner, skipped, util::Tr(L"Export")))
    {
        return false;
    }
    *saved_path = path;
    return WriteRegFile(path, std::move(writer), error);
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
bool LoadHive(HWND owner, HKEY* root, std::wstring* error)
{
    if (!root)
    {
        return false;
    }
    editors::LoadHiveResult choice;
    choice.root = *root;
    if (!editors::ChooseHiveToLoad(owner, &choice))
    {
        return false;
    }
    *root = choice.root;
    const util::PrivilegeScope privileges({SE_RESTORE_NAME, SE_BACKUP_NAME});
    if (!privileges.held())
    {
        if (error)
        {
            *error = util::Tr(L"Loading a hive needs the backup and restore privileges. Run RegKit elevated.");
        }
        return false;
    }
    const LONG result = RegLoadKeyW(*root, choice.key_name.c_str(), choice.file.c_str());
    if (result != ERROR_SUCCESS)
    {
        if (error)
        {
            *error = FormatWin32Error(result);
        }
        return false;
    }
    return true;
}

bool UnloadHive(HWND owner, HKEY root, const std::wstring& subkey, std::wstring* error)
{
    std::wstring target = subkey;
    if (target.empty())
    {
        editors::TextRequest request;
        request.title = util::Tr(L"Unload Hive");
        request.label = util::Tr(L"Key name:");
        request.text = target;
        editors::TextResult result;
        if (!editors::EditText(owner, request, &result))
        {
            return false;
        }
        target = std::move(result.text);
    }
    if (target.empty())
    {
        if (error)
        {
            *error = util::Tr(L"Key name is required.");
        }
        return false;
    }
    const util::PrivilegeScope privileges({SE_RESTORE_NAME, SE_BACKUP_NAME});
    if (!privileges.held())
    {
        if (error)
        {
            *error = util::Tr(L"Unloading a hive needs the backup and restore privileges. Run RegKit elevated.");
        }
        return false;
    }
    const LONG result = RegUnLoadKeyW(root, target.c_str());
    if (result != ERROR_SUCCESS)
    {
        if (error)
        {
            *error = FormatWin32Error(result);
        }
        return false;
    }
    return true;
}

} // namespace regkit
