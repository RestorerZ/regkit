// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "registry/hive_files.h"

#include "registry/registry_path.h"
#include "win32/handle_owner.h"
#include "win32/shell_paths.h"
#include "win32/text_transform.h"

#include <cwchar>
#include <unordered_set>

namespace regkit
{

namespace
{

std::wstring ResolveDevicePath(const std::wstring& path)
{
    if (!util::StartsWithInsensitive(path, L"\\Device\\"))
    {
        return path;
    }
    wchar_t drives[512] = {};
    DWORD drive_len = GetLogicalDriveStringsW(static_cast<DWORD>(_countof(drives) - 1), drives);
    if (drive_len == 0 || drive_len >= _countof(drives))
    {
        return path;
    }
    for (const wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1)
    {
        wchar_t device[MAX_PATH] = {};
        wchar_t drive_root[4] = {};
        wcsncpy_s(drive_root, drive, _TRUNCATE);
        size_t root_len = wcslen(drive_root);
        if (root_len >= 2 && drive_root[1] == L':')
        {
            drive_root[2] = L'\0';
        }
        if (!QueryDosDeviceW(drive_root, device, static_cast<DWORD>(_countof(device))))
        {
            continue;
        }
        const size_t device_len = wcslen(device);
        if (util::StartsWithInsensitive(path, device) && (path.size() == device_len || path[device_len] == L'\\'))
        {
            return drive_root + path.substr(device_len);
        }
    }
    return path;
}

void AddOfflineHiveCandidate(std::vector<OfflineHiveCandidate>* out, std::unordered_set<std::wstring>* seen, const std::wstring& path, const std::wstring& label)
{
    if (!out || !seen || !util::IsFile(path))
    {
        return;
    }
    std::wstring key = util::ToLower(path);
    if (!seen->insert(key).second)
    {
        return;
    }
    std::wstring use_label = util::TrimWhitespace(label);
    if (use_label.empty())
    {
        use_label = util::TrimWhitespace(util::FileBaseName(path));
        if (use_label.empty())
        {
            use_label = L"OfflineHive";
        }
    }
    out->push_back({path, use_label});
}

std::wstring TopLevelFolderLabel(const std::wstring& base, const std::wstring& folder)
{
    std::wstring prefix = base;
    if (!prefix.empty() && prefix.back() != L'\\' && prefix.back() != L'/')
    {
        prefix.push_back(L'\\');
    }
    if (util::StartsWithInsensitive(folder, prefix))
    {
        std::wstring relative = folder.substr(prefix.size());
        size_t sep = relative.find_first_of(L"\\/");
        if (sep != std::wstring::npos)
        {
            return relative.substr(0, sep);
        }
        if (!relative.empty())
        {
            return relative;
        }
    }
    return util::FileBaseName(folder);
}

void CollectUserHiveCandidates(const std::wstring& folder, const std::wstring& base, std::vector<OfflineHiveCandidate>* out, std::unordered_set<std::wstring>* seen)
{
    std::wstring label = TopLevelFolderLabel(base, folder);
    std::wstring ntuser = util::JoinPath(folder, L"NTUSER.DAT");
    AddOfflineHiveCandidate(out, seen, ntuser, label);
    std::wstring usrclass = util::JoinPath(folder, L"USRCLASS.DAT");
    std::wstring class_label = label.empty() ? L"" : (label + L"_Classes");
    AddOfflineHiveCandidate(out, seen, usrclass, class_label);
}

void CollectUserHivesRecursive(const std::wstring& folder, const std::wstring& base, std::vector<OfflineHiveCandidate>* out, std::unordered_set<std::wstring>* seen)
{
    WIN32_FIND_DATAW data = {};
    std::wstring search = util::JoinPath(folder, L"*");
    const util::UniqueFind find(FindFirstFileW(search.c_str(), &data));
    if (!find)
    {
        return;
    }
    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            continue;
        }
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
        {
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            continue;
        }
        std::wstring subdir = util::JoinPath(folder, data.cFileName);
        CollectUserHiveCandidates(subdir, base, out, seen);
        CollectUserHivesRecursive(subdir, base, out, seen);
    } while (FindNextFileW(find.get(), &data));
}

bool ShouldIncludeOfflineHiveFile(const std::wstring& name)
{
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos)
    {
        return true;
    }
    std::wstring ext = name.substr(dot);
    return util::EqualsInsensitive(ext, L".dat");
}

void CollectLooseHivesInFolder(const std::wstring& folder, std::vector<OfflineHiveCandidate>* out, std::unordered_set<std::wstring>* seen)
{
    WIN32_FIND_DATAW data = {};
    std::wstring search = util::JoinPath(folder, L"*");
    const util::UniqueFind find(FindFirstFileW(search.c_str(), &data));
    if (!find)
    {
        return;
    }
    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            continue;
        }
        if (!ShouldIncludeOfflineHiveFile(data.cFileName))
        {
            continue;
        }
        std::wstring candidate = util::JoinPath(folder, data.cFileName);
        std::wstring label = util::FileBaseName(data.cFileName);
        AddOfflineHiveCandidate(out, seen, candidate, label);
    } while (FindNextFileW(find.get(), &data));
}

} // namespace

std::wstring NormalizeHiveFilePath(const std::wstring& raw_path)
{
    if (raw_path.empty())
    {
        return raw_path;
    }
    std::wstring path = raw_path;
    if (util::StartsWithInsensitive(path, L"\\??\\") || util::StartsWithInsensitive(path, L"\\\\?\\"))
    {
        path.erase(0, 4);
        if (util::StartsWithInsensitive(path, L"UNC\\"))
        {
            path.replace(0, 3, L"\\");
        }
    }
    else if (util::StartsWithInsensitive(path, L"\\DosDevices\\"))
    {
        path.erase(0, wcslen(L"\\DosDevices\\"));
    }
    if (util::StartsWithInsensitive(path, L"\\SystemRoot") && (path.size() == 11 || path[11] == L'\\'))
    {
        wchar_t windows_dir[MAX_PATH] = {};
        UINT len = GetWindowsDirectoryW(windows_dir, _countof(windows_dir));
        if (len > 0 && len < _countof(windows_dir))
        {
            std::wstring suffix = path.substr(wcslen(L"\\SystemRoot"));
            path = std::wstring(windows_dir) + suffix;
        }
    }
    std::wstring expanded = util::ExpandEnvironmentStringsDynamic(path);
    if (!expanded.empty())
    {
        path = std::move(expanded);
    }
    path = ResolveDevicePath(path);
    return path;
}

void CollectOfflineHivesInFolder(const std::wstring& folder, std::vector<OfflineHiveCandidate>* out)
{
    if (!out)
    {
        return;
    }
    out->clear();
    std::unordered_set<std::wstring> seen;
    static const wchar_t* kMachineHives[] = {
        L"SYSTEM",
        L"SOFTWARE",
        L"SAM",
        L"SECURITY",
        L"DEFAULT",
    };
    for (const auto* name : kMachineHives)
    {
        std::wstring candidate = util::JoinPath(folder, name);
        AddOfflineHiveCandidate(out, &seen, candidate, name);
    }
    CollectUserHiveCandidates(folder, folder, out, &seen);
    CollectLooseHivesInFolder(folder, out, &seen);
    CollectUserHivesRecursive(folder, folder, out, &seen);
}

std::wstring ResolveOfflineRootName(const std::wstring& path, bool is_dir, const RegistryNode* current_node)
{
    std::wstring base = util::FileBaseName(path);
    if (is_dir)
    {
        if (util::EqualsInsensitive(base, L"HKEY_USERS") || util::EqualsInsensitive(base, L"HKU"))
        {
            return L"HKEY_USERS";
        }
        if (util::EqualsInsensitive(base, L"HKEY_LOCAL_MACHINE") || util::EqualsInsensitive(base, L"HKLM"))
        {
            return L"HKEY_LOCAL_MACHINE";
        }
    }
    else
    {
        if (util::EqualsInsensitive(base, L"NTUSER") || util::EqualsInsensitive(base, L"USRCLASS"))
        {
            return L"HKEY_USERS";
        }
        if (util::EqualsInsensitive(base, L"SYSTEM") || util::EqualsInsensitive(base, L"SOFTWARE") ||
            util::EqualsInsensitive(base, L"SAM") || util::EqualsInsensitive(base, L"SECURITY") ||
            util::EqualsInsensitive(base, L"DEFAULT") || util::EqualsInsensitive(base, L"COMPONENTS") ||
            util::EqualsInsensitive(base, L"BCD"))
        {
            return L"HKEY_LOCAL_MACHINE";
        }
    }
    if (current_node && (current_node->root == HKEY_LOCAL_MACHINE || current_node->root == HKEY_USERS))
    {
        std::wstring root_name = registry_path::RootName(current_node->root);
        if (!root_name.empty())
        {
            return root_name;
        }
    }
    return L"HKEY_LOCAL_MACHINE";
}

} // namespace regkit
