// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/reg_command.h"

#include "regfile/reg_exe_syntax.h"
#include "regfile/reg_file.h"
#include "regfile/script_convert.h"
#include "regfile/registry_transfer.h"
#include "registry/key_algorithms.h"
#include "registry/registry_path.h"
#include "registry/value_format.h"
#include "win32/file_text.h"
#include "win32/handle_owner.h"
#include "win32/process_rights.h"
#include "win32/registry_native.h"
#include "win32/registry_view.h"
#include "win32/system_error.h"
#include "win32/text_transform.h"

#include <algorithm>
#include <cstdio>

namespace regkit::cli
{

namespace
{

constexpr int kOk = 0;
constexpr int kFailed = 1;

bool g_console_ready = false;
HANDLE g_out = nullptr;
HANDLE g_err = nullptr;

void EnsureConsole()
{
    if (g_console_ready)
    {
        return;
    }
    g_console_ready = true;
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_err = GetStdHandle(STD_ERROR_HANDLE);
    const bool have_out = g_out && g_out != INVALID_HANDLE_VALUE;
    if (!have_out && !AttachConsole(ATTACH_PARENT_PROCESS))
    {
        AllocConsole();
    }
    if (!have_out)
    {
        g_out = GetStdHandle(STD_OUTPUT_HANDLE);
        g_err = GetStdHandle(STD_ERROR_HANDLE);
    }
    if (!g_err || g_err == INVALID_HANDLE_VALUE)
    {
        g_err = g_out;
    }
}

void WriteTo(HANDLE handle, const std::wstring& text)
{
    if (!handle || handle == INVALID_HANDLE_VALUE || text.empty())
    {
        return;
    }
    DWORD written = 0;
    if (GetFileType(handle) == FILE_TYPE_CHAR)
    {
        WriteConsoleW(handle, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    const std::string utf8 = util::WideToUtf8(text);
    WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void Print(const std::wstring& text)
{
    EnsureConsole();
    WriteTo(g_out, text + L"\r\n");
}

void PrintError(const std::wstring& text)
{
    EnsureConsole();
    const bool prefixed = text.rfind(L"ERROR: ", 0) == 0;
    WriteTo(g_err, (prefixed ? text : L"ERROR: " + text) + L"\r\n");
}

int Fail(LONG status)
{
    PrintError(status == ERROR_FILE_NOT_FOUND ? L"The system was unable to find the specified registry key or value." : util::FormatWin32Error(static_cast<DWORD>(status)));
    return kFailed;
}

struct KeyRef
{
    HKEY root = nullptr;
    std::wstring subkey;
    std::wstring display;
};

// split path into root/subkey & display form
bool ParseKey(const std::wstring& text, KeyRef* key)
{
    std::wstring_view view = text;
    while (!view.empty() && view.front() == L'\\')
    {
        view.remove_prefix(1);
    }
    const size_t split = view.find(L'\\');
    const std::wstring_view root_name = split == std::wstring_view::npos ? view : view.substr(0, split);
    key->root = registry_path::RootFromName(root_name);
    if (!key->root)
    {
        PrintError(L"Invalid key name: " + text);
        return false;
    }
    key->subkey = split == std::wstring_view::npos ? std::wstring() : std::wstring(view.substr(split + 1));
    key->display = registry_path::RootName(key->root);
    if (!key->subkey.empty())
    {
        key->display += L'\\';
        key->display += key->subkey;
    }
    return true;
}

using reg_exe::IsSwitch;
using reg_exe::Options;
using reg_exe::TypeName;

bool ParseOptions(const std::vector<std::wstring>& args, size_t first, Options* options, std::vector<std::wstring>* positional, bool separator_switch = false)
{
    std::wstring error;
    if (!reg_exe::ParseOptions(args, first, options, positional, separator_switch, &error))
    {
        PrintError(error);
        return false;
    }
    return true;
}

std::wstring FormatData(DWORD type, const BYTE* data, DWORD size)
{
    switch (type)
    {
    case REG_SZ:
    case REG_EXPAND_SZ:
    case REG_LINK:
        {
            std::wstring text(reinterpret_cast<const wchar_t*>(data), size / sizeof(wchar_t));
            while (!text.empty() && text.back() == L'\0')
            {
                text.pop_back();
            }
            return text;
        }
    case REG_MULTI_SZ:
        {
            std::wstring joined;
            for (const auto& item : value_format::MultiStringItems({data, size}))
            {
                if (!joined.empty())
                {
                    joined += L"\\0";
                }
                joined += item;
            }
            return joined;
        }
    case REG_DWORD:
    case REG_DWORD_BIG_ENDIAN:
        {
            DWORD value = 0;
            if (size >= sizeof(value))
            {
                memcpy(&value, data, sizeof(value));
            }
            wchar_t buffer[24] = {};
            swprintf_s(buffer, L"0x%x", value);
            return buffer;
        }
    case REG_QWORD:
        {
            unsigned long long value = 0;
            if (size >= sizeof(value))
            {
                memcpy(&value, data, sizeof(value));
            }
            wchar_t buffer[32] = {};
            swprintf_s(buffer, L"0x%llx", value);
            return buffer;
        }
    default:
        return util::ToHex({data, size}, L'\0', true);
    }
}

KeyRef ChildRef(const KeyRef& parent, const std::wstring& name)
{
    KeyRef child = parent;
    child.subkey = registry_path::JoinSubkey(parent.subkey, name);
    child.display = parent.display + L"\\" + name;
    return child;
}

using registry_backend::KeyContents;

LONG ReadKey(const KeyRef& key, REGSAM view, bool include_data, KeyContents* contents)
{
    return registry_backend::ReadKeyContents(key.root, key.subkey, view, include_data, contents);
}

void SelectValue(const Options& options, std::vector<RegistryValue>* values)
{
    if (options.has_value)
    {
        const std::wstring wanted = options.default_value ? std::wstring() : options.value_name;
        std::erase_if(*values, [&](const RegistryValue& value) { return !util::EqualsInsensitive(value.name, wanted); });
    }
}

int QueryKey(const KeyRef& key, const Options& options, bool recurse, bool* matched)
{
    KeyContents contents;
    const LONG status = ReadKey(key, options.view, true, &contents);
    if (status != ERROR_SUCCESS)
    {
        return Fail(status);
    }
    Print(key.display);
    SelectValue(options, &contents.values);
    // keep unnamed default value at the top of the output
    std::stable_partition(contents.values.begin(), contents.values.end(), [](const RegistryValue& value) { return value.name.empty(); });
    for (const RegistryValue& value : contents.values)
    {
        Print(L"    " + (value.name.empty() ? std::wstring(L"(Default)") : value.name) + L"    " + TypeName(value.type) + L"    " + FormatData(value.type, value.data.data(), static_cast<DWORD>(value.data.size())));
    }
    const bool printed = !contents.values.empty();
    if (printed && matched)
    {
        *matched = true;
    }
    if (!options.has_value || printed)
    {
        Print(L"");
    }
    if (!options.has_value && !recurse)
    {
        for (const auto& child : contents.subkeys)
        {
            Print(key.display + L'\\' + child);
        }
        if (!contents.subkeys.empty())
        {
            Print(L"");
        }
    }
    if (recurse)
    {
        for (const auto& child : contents.subkeys)
        {
            QueryKey(ChildRef(key, child), options, true, matched);
        }
    }
    return kOk;
}

int CmdQuery(const std::vector<std::wstring>& args)
{
    Options options;
    std::vector<std::wstring> positional;
    if (!ParseOptions(args, 1, &options, &positional))
    {
        return kFailed;
    }
    if (positional.empty())
    {
        PrintError(L"reg query requires a key name.");
        return kFailed;
    }
    if (positional.size() > 1)
    {
        PrintError(L"Invalid syntax.");
        return kFailed;
    }
    KeyRef key;
    if (!ParseKey(positional[0], &key))
    {
        return kFailed;
    }
    bool matched = false;
    const int result = QueryKey(key, options, options.recurse, &matched);
    // key can exist even when the value requested with /v or /ve doesn't
    if (result == kOk && options.has_value && !matched)
    {
        return Fail(ERROR_FILE_NOT_FOUND);
    }
    return result;
}

int CmdAdd(const std::vector<std::wstring>& args)
{
    Options options;
    std::vector<std::wstring> positional;
    if (!ParseOptions(args, 1, &options, &positional, true))
    {
        return kFailed;
    }
    if (positional.empty())
    {
        PrintError(L"reg add requires a key name.");
        return kFailed;
    }
    if (positional.size() > 1)
    {
        PrintError(L"Invalid syntax.");
        return kFailed;
    }
    KeyRef key;
    if (!ParseKey(positional[0], &key))
    {
        return kFailed;
    }

    DWORD value_type = REG_SZ;
    std::vector<BYTE> value_data;
    if (options.has_value)
    {
        if (!options.type_text.empty() && !reg_exe::ParseType(options.type_text, &value_type))
        {
            PrintError(L"Invalid type: " + options.type_text);
            return kFailed;
        }
        std::wstring error;
        if (!reg_exe::BuildData(value_type, options.has_data ? options.data : std::wstring(), options.separator, &value_data, &error))
        {
            PrintError(error);
            return kFailed;
        }
    }

    util::UniqueHKey handle;
    LONG status = RegCreateKeyExW(key.root, key.subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_QUERY_VALUE | options.view, nullptr, handle.put(), nullptr);
    if (status != ERROR_SUCCESS)
    {
        return Fail(status);
    }
    if (options.has_value)
    {
        const std::wstring name = options.default_value ? std::wstring() : options.value_name;
        if (!options.force &&
            RegQueryValueExW(handle.get(), name.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
        {
            PrintError(L"Value " + (name.empty() ? std::wstring(L"(Default)") : name) + L" already exists. Use /f to overwrite.");
            return kFailed;
        }
        status = RegSetValueExW(handle.get(), name.c_str(), 0, value_type, value_data.data(), static_cast<DWORD>(value_data.size()));
        if (status != ERROR_SUCCESS)
        {
            return Fail(status);
        }
    }
    Print(L"The operation completed successfully.");
    return kOk;
}

int CmdDelete(const std::vector<std::wstring>& args)
{
    Options options;
    std::vector<std::wstring> positional;
    if (!ParseOptions(args, 1, &options, &positional))
    {
        return kFailed;
    }
    if (positional.empty())
    {
        PrintError(L"reg delete requires a key name.");
        return kFailed;
    }
    if (positional.size() > 1)
    {
        PrintError(L"Invalid syntax.");
        return kFailed;
    }
    KeyRef key;
    if (!ParseKey(positional[0], &key))
    {
        return kFailed;
    }

    if (!options.force)
    {
        PrintError(L"This operation deletes registry data. Rerun with /f to confirm.");
        return kFailed;
    }

    if (options.has_value || options.all_values)
    {
        KeyContents contents;
        LONG status = options.all_values ? ReadKey(key, options.view, false, &contents) : ERROR_SUCCESS;
        util::UniqueHKey handle;
        if (status == ERROR_SUCCESS)
        {
            status = RegOpenKeyExW(key.root, key.subkey.c_str(), 0, KEY_SET_VALUE | options.view, handle.put());
        }
        if (!options.all_values)
        {
            contents.values.push_back({options.default_value ? std::wstring() : options.value_name});
        }
        for (size_t index = 0; status == ERROR_SUCCESS && index < contents.values.size(); ++index)
        {
            status = RegDeleteValueW(handle.get(), contents.values[index].name.c_str());
        }
        if (status != ERROR_SUCCESS)
        {
            return Fail(status);
        }
        Print(L"The operation completed successfully.");
        return kOk;
    }

    if (key.subkey.empty())
    {
        PrintError(L"Refusing to delete a registry root.");
        return kFailed;
    }
    util::UniqueHKey target;
    LONG status = util::OpenRegistryPath(
        key.root,
        key.subkey,
        DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | options.view,
        true,
        &target
    );
    if (status == ERROR_SUCCESS)
    {
        status = util::DeleteRegistryTree(target.get());
    }
    if (status != ERROR_SUCCESS)
    {
        return Fail(status);
    }
    Print(L"The operation completed successfully.");
    return kOk;
}

LONG CopyTree(const KeyRef& from, const KeyRef& to, REGSAM view, bool recurse)
{
    KeyContents contents;
    LONG status = ReadKey(from, view, true, &contents);
    util::UniqueHKey target;
    if (status == ERROR_SUCCESS)
    {
        status = RegCreateKeyExW(to.root, to.subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE | view, nullptr, target.put(), nullptr);
    }
    for (size_t index = 0; status == ERROR_SUCCESS && index < contents.values.size(); ++index)
    {
        const RegistryValue& value = contents.values[index];
        status = RegSetValueExW(target.get(), value.name.c_str(), 0, value.type, value.data.data(), static_cast<DWORD>(value.data.size()));
    }
    target.reset();
    for (size_t index = 0; recurse && status == ERROR_SUCCESS && index < contents.subkeys.size(); ++index)
    {
        status = CopyTree(ChildRef(from, contents.subkeys[index]), ChildRef(to, contents.subkeys[index]), view, true);
    }
    return status;
}

int CmdCopy(const std::vector<std::wstring>& args)
{
    Options options;
    std::vector<std::wstring> positional;
    if (!ParseOptions(args, 1, &options, &positional))
    {
        return kFailed;
    }
    if (positional.size() != 2)
    {
        PrintError(positional.size() < 2 ? L"reg copy requires a source and a destination key." : L"Invalid syntax.");
        return kFailed;
    }
    KeyRef from;
    KeyRef to;
    if (!ParseKey(positional[0], &from) || !ParseKey(positional[1], &to))
    {
        return kFailed;
    }
    const LONG status = CopyTree(from, to, options.view, options.recurse);
    if (status != ERROR_SUCCESS)
    {
        return Fail(status);
    }
    Print(L"The operation completed successfully.");
    return kOk;
}

bool ExportKeyToFile(const KeyRef& key, const std::wstring& path, REGSAM view, std::wstring* error)
{
    regfile::Writer writer;
    if (regfile::AppendRegistryTree(&writer, key.root, key.subkey, key.display, view, true) != ERROR_SUCCESS)
    {
        if (error)
        {
            *error = L"The key doesn't exist: " + key.display;
        }
        return false;
    }
    if (!util::WriteTextFile(path, std::move(writer).Finish(), true))
    {
        if (error)
        {
            *error = L"Failed to write " + path;
        }
        return false;
    }
    return true;
}

int CmdExport(const std::vector<std::wstring>& args)
{
    Options options;
    std::vector<std::wstring> positional;
    if (!ParseOptions(args, 1, &options, &positional))
    {
        return kFailed;
    }
    if (positional.size() != 2)
    {
        PrintError(positional.size() < 2 ? L"reg export requires a key name and a file name." : L"Invalid syntax.");
        return kFailed;
    }
    KeyRef key;
    if (!ParseKey(positional[0], &key))
    {
        return kFailed;
    }
    if (!options.force && GetFileAttributesW(positional[1].c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        PrintError(positional[1] + L" already exists. Use /y to overwrite.");
        return kFailed;
    }
    std::wstring error;
    if (!ExportKeyToFile(key, positional[1], options.view, &error))
    {
        PrintError(error.empty() ? L"Export failed." : error);
        return kFailed;
    }
    Print(L"The operation completed successfully.");
    return kOk;
}

int CmdImport(const std::vector<std::wstring>& args)
{
    if (args.size() < 2)
    {
        PrintError(L"reg import requires a file name.");
        return kFailed;
    }
    std::wstring error;
    if (!ImportRegFileFromPath(args[1], &error))
    {
        PrintError(error.empty() ? L"Import failed." : error);
        return kFailed;
    }
    Print(L"The operation completed successfully.");
    return kOk;
}

int CmdSave(const std::vector<std::wstring>& args)
{
    Options options;
    std::vector<std::wstring> positional;
    if (!ParseOptions(args, 1, &options, &positional))
    {
        return kFailed;
    }
    if (positional.size() < 2)
    {
        PrintError(L"reg save requires a key name and a file name.");
        return kFailed;
    }
    KeyRef key;
    if (!ParseKey(positional[0], &key))
    {
        return kFailed;
    }
    if (!options.force && GetFileAttributesW(positional[1].c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        PrintError(positional[1] + L" already exists. Use /y to overwrite.");
        return kFailed;
    }
    const util::PrivilegeScope privileges({SE_BACKUP_NAME});
    util::UniqueHKey handle;
    LONG status = RegOpenKeyExW(key.root, key.subkey.c_str(), 0, KEY_READ | options.view, handle.put());
    if (status != ERROR_SUCCESS)
    {
        return Fail(status);
    }
    // RegSaveKey can't overwrite a file, so stage the save before replacing it
    std::wstring staged = positional[1];
    const bool existed = GetFileAttributesW(positional[1].c_str()) != INVALID_FILE_ATTRIBUTES;
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        if (existed)
        {
            staged = positional[1] + util::RandomFileSuffix(L".part");
        }
        status = RegSaveKeyW(handle.get(), staged.c_str(), nullptr);
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
        return Fail(status);
    }
    if (existed && !MoveFileExW(staged.c_str(), positional[1].c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        const LONG move_error = static_cast<LONG>(GetLastError());
        DeleteFileW(staged.c_str());
        return Fail(move_error);
    }
    Print(L"The operation completed successfully.");
    return kOk;
}

int CmdRestore(const std::vector<std::wstring>& args)
{
    Options options;
    std::vector<std::wstring> positional;
    if (!ParseOptions(args, 1, &options, &positional))
    {
        return kFailed;
    }
    if (positional.size() < 2)
    {
        PrintError(L"reg restore requires a key name and a file name.");
        return kFailed;
    }
    KeyRef key;
    if (!ParseKey(positional[0], &key))
    {
        return kFailed;
    }
    const util::PrivilegeScope privileges({SE_RESTORE_NAME, SE_BACKUP_NAME});
    if (!privileges.held())
    {
        return Fail(ERROR_PRIVILEGE_NOT_HELD);
    }
    util::UniqueHKey handle;
    LONG status = RegOpenKeyExW(key.root, key.subkey.c_str(), 0, KEY_WRITE | options.view, handle.put());
    if (status == ERROR_SUCCESS)
    {
        status = RegRestoreKeyW(handle.get(), positional[1].c_str(), REG_FORCE_RESTORE);
    }
    if (status != ERROR_SUCCESS)
    {
        return Fail(status);
    }
    Print(L"The operation completed successfully.");
    return kOk;
}

int CmdLoad(const std::vector<std::wstring>& args)
{
    if (args.size() < 3)
    {
        PrintError(L"reg load requires a key name and a file name.");
        return kFailed;
    }
    KeyRef key;
    if (!ParseKey(args[1], &key))
    {
        return kFailed;
    }
    const util::PrivilegeScope privileges({SE_RESTORE_NAME, SE_BACKUP_NAME});
    const LONG status = RegLoadKeyW(key.root, key.subkey.c_str(), args[2].c_str());
    if (status != ERROR_SUCCESS)
    {
        return Fail(status);
    }
    Print(L"The operation completed successfully.");
    return kOk;
}

int CmdUnload(const std::vector<std::wstring>& args)
{
    if (args.size() < 2)
    {
        PrintError(L"reg unload requires a key name.");
        return kFailed;
    }
    KeyRef key;
    if (!ParseKey(args[1], &key))
    {
        return kFailed;
    }
    const util::PrivilegeScope privileges({SE_RESTORE_NAME, SE_BACKUP_NAME});
    const LONG status = RegUnLoadKeyW(key.root, key.subkey.c_str());
    if (status != ERROR_SUCCESS)
    {
        return Fail(status);
    }
    Print(L"The operation completed successfully.");
    return kOk;
}

int CompareKeys(const KeyRef& left, const KeyRef& right, const Options& options, bool* differs)
{
    KeyContents left_contents;
    KeyContents right_contents;
    const LONG left_status = ReadKey(left, options.view, true, &left_contents);
    const LONG right_status = ReadKey(right, options.view, true, &right_contents);
    for (const auto& [status, ref] : {std::pair<LONG, const KeyRef*>{left_status, &left}, {right_status, &right}})
    {
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        {
            PrintError(L"The system was unable to open " + ref->display + L".");
            return kFailed;
        }
    }
    const bool left_exists = left_status == ERROR_SUCCESS;
    const bool right_exists = right_status == ERROR_SUCCESS;
    if (!left_exists && !right_exists)
    {
        return kOk;
    }
    if (left_exists != right_exists)
    {
        Print(left_exists ? L"< " + left.display : L"> " + right.display);
        *differs = true;
    }
    SelectValue(options, &left_contents.values);
    SelectValue(options, &right_contents.values);
    auto find = [](const std::vector<RegistryValue>& list, const std::wstring& name) -> const RegistryValue* {
        const auto found = std::find_if(list.begin(), list.end(), [&](const RegistryValue& value) {
            return util::EqualsInsensitive(value.name, name);
        });
        return found == list.end() ? nullptr : &*found;
    };
    for (const RegistryValue& value : left_contents.values)
    {
        const RegistryValue* other = find(right_contents.values, value.name);
        if (!other)
        {
            Print(L"< " + left.display + L"    " + value.name);
            *differs = true;
        }
        else if (other->type != value.type || other->data != value.data)
        {
            Print(L"< " + left.display + L"    " + value.name + L"    " + FormatData(value.type, value.data.data(), static_cast<DWORD>(value.data.size())));
            Print(L"> " + right.display + L"    " + other->name + L"    " + FormatData(other->type, other->data.data(), static_cast<DWORD>(other->data.size())));
            *differs = true;
        }
    }
    for (const RegistryValue& value : right_contents.values)
    {
        if (!find(left_contents.values, value.name))
        {
            Print(L"> " + right.display + L"    " + value.name);
            *differs = true;
        }
    }
    if (!options.recurse)
    {
        return kOk;
    }
    // go through both child lists so keys found on only one side are still compared
    std::vector<std::wstring> children = std::move(left_contents.subkeys);
    children.insert(children.end(), right_contents.subkeys.begin(), right_contents.subkeys.end());
    std::sort(children.begin(), children.end(), [](const std::wstring& a, const std::wstring& b) { return util::CompareInsensitive(a, b) < 0; });
    children.erase(
        std::unique(children.begin(), children.end(), [](const std::wstring& a, const std::wstring& b) { return util::EqualsInsensitive(a, b); }),
        children.end()
    );
    for (const std::wstring& child : children)
    {
        if (CompareKeys(ChildRef(left, child), ChildRef(right, child), options, differs) == kFailed)
        {
            return kFailed;
        }
    }
    return kOk;
}

int CmdCompare(const std::vector<std::wstring>& args)
{
    Options options;
    std::vector<std::wstring> positional;
    if (!ParseOptions(args, 1, &options, &positional))
    {
        return kFailed;
    }
    if (positional.size() < 2)
    {
        PrintError(L"reg compare requires two key names.");
        return kFailed;
    }
    KeyRef left;
    KeyRef right;
    if (!ParseKey(positional[0], &left) || !ParseKey(positional[1], &right))
    {
        return kFailed;
    }

    KeyContents probe;
    if (ReadKey(left, options.view, false, &probe) != ERROR_SUCCESS &&
        ReadKey(right, options.view, false, &probe) != ERROR_SUCCESS)
    {
        PrintError(L"The system was unable to open " + left.display + L".");
        return kFailed;
    }

    bool differs = false;
    if (CompareKeys(left, right, options, &differs) == kFailed)
    {
        return kFailed;
    }
    Print(differs ? L"Result Compared: Different" : L"Result Compared: Identical");
    // reg.exe exit code 2 = keys are different
    return differs ? 2 : kOk;
}

int CmdConvert(const std::vector<std::wstring>& args)
{
    regfile::Format format = regfile::Format::kReg;
    if (args.size() != 3 || !regfile::FormatFromPath(args[2], &format))
    {
        PrintError(L"Usage: convert <input> <output>, both .reg, .bat, .cmd or .ps1.");
        return kFailed;
    }
    std::vector<regfile::Operation> operations;
    std::wstring error;
    if (!regfile::ReadOperations(args[1], &operations, &error))
    {
        PrintError(error);
        return kFailed;
    }
    std::vector<std::wstring> skipped;
    const std::wstring text = regfile::RenderOperations(format, operations, true, &skipped);
    for (const std::wstring& entry : skipped)
    {
        PrintError(L"Skipped " + entry);
    }
    if (!regfile::SaveRendered(args[2], format, text))
    {
        PrintError(L"Failed to write " + args[2]);
        return kFailed;
    }
    Print(L"The operation completed successfully.");
    return skipped.empty() ? kOk : kFailed;
}

void PrintUsage()
{
    Print(L"RegKit command line\n"
          L"\n"
          L"regedit compatible:\n"
          L"  regkit file.reg                 import a .reg file (asks first)\n"
          L"  regkit /s file.reg              import without prompting\n"
          L"  regkit /e file.reg <key>        export a key\n"
          L"  regkit /a file.reg <key>        same as /e, kept for compatibility\n"
          L"  regkit /c /m /l:file /r:file    accepted and ignored (legacy)\n"
          L"\n"
          L"reg.exe compatible (the leading \"reg\" is optional):\n"
          L"  add <key> [/v name | /ve] [/t type] [/s sep] [/d data] [/f]\n"
          L"  delete <key> [/v name | /ve | /va] [/f]\n"
          L"  query <key> [/v name | /ve] [/s]\n"
          L"  copy <src> <dst> [/s] [/f]\n"
          L"  export <key> <file.reg> [/y]\n"
          L"  import <file.reg>\n"
          L"  save <key> <file.hiv> [/y]      restore <key> <file.hiv>\n"
          L"  load <key> <file.hiv>           unload <key>\n"
          L"  compare <key1> <key2> [/s]\n"
          L"  /reg:32 | /reg:64               pick the registry view\n"
          L"\n"
          L"RegKit additions:\n"
          L"  regkit <key>                    open the window at that key\n"
          L"  regkit --goto <key>             same, explicit form\n"
          L"  regkit --edit-reg file.reg      open a .reg file in a tab\n"
          L"  regkit convert <in> <out>       convert between .reg, .bat, .cmd and .ps1\n"
          L"  regkit --install-edit-context-menu\n"
          L"                                    add the Edit with RegKit context menu\n"
          L"  regkit --uninstall-edit-context-menu\n"
          L"                                    remove this executable's context menu\n"
          L"  regkit --install-regedit-replacement [--override]\n"
          L"                                    replace RegEdit with this executable\n"
          L"                                    fails if another program owns RegEdits\n"
          L"                                    Debugger entry, --override replaces it anyway\n"
          L"  regkit --uninstall-regedit-replacement\n"
          L"                                    remove this executable's replacement\n"
          L"  regkit --restart-system         relaunch as SYSTEM\n"
          L"  regkit --restart-ti             relaunch as TrustedInstaller\n"
          L"  regkit --help                   show this text\n"
          L"\n"
          L"Key names accept HKLM, HKCU, HKCR, HKU, HKCC and their full forms.\n"
          L"reg flags isn't implemented, every other verb above is.");
}

int RunVerb(const std::wstring& verb, const std::vector<std::wstring>& args)
{
    struct Verb
    {
        const wchar_t* name;
        int (*run)(const std::vector<std::wstring>&);
    };
    static constexpr Verb kVerbs[] = {
        {L"query", CmdQuery},
        {L"add", CmdAdd},
        {L"delete", CmdDelete},
        {L"copy", CmdCopy},
        {L"export", CmdExport},
        {L"import", CmdImport},
        {L"save", CmdSave},
        {L"restore", CmdRestore},
        {L"load", CmdLoad},
        {L"unload", CmdUnload},
        {L"compare", CmdCompare},
        {L"convert", CmdConvert},
    };
    for (const Verb& entry : kVerbs)
    {
        if (util::EqualsInsensitive(verb, entry.name))
        {
            return entry.run(args);
        }
    }
    if (util::EqualsInsensitive(verb, L"flags"))
    {
        PrintError(L"reg flags isn't implemented.");
        return kFailed;
    }
    return -1;
}
} // namespace

bool Execute(const std::vector<std::wstring>& args, int* exit_code)
{
    if (args.empty() || !exit_code)
    {
        return false;
    }

    for (const std::wstring& arg : args)
    {
        if (IsSwitch(arg, L"?") || IsSwitch(arg, L"h") || IsSwitch(arg, L"-help") || IsSwitch(arg, L"help"))
        {
            PrintUsage();
            *exit_code = kOk;
            return true;
        }
    }

    std::vector<std::wstring> verb_args = args;
    if (util::EqualsInsensitive(verb_args[0], L"reg"))
    {
        verb_args.erase(verb_args.begin());
    }
    if (!verb_args.empty())
    {
        const int result = RunVerb(verb_args[0], verb_args);
        if (result >= 0)
        {
            *exit_code = result;
            return true;
        }
    }

    // accept older RegEdit import/export
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (IsSwitch(args[i], L"s") && i + 1 < args.size())
        {
            std::wstring error;
            if (!ImportRegFileFromPath(args[i + 1], &error))
            {
                PrintError(error.empty() ? L"Import failed." : error);
                *exit_code = kFailed;
            }
            else
            {
                *exit_code = kOk;
            }
            return true;
        }
        if ((IsSwitch(args[i], L"e") || IsSwitch(args[i], L"a")) && i + 1 < args.size())
        {
            KeyRef key;
            std::wstring error;
            *exit_code = kFailed;
            if (i + 2 >= args.size())
            {
                PrintError(L"Exporting the whole registry isn't supported, name a key.");
            }
            else if (ParseKey(args[i + 2], &key))
            {
                if (ExportKeyToFile(key, args[i + 1], win32::kDefaultRegistryView, &error))
                {
                    *exit_code = kOk;
                }
                else
                {
                    PrintError(error.empty() ? L"Export failed." : error);
                }
            }
            return true;
        }
    }
    return false;
}

} // namespace regkit::cli
