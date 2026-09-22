// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "registry/registry_path.h"
#include "registry/registry_store.h"
#include "win32/handle_owner.h"
#include "win32/text_transform.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace regkit::registry_backend
{

inline constexpr SECURITY_INFORMATION kKeySecurityInformation =
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;

class RegistryKeyHandle
{
  public:
    RegistryKeyHandle() = default;
    explicit RegistryKeyHandle(util::UniqueHKey key)
        : key_(std::move(key))
    {
    }

    explicit operator bool() const noexcept
    {
        return static_cast<bool>(key_);
    }
    HKEY get() const noexcept
    {
        return key_.get();
    }

    LONG QueryInfo(DWORD* subkeys, DWORD* max_subkey_length, DWORD* values, DWORD* max_value_name_length, DWORD* max_value_data_length, FILETIME* last_write) const
    {
        return RegQueryInfoKeyW(key_.get(), nullptr, nullptr, nullptr, subkeys, max_subkey_length, nullptr, values, max_value_name_length, max_value_data_length, nullptr, last_write);
    }
    LONG EnumKey(DWORD index, wchar_t* name, DWORD* length) const
    {
        return RegEnumKeyExW(key_.get(), index, name, length, nullptr, nullptr, nullptr, nullptr);
    }
    LONG EnumValue(DWORD index, wchar_t* name, DWORD* name_length, DWORD* type, BYTE* data, DWORD* data_length) const
    {
        return RegEnumValueW(key_.get(), index, name, name_length, nullptr, type, data, data_length);
    }
    LONG GetValue(const wchar_t* name, DWORD* type, BYTE* data, DWORD* size) const
    {
        return RegQueryValueExW(key_.get(), name, nullptr, type, data, size);
    }
    LONG SetValue(const wchar_t* name, DWORD type, const BYTE* data, DWORD size) const
    {
        return RegSetValueExW(key_.get(), name, 0, type, data, size);
    }
    LONG DeleteValue(const wchar_t* name) const
    {
        return RegDeleteValueW(key_.get(), name);
    }
    LONG GetSecurity(SECURITY_INFORMATION information, PSECURITY_DESCRIPTOR descriptor, DWORD* size) const
    {
        return RegGetKeySecurity(key_.get(), information, descriptor, size);
    }
    LONG SetSecurity(SECURITY_INFORMATION information, PSECURITY_DESCRIPTOR descriptor) const
    {
        return RegSetKeySecurity(key_.get(), information, descriptor);
    }

  protected:
    util::UniqueHKey key_;
};

inline const wchar_t* ValueNameArg(const std::wstring& name)
{
    return name.empty() ? nullptr : name.c_str();
}

inline bool SplitNode(const RegistryNode& node, RegistryNode* parent, std::wstring* name)
{
    *name = registry_path::Leaf(node.subkey);
    *parent = node;
    parent->subkey = registry_path::Parent(node.subkey);
    return !name->empty();
}

inline void SortNames(std::vector<std::wstring>* names)
{
    std::sort(names->begin(), names->end(), [](const std::wstring& left, const std::wstring& right) {
        return util::CompareInsensitive(left, right) < 0;
    });
}

template <typename Key>
bool HasSubKeys(const Key& key)
{
    DWORD count = 0;
    return key.QueryInfo(&count, nullptr, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS && count > 0;
}

template <typename Key>
bool QueryKeyInfo(const Key& key, KeyInfo* info)
{
    return key.QueryInfo(&info->subkey_count, nullptr, &info->value_count, nullptr, nullptr, &info->last_write) ==
           ERROR_SUCCESS;
}

template <typename Key>
std::vector<std::wstring> SubKeyNames(const Key& key, bool sorted)
{
    std::vector<std::wstring> names;
    DWORD count = 0;
    DWORD max_length = 0;
    if (key.QueryInfo(&count, &max_length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
    {
        return names;
    }
    names.reserve(count);
    std::wstring buffer(max_length + 1, L'\0');
    for (DWORD index = 0; index < count; ++index)
    {
        DWORD length = static_cast<DWORD>(buffer.size());
        if (key.EnumKey(index, buffer.data(), &length) == ERROR_SUCCESS)
        {
            names.emplace_back(buffer.data(), length);
        }
    }
    if (sorted)
    {
        SortNames(&names);
    }
    return names;
}

template <typename Key>
bool EnumerateKey(const Key& key, bool include_values, bool include_data, bool include_subkeys, RegistryStore::KeyEnumResult* out_info, const RegistryStore::ValueStreamCallback& value_callback, const RegistryStore::SubkeyStreamCallback& subkey_callback, DWORD max_data_size, EnumerationScratch* scratch)
{
    EnumerationScratch local;
    EnumerationScratch& buffers = scratch ? *scratch : local;
    KeyInfo info;
    DWORD max_subkey_length = 0;
    DWORD max_value_name_length = 0;
    DWORD max_value_data_length = 0;
    if (key.QueryInfo(&info.subkey_count, &max_subkey_length, &info.value_count, &max_value_name_length, &max_value_data_length, &info.last_write) != ERROR_SUCCESS)
    {
        return false;
    }
    if (out_info)
    {
        out_info->info = info;
        out_info->info_valid = true;
    }

    if (include_values && value_callback)
    {
        std::wstring& name = buffers.value_name;
        std::vector<BYTE>& data = buffers.value_data;
        name.resize(static_cast<size_t>(max_value_name_length) + 1);
        if (include_data)
        {
            data.resize(std::min(max_value_data_length, max_data_size));
        }
        for (DWORD index = 0; index < info.value_count; ++index)
        {
            DWORD name_length = static_cast<DWORD>(name.size());
            DWORD data_length = include_data ? static_cast<DWORD>(data.size()) : 0;
            DWORD type = 0;
            LONG result = key.EnumValue(index, name.data(), &name_length, &type, include_data && !data.empty() ? data.data() : nullptr, &data_length);
            if (result == ERROR_MORE_DATA && include_data && data_length <= max_data_size)
            {
                data.resize(std::max<size_t>(data.size(), data_length));
                name_length = static_cast<DWORD>(name.size());
                data_length = static_cast<DWORD>(data.size());
                result = key.EnumValue(index, name.data(), &name_length, &type, data.data(), &data_length);
            }
            bool data_available = include_data && result == ERROR_SUCCESS && data_length > 0;
            if (result == ERROR_MORE_DATA)
            {
                name_length = static_cast<DWORD>(name.size());
                data_length = 0;
                result = key.EnumValue(index, name.data(), &name_length, &type, nullptr, &data_length);
            }
            if (result != ERROR_SUCCESS)
            {
                continue;
            }
            ValueInfo value;
            value.name.assign(name.data(), name_length);
            value.type = type;
            value.data_size = data_length;
            if (!value_callback(value, data_available ? data.data() : nullptr, data_length))
            {
                return false;
            }
        }
    }

    if (include_subkeys && subkey_callback)
    {
        std::wstring& name = buffers.subkey_name;
        name.resize(static_cast<size_t>(max_subkey_length) + 1);
        for (DWORD index = 0; index < info.subkey_count; ++index)
        {
            DWORD name_length = static_cast<DWORD>(name.size());
            if (key.EnumKey(index, name.data(), &name_length) == ERROR_SUCCESS &&
                !subkey_callback(std::wstring(name.data(), name_length)))
            {
                return false;
            }
        }
    }
    return true;
}

template <typename Key>
LONG ReadValue(const Key& key, const wchar_t* name, DWORD* type, std::vector<BYTE>* data)
{
    DWORD size = 0;
    LONG result = key.GetValue(name, type, nullptr, &size);
    for (int attempt = 0; attempt < 4 && (result == ERROR_SUCCESS || result == ERROR_MORE_DATA); ++attempt)
    {
        data->resize(size);
        result = key.GetValue(name, type, data->empty() ? nullptr : data->data(), &size);
        if (result == ERROR_SUCCESS)
        {
            data->resize(size);
            return result;
        }
    }
    data->clear();
    return result == ERROR_SUCCESS ? ERROR_MORE_DATA : result;
}

template <typename Key>
bool QueryValue(const Key& key, const std::wstring& value_name, ValueEntry* out)
{
    DWORD type = 0;
    std::vector<BYTE> data;
    if (ReadValue(key, ValueNameArg(value_name), &type, &data) != ERROR_SUCCESS)
    {
        return false;
    }
    out->name = value_name;
    out->type = type;
    out->data = std::move(data);
    return true;
}

template <typename Key>
bool ReadLinkTarget(const Key& key, std::wstring* target)
{
    DWORD type = 0;
    std::vector<BYTE> data;
    if (ReadValue(key, L"SymbolicLinkValue", &type, &data) != ERROR_SUCCESS || type != REG_LINK)
    {
        return false;
    }
    target->assign(reinterpret_cast<const wchar_t*>(data.data()), data.size() / sizeof(wchar_t));
    while (!target->empty() && target->back() == L'\0')
    {
        target->pop_back();
    }
    return true;
}

template <typename Key>
bool RenameValue(const Key& key, const std::wstring& old_name, const std::wstring& new_name, bool* both_names_left)
{
    DWORD type = 0;
    DWORD existing_size = 0;
    std::vector<BYTE> data;
    if (ReadValue(key, ValueNameArg(old_name), &type, &data) != ERROR_SUCCESS ||
        key.GetValue(new_name.c_str(), nullptr, nullptr, &existing_size) != ERROR_FILE_NOT_FOUND ||
        key.SetValue(new_name.c_str(), type, data.empty() ? nullptr : data.data(), static_cast<DWORD>(data.size())) !=
            ERROR_SUCCESS)
    {
        return false;
    }
    if (key.DeleteValue(ValueNameArg(old_name)) != ERROR_SUCCESS)
    {
        if (key.DeleteValue(new_name.c_str()) != ERROR_SUCCESS && both_names_left)
        {
            *both_names_left = true;
        }
        return false;
    }
    return true;
}

template <typename Key>
bool ReadSecurity(const Key& key, std::vector<BYTE>* descriptor)
{
    DWORD size = 0;
    LONG result = key.GetSecurity(kKeySecurityInformation, nullptr, &size);
    if ((result != ERROR_INSUFFICIENT_BUFFER && result != ERROR_MORE_DATA) || size == 0)
    {
        return false;
    }
    descriptor->resize(size);
    result = key.GetSecurity(kKeySecurityInformation, descriptor->data(), &size);
    descriptor->resize(result == ERROR_SUCCESS ? size : 0);
    return result == ERROR_SUCCESS;
}

template <typename Key>
bool WriteSecurity(const Key& key, const std::vector<BYTE>& descriptor)
{
    return !descriptor.empty() &&
           key.SetSecurity(kKeySecurityInformation, const_cast<BYTE*>(descriptor.data())) == ERROR_SUCCESS;
}

} // namespace regkit::registry_backend
