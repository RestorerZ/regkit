// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "registry/registry_path.h"

#include "registry/registry_store.h"
#include "win32/process_rights.h"
#include "win32/text_transform.h"

#include <algorithm>
#include <array>
#include <cwctype>

namespace regkit::registry_path
{
namespace
{

struct RootNames
{
    HKEY root;
    const wchar_t* name;
    const wchar_t* abbreviation;
};

// keep normal path parsing limited to five standard roots
constexpr size_t kBrowsableRoots = 5;

const std::array<RootNames, 8>& Roots()
{
    static const std::array<RootNames, 8> roots = {{
        {HKEY_CLASSES_ROOT, L"HKEY_CLASSES_ROOT", L"HKCR"},
        {HKEY_CURRENT_USER, L"HKEY_CURRENT_USER", L"HKCU"},
        {HKEY_LOCAL_MACHINE, L"HKEY_LOCAL_MACHINE", L"HKLM"},
        {HKEY_USERS, L"HKEY_USERS", L"HKU"},
        {HKEY_CURRENT_CONFIG, L"HKEY_CURRENT_CONFIG", L"HKCC"},
        {HKEY_PERFORMANCE_DATA, L"HKEY_PERFORMANCE_DATA", L"HKPD"},
        {HKEY_PERFORMANCE_TEXT, L"HKEY_PERFORMANCE_TEXT", L""},
        {HKEY_PERFORMANCE_NLSTEXT, L"HKEY_PERFORMANCE_NLSTEXT", L""},
    }};
    return roots;
}

const RootNames* FindRoot(std::wstring_view name, size_t count)
{
    for (size_t index = 0; index < count && !name.empty(); ++index)
    {
        const RootNames& entry = Roots()[index];
        if (util::EqualsInsensitive(name, entry.name) || util::EqualsInsensitive(name, entry.abbreviation))
        {
            return &entry;
        }
    }
    return nullptr;
}

std::wstring CanonicalRoot(std::wstring_view root)
{
    if (util::EqualsInsensitive(root, L"MACHINE"))
    {
        return L"HKEY_LOCAL_MACHINE";
    }
    if (util::EqualsInsensitive(root, L"USER") || util::EqualsInsensitive(root, L"USERS"))
    {
        return L"HKEY_USERS";
    }
    if (util::EqualsInsensitive(root, L"REGISTRY"))
    {
        return L"REGISTRY";
    }
    const RootNames* entry = FindRoot(root, kBrowsableRoots);
    return entry ? entry->name : L"";
}

std::wstring AbbreviatedRoot(std::wstring_view root)
{
    const RootNames* entry = FindRoot(root, kBrowsableRoots);
    return entry ? entry->abbreviation : std::wstring(root);
}
std::wstring Join(std::wstring_view root, std::wstring_view rest)
{
    if (rest.empty())
    {
        return std::wstring(root);
    }
    std::wstring result(root);
    result.push_back(L'\\');
    result.append(rest);
    return result;
}

bool HasComponentPrefix(std::wstring_view path, std::wstring_view prefix)
{
    return util::StartsWithInsensitive(path, prefix) && (path.size() == prefix.size() || path[prefix.size()] == L'\\');
}

std::wstring JoinRange(const std::vector<std::wstring>& parts, size_t first, size_t last)
{
    size_t characters = 0;
    for (size_t index = first; index < last; ++index)
    {
        characters += parts[index].size() + 1;
    }
    std::wstring result;
    result.reserve(characters);
    for (size_t index = first; index < last; ++index)
    {
        if (parts[index].empty())
        {
            continue;
        }
        if (!result.empty())
        {
            result.push_back(L'\\');
        }
        result += parts[index];
    }
    return result;
}

} // namespace

std::wstring DisplayName(std::wstring_view name)
{
    // show embedded nulls without truncating text in the UI
    std::wstring text(name);
    std::replace(text.begin(), text.end(), wchar_t(0), kNullSymbol);
    return text;
}

std::wstring RawName(std::wstring_view text)
{
    // restore embedded nulls before native registry operations
    std::wstring name(text);
    std::replace(name.begin(), name.end(), kNullSymbol, wchar_t(0));
    return name;
}

HKEY RootFromName(std::wstring_view name)
{
    const RootNames* entry = FindRoot(name, Roots().size());
    return entry ? entry->root : nullptr;
}

std::wstring RootName(HKEY root)
{
    std::wstring virtual_name;
    if (RegistryStore::GetVirtualRootName(root, &virtual_name))
    {
        return virtual_name;
    }
    for (const RootNames& entry : Roots())
    {
        if (entry.root == root)
        {
            return entry.name;
        }
    }
    return {};
}
std::wstring Build(const RegistryNode& node)
{
    const std::wstring root = node.root_name.empty() ? RootName(node.root) : node.root_name;
    return DisplayName(Join(root, node.subkey));
}

std::wstring BuildNative(const RegistryNode& node)
{
    if (RegistryStore::IsVirtualRoot(node.root))
    {
        return {};
    }
    if (util::EqualsInsensitive(node.root_name, L"REGISTRY"))
    {
        return Join(L"\\REGISTRY", node.subkey);
    }

    std::wstring root;
    if (node.root == HKEY_LOCAL_MACHINE)
    {
        root = L"\\REGISTRY\\MACHINE";
    }
    else if (node.root == HKEY_USERS)
    {
        root = L"\\REGISTRY\\USER";
    }
    else if (node.root == HKEY_CURRENT_USER)
    {
        // HKCU is stored below the current users SID in native namespace
        const std::wstring sid = util::GetCurrentUserSidString();
        if (sid.empty())
        {
            return {};
        }
        root = L"\\REGISTRY\\USER\\" + sid;
    }
    else if (node.root == HKEY_CURRENT_CONFIG)
    {
        // HKCC points to active hardware profile
        root = L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current";
    }
    else
    {
        return {};
    }
    return Join(root, node.subkey);
}

std::vector<std::wstring> Split(std::wstring_view path)
{
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start < path.size())
    {
        while (start < path.size() && (path[start] == L'\\' || path[start] == L'/'))
        {
            ++start;
        }
        if (start == path.size())
        {
            break;
        }
        size_t end = path.find_first_of(L"\\/", start);
        if (end == std::wstring_view::npos)
        {
            end = path.size();
        }
        parts.emplace_back(path.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::wstring Join(const std::vector<std::wstring>& parts, size_t first_part)
{
    return JoinRange(parts, std::min(first_part, parts.size()), parts.size());
}

std::wstring JoinPrefix(const std::vector<std::wstring>& parts, size_t part_count)
{
    return JoinRange(parts, 0, std::min(part_count, parts.size()));
}

std::wstring JoinSubkey(std::wstring_view parent, std::wstring_view name)
{
    return parent.empty() ? std::wstring(name) : Join(parent, name);
}

RegistryNode ChildNode(const RegistryNode& parent, std::wstring_view name)
{
    RegistryNode child;
    child.root = parent.root;
    child.root_name = parent.root_name;
    child.subkey = JoinSubkey(parent.subkey, name);
    child.simulated = parent.simulated;
    return child;
}

std::wstring Parent(std::wstring_view path)
{
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
    {
        path.remove_suffix(1);
    }
    const size_t split = path.find_last_of(L"\\/");
    return split == std::wstring_view::npos ? std::wstring{} : std::wstring(path.substr(0, split));
}

std::wstring Leaf(std::wstring_view path)
{
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
    {
        path.remove_suffix(1);
    }
    const size_t split = path.find_last_of(L"\\/");
    return std::wstring(path.substr(split == std::wstring_view::npos ? 0 : split + 1));
}

std::wstring Clean(std::wstring_view input)
{
    std::wstring path = util::TrimWhitespace(input);
    if (path.size() >= 2 &&
        ((path.front() == L'[' && path.back() == L']') || (path.front() == L'"' && path.back() == L'"') ||
         (path.front() == L'\'' && path.back() == L'\'')))
    {
        path = util::TrimWhitespace(std::wstring_view(path).substr(1, path.size() - 2));
    }
    if (!path.empty() && path.front() == L'-')
    {
        path = util::TrimWhitespace(std::wstring_view(path).substr(1));
    }
    for (wchar_t& character : path)
    {
        if (character == L'/')
        {
            character = L'\\';
        }
    }

    std::wstring collapsed;
    collapsed.reserve(path.size());
    for (wchar_t character : path)
    {
        if (character != L'\\' || collapsed.empty() || collapsed.back() != L'\\')
        {
            collapsed.push_back(character);
        }
    }
    path = std::move(collapsed);

    if (util::StartsWithInsensitive(path, L"reg:"))
    {
        path = util::TrimWhitespace(std::wstring_view(path).substr(4));
    }
    if (util::StartsWithInsensitive(path, L"Registry::"))
    {
        path.erase(0, 10);
    }
    while (!path.empty() && path.front() == L'\\')
    {
        path.erase(path.begin());
    }
    if (util::StartsWithInsensitive(path, L"My Computer\\"))
    {
        path.erase(0, 12);
    }
    else if (util::StartsWithInsensitive(path, L"Computer\\"))
    {
        path.erase(0, 9);
    }
    return path;
}

std::wstring Normalize(std::wstring_view input, std::wstring_view current_user_sid)
{
    std::wstring path = Clean(input);

    if (util::StartsWithInsensitive(path, L"REGISTRY\\"))
    {
        std::wstring native = path.substr(9);
        auto native_rest = [&](std::wstring_view prefix) {
            std::wstring_view rest(native);
            rest.remove_prefix(prefix.size());
            while (!rest.empty() && rest.front() == L'\\')
            {
                rest.remove_prefix(1);
            }
            return rest;
        };
        constexpr std::wstring_view classes = L"MACHINE\\SOFTWARE\\Classes";
        if (HasComponentPrefix(native, classes))
        {
            return Join(L"HKEY_CLASSES_ROOT", native_rest(classes));
        }
        constexpr std::wstring_view current_config = L"MACHINE\\SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current";
        if (HasComponentPrefix(native, current_config))
        {
            return Join(L"HKEY_CURRENT_CONFIG", native_rest(current_config));
        }
        if (HasComponentPrefix(native, L"MACHINE"))
        {
            std::wstring_view rest(native);
            rest.remove_prefix(7);
            while (!rest.empty() && rest.front() == L'\\')
            {
                rest.remove_prefix(1);
            }
            return Join(L"HKEY_LOCAL_MACHINE", rest);
        }
        if (HasComponentPrefix(native, L"USER"))
        {
            std::wstring_view rest(native);
            rest.remove_prefix(4);
            while (!rest.empty() && rest.front() == L'\\')
            {
                rest.remove_prefix(1);
            }
            if (!current_user_sid.empty() && HasComponentPrefix(rest, current_user_sid))
            {
                rest.remove_prefix(current_user_sid.size());
                while (!rest.empty() && rest.front() == L'\\')
                {
                    rest.remove_prefix(1);
                }
                return Join(L"HKEY_CURRENT_USER", rest);
            }
            return Join(L"HKEY_USERS", rest);
        }
        const size_t native_split = native.find(L'\\');
        const std::wstring_view native_root = native_split == std::wstring::npos
                                                  ? std::wstring_view(native)
                                                  : std::wstring_view(native).substr(0, native_split);
        const std::wstring canonical = CanonicalRoot(native_root);
        if (!canonical.empty() && !util::EqualsInsensitive(canonical, L"REGISTRY"))
        {
            const std::wstring_view rest = native_split == std::wstring::npos
                                               ? std::wstring_view{}
                                               : std::wstring_view(native).substr(native_split + 1);
            return Join(canonical, rest);
        }
    }

    const size_t split = path.find_first_of(L":\\");
    const std::wstring_view root(path.data(), split == std::wstring::npos ? path.size() : split);
    std::wstring_view rest = split == std::wstring::npos
                                 ? std::wstring_view{}
                                 : std::wstring_view(path).substr(split + (path[split] == L':' ? 1 : 0));
    while (!rest.empty() && rest.front() == L'\\')
    {
        rest.remove_prefix(1);
    }
    const std::wstring canonical = CanonicalRoot(root);
    return canonical.empty() ? path : Join(canonical, rest);
}

std::wstring Format(std::wstring_view path, Style style, std::wstring_view tree_root)
{
    const size_t split = path.find(L'\\');
    const std::wstring_view root = split == std::wstring_view::npos ? path : path.substr(0, split);
    const std::wstring_view rest = split == std::wstring_view::npos ? std::wstring_view{} : path.substr(split + 1);
    switch (style)
    {
    case Style::kAbbreviated:
        return Join(AbbreviatedRoot(root), rest);
    case Style::kRegEditAddress:
        return Join(tree_root.empty() ? L"Computer" : tree_root, path);
    case Style::kRegFileHeader:
        return L"[" + std::wstring(path) + L"]";
    case Style::kPowerShellDrive:
        {
            std::wstring result = AbbreviatedRoot(root) + L":";
            if (!rest.empty())
            {
                result += L"\\" + std::wstring(rest);
            }
            return result;
        }
    case Style::kPowerShellProvider:
        return L"Registry::" + std::wstring(path);
    case Style::kEscaped:
        {
            std::wstring result;
            result.reserve(path.size() * 2);
            for (wchar_t character : path)
            {
                if (character == L'\\')
                {
                    result.push_back(L'\\');
                }
                result.push_back(character);
            }
            return result;
        }
    case Style::kFull:
        return std::wstring(path);
    }
    return std::wstring(path);
}

bool ParseRoot(std::wstring_view input, RegistryNode* node)
{
    if (!node)
    {
        return false;
    }
    const std::wstring normalized = Normalize(input);
    const size_t split = normalized.find(L'\\');
    const std::wstring root = normalized.substr(0, split);
    const std::wstring rest = split == std::wstring::npos ? L"" : normalized.substr(split + 1);
    node->subkey = RawName(rest);
    node->root_name = root;
    const RootNames* entry = FindRoot(root, kBrowsableRoots);
    node->root = entry ? entry->root : nullptr;
    // native REGISTRY paths intentionally have no win32 root handle
    return entry || util::EqualsInsensitive(root, L"REGISTRY");
}

} // namespace regkit::registry_path
