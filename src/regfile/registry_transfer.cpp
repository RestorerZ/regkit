// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "regfile/registry_transfer.h"

#include "appearance/feedback.h"
#include "editors/export_dialog.h"
#include "editors/hive_dialog.h"
#include "editors/value_editor.h"
#include "regfile/reg_file.h"
#include "registry/registry_path.h"
#include "registry/registry_store.h"
#include "win32/file_dialog.h"
#include "win32/file_text.h"
#include "win32/handle_owner.h"
#include "win32/process_rights.h"
#include "win32/registry_view.h"
#include "win32/shell_paths.h"
#include "win32/system_error.h"
#include "win32/text_transform.h"

#include <algorithm>
#include <cwchar>
#include <unordered_set>
#include <vector>

#include <shlobj.h>
namespace regkit
{

namespace
{

using util::FormatWin32Error;
using util::ToLower;

constexpr wchar_t kRegFileFilter[] = L"Registry Files (*.reg)\0*.reg\0All Files (*.*)\0*.*\0";

struct TemporaryFile
{
    std::wstring path;
    ~TemporaryFile()
    {
        if (!path.empty())
        {
            DeleteFileW(path.c_str());
        }
    }
};

bool RunRegCommand(const std::wstring& args, std::wstring* error)
{
    wchar_t system_dir[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system_dir, _countof(system_dir));
    if (length == 0 || length >= _countof(system_dir))
    {
        if (error)
        {
            *error = L"The system directory couldn't be resolved.";
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
        std::wstring detail;
        const int chars = output.empty() ? 0
                                         : MultiByteToWideChar(CP_OEMCP, 0, output.data(), static_cast<int>(output.size()), nullptr, 0);
        if (chars > 0)
        {
            detail.resize(static_cast<size_t>(chars));
            MultiByteToWideChar(CP_OEMCP, 0, output.data(), static_cast<int>(output.size()), detail.data(), chars);
            detail = util::TrimWhitespace(detail);
        }
        *error = detail.empty() ? L"reg.exe exited with code " + std::to_wstring(code) + L"." : detail;
    }
    return code == 0;
}

std::wstring NormalizeExportKeyPath(const std::wstring& key_path, std::wstring* error)
{
    const std::wstring path = registry_path::Normalize(key_path, util::GetCurrentUserSidString());
    RegistryNode node;
    if (!registry_path::ParseRoot(path, &node) || !node.root)
    {
        if (error)
        {
            *error = L"Export supports the standard root keys only.";
        }
        return {};
    }
    return path;
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

std::wstring ExportDefaultNameFromKeyPath(const std::wstring& key_path)
{
    const std::wstring file_name = util::EnsureFileExtension(SanitizeFileName(registry_path::Leaf(key_path)), L".reg");
    PWSTR desktop = nullptr;
    std::wstring path = file_name;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
    {
        path = util::JoinPath(desktop, file_name);
    }
    CoTaskMemFree(desktop);
    return path;
}

bool FilterExportedRegFile(const std::wstring& source, const std::wstring& target, std::wstring* error)
{
    std::wstring content;
    bool utf16 = false;
    if (!util::ReadTextFile(source, &content, &utf16))
    {
        if (error)
        {
            *error = L"Failed to read exported registry file.";
        }
        return false;
    }
    std::wstring output;
    output.reserve(content.size());
    bool wrote_section = false;
    for (size_t start = 0; start < content.size();)
    {
        const size_t end = std::min(content.find(L'\n', start), content.size());
        std::wstring_view line(content.data() + start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == L'\r')
        {
            line.remove_suffix(1);
        }
        if (!line.empty() && line.front() == L'[' && line.back() == L']')
        {
            if (wrote_section)
            {
                break;
            }
            wrote_section = true;
        }
        output.append(line).append(L"\r\n");
    }
    if (!util::WriteTextFile(target, output, utf16))
    {
        if (error)
        {
            *error = L"Failed to write exported registry file.";
        }
        return false;
    }
    return true;
}

bool AppendRegContent(const std::wstring& content, regfile::Document* output, std::wstring* error)
{
    regfile::Document parsed;
    if (!regfile::Parse(content, &parsed))
    {
        if (error)
        {
            *error = L"Failed to parse exported registry data.";
        }
        return false;
    }
    for (const auto& path : parsed.key_order)
    {
        const std::wstring lower = ToLower(path);
        auto source = parsed.keys.find(lower);
        if (source == parsed.keys.end())
        {
            continue;
        }
        auto [target, inserted] = output->keys.try_emplace(lower, std::move(source->second));
        if (inserted)
        {
            output->key_order.push_back(path);
            continue;
        }
        for (auto& value : source->second.values)
        {
            target->second.values[value.first] = std::move(value.second);
        }
    }
    return true;
}

bool FilterRegFileValues(const std::wstring& content, const std::vector<std::wstring>& values, regfile::Document* output, std::wstring* error)
{
    if (!regfile::Parse(content, output) || output->key_order.empty())
    {
        if (error)
        {
            *error = L"Failed to parse exported registry data.";
        }
        return false;
    }
    std::unordered_set<std::wstring> wanted;
    for (const auto& value : values)
    {
        wanted.insert(ToLower(value));
    }
    const std::wstring first_path = output->key_order.front();
    const std::wstring first_lower = ToLower(first_path);
    auto first_key = output->keys.find(first_lower);
    if (first_key == output->keys.end())
    {
        return false;
    }
    std::erase_if(first_key->second.values, [&](const auto& value) { return !wanted.contains(value.first); });
    if (first_key->second.values.empty())
    {
        if (error)
        {
            *error = L"No selected values were found in the export.";
        }
        return false;
    }
    regfile::Key selected = std::move(first_key->second);
    output->keys.clear();
    output->key_order.assign(1, first_path);
    output->keys.emplace(first_lower, std::move(selected));
    return true;
}

std::wstring MakeTempRegPath(std::wstring* error)
{
    const std::wstring folder = util::GetCacheFolder();
    if (folder.empty())
    {
        if (error)
        {
            *error = L"Failed to locate the RegKit data folder.";
        }
        return {};
    }
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        std::wstring path = util::JoinPath(folder, L"export" + util::RandomFileSuffix(L".reg"));
        const util::UniqueHandle reserved(
            CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr)
        );
        if (reserved)
        {
            return path;
        }
        if (GetLastError() != ERROR_FILE_EXISTS)
        {
            break;
        }
    }
    if (error)
    {
        *error = FormatWin32Error(GetLastError());
    }
    return {};
}

bool ExportKey(const std::wstring& key_path, bool include_subkeys, const std::wstring& target_path, std::wstring* error)
{
    const std::wstring normalized = NormalizeExportKeyPath(key_path, error);
    TemporaryFile unfiltered;
    if (!include_subkeys)
    {
        unfiltered.path = MakeTempRegPath(error);
    }
    if (normalized.empty() || (!include_subkeys && unfiltered.path.empty()))
    {
        return false;
    }
    const std::wstring& export_path = include_subkeys ? target_path : unfiltered.path;
    const std::wstring args = L"export \"" + normalized + L"\" \"" + export_path + L"\" /y " +
                              win32::RegExeViewSwitch(win32::kDefaultRegistryView);
    return RunRegCommand(args, error) &&
           (include_subkeys || FilterExportedRegFile(unfiltered.path, target_path, error));
}

bool ExportKeyToContent(const std::wstring& key_path, bool include_subkeys, std::wstring* content, bool* utf16, std::wstring* error)
{
    TemporaryFile exported{MakeTempRegPath(error)};
    if (exported.path.empty() || !ExportKey(key_path, include_subkeys, exported.path, error))
    {
        return false;
    }
    if (!util::ReadTextFile(exported.path, content, utf16))
    {
        if (error)
        {
            *error = L"Failed to read exported registry file.";
        }
        return false;
    }
    return true;
}

} // namespace

bool ImportRegFileFromPath(const std::wstring& path, std::wstring* error)
{
    return !path.empty() &&
           RunRegCommand(L"import \"" + path + L"\" " + win32::RegExeViewSwitch(win32::kDefaultRegistryView), error);
}

bool ExportRegFile(HWND owner, const std::wstring& key_path, std::wstring* error, std::wstring* open_after_path)
{
    if (open_after_path)
    {
        open_after_path->clear();
    }
    editors::ExportRequest request;
    request.path = ExportDefaultNameFromKeyPath(key_path);
    editors::ExportResult options;
    if (!editors::ChooseExport(owner, request, &options))
    {
        return false;
    }
    options.path = util::EnsureFileExtension(options.path, L".reg");
    if (!ExportKey(key_path, options.include_subkeys, options.path, error))
    {
        return false;
    }
    if (options.open_after && open_after_path)
    {
        *open_after_path = options.path;
    }
    return true;
}

bool ExportRegFileSelection(HWND owner, const std::wstring& base_key_path, const std::vector<std::wstring>& value_names, const std::vector<std::wstring>& subkey_names, std::wstring* error)
{
    if (value_names.empty() && subkey_names.empty())
    {
        if (error)
        {
            *error = L"No data to export.";
        }
        return false;
    }
    const std::wstring first_name =
        !value_names.empty() ? (value_names.front().empty() ? L"Default" : value_names.front()) : subkey_names.front();
    std::wstring path;
    if (!ui::ReportFileDialogResult(
            owner,
            win32::ChooseFileToSave(owner, kRegFileFilter, util::EnsureFileExtension(SanitizeFileName(first_name), L".reg").c_str(), &path)
        ))
    {
        return false;
    }
    path = util::EnsureFileExtension(path, L".reg");

    regfile::Document output;
    bool output_utf16 = false;
    bool exported_any = false;
    auto export_content = [&](const std::wstring& key_path, bool include_subkeys, std::wstring* content) {
        bool utf16 = false;
        if (!ExportKeyToContent(key_path, include_subkeys, content, &utf16, error))
        {
            return false;
        }
        output_utf16 = exported_any ? output_utf16 : utf16;
        exported_any = true;
        return true;
    };

    std::wstring content;
    if (!value_names.empty() &&
        (!export_content(base_key_path, false, &content) || !FilterRegFileValues(content, value_names, &output, error)))
    {
        return false;
    }
    for (const auto& subkey : subkey_names)
    {
        if (subkey.empty())
        {
            continue;
        }
        const std::wstring key_path = base_key_path.empty() ? subkey : base_key_path + L"\\" + subkey;
        if (!export_content(key_path, true, &content) || !AppendRegContent(content, &output, error))
        {
            return false;
        }
    }
    if (output.key_order.empty())
    {
        if (error)
        {
            *error = L"No data to export.";
        }
        return false;
    }
    if (!util::WriteTextFile(path, regfile::Serialize(output), output_utf16))
    {
        if (error)
        {
            *error = L"Failed to write exported registry file.";
        }
        return false;
    }
    return true;
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
            *error = L"Loading a hive needs the backup and restore privileges. Run RegKit elevated.";
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
        request.title = L"Unload Hive";
        request.label = L"Key name:";
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
            *error = L"Key name is required.";
        }
        return false;
    }
    const util::PrivilegeScope privileges({SE_RESTORE_NAME, SE_BACKUP_NAME});
    if (!privileges.held())
    {
        if (error)
        {
            *error = L"Unloading a hive needs the backup and restore privileges. Run RegKit elevated.";
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
