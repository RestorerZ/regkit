// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "workspace/favorites.h"

#include "win32/file_text.h"
#include "win32/handle_owner.h"
#include "win32/shell_paths.h"
#include "win32/system_error.h"
#include "win32/text_transform.h"

#include <algorithm>

namespace regkit::workspace {

namespace {

bool LoadFromFile(
    const std::wstring& path,
    std::vector<std::wstring>* favorites
) {
  std::wstring content;
  const bool loaded = !path.empty() && util::ReadTextFile(path, &content);
  *favorites = util::SplitLines(content);
  return loaded;
}

bool SaveToFile(
    const std::wstring& path,
    const std::vector<std::wstring>& favorites
) {
  return !path.empty() && util::WriteTextFile(path, util::JoinLines(favorites), false);
}

size_t MergeUnique(
    std::vector<std::wstring>* favorites,
    const std::vector<std::wstring>& additions
) {
  const size_t before = favorites->size();
  for (const std::wstring& entry : additions) {
    const bool present = std::any_of(favorites->begin(), favorites->end(), [&](const std::wstring& existing) { return util::EqualsInsensitive(existing, entry); });
    if (!entry.empty() && !present) {
      favorites->push_back(entry);
    }
  }
  return favorites->size() - before;
}

} // namespace

std::wstring FavoritesStore::FavoritesPath() {
  const std::wstring folder = util::GetAppDataFolder();
  return folder.empty() ? std::wstring() : util::JoinPath(folder, L"favorites.txt");
}

bool FavoritesStore::Load(
    std::vector<std::wstring>* favorites
) {
  const std::wstring path = FavoritesPath();
  LoadFromFile(path, favorites);
  return !path.empty();
}

bool FavoritesStore::Save(
    const std::vector<std::wstring>& favorites
) {
  return SaveToFile(FavoritesPath(), favorites);
}

bool FavoritesStore::Add(
    const std::wstring& path
) {
  std::vector<std::wstring> favorites;
  Load(&favorites);
  return !path.empty() && (MergeUnique(&favorites, {path}) == 0 || Save(favorites));
}

bool FavoritesStore::Remove(
    const std::wstring& path
) {
  std::vector<std::wstring> favorites;
  Load(&favorites);
  const size_t removed = std::erase_if(favorites, [&](const std::wstring& entry) { return util::EqualsInsensitive(entry, path); });
  return !path.empty() && (removed == 0 || Save(favorites));
}

bool FavoritesStore::ImportFromFile(
    const std::wstring& path
) {
  std::vector<std::wstring> imported;
  if (!LoadFromFile(path, &imported)) {
    return false;
  }
  std::vector<std::wstring> favorites;
  Load(&favorites);
  return MergeUnique(&favorites, imported) == 0 || Save(favorites);
}

bool FavoritesStore::ExportToFile(
    const std::wstring& path
) {
  std::vector<std::wstring> favorites;
  Load(&favorites);
  return SaveToFile(path, favorites);
}

bool FavoritesStore::ImportFromRegEdit(
    size_t* imported_count,
    std::wstring* error
) {
  if (imported_count) {
    *imported_count = 0;
  }
  std::vector<NamedFavorite> named;
  if (!LoadRegEdit(&named, error)) {
    return false;
  }
  std::vector<std::wstring> imported;
  imported.reserve(named.size());
  for (auto& favorite : named) {
    imported.push_back(std::move(favorite.path));
  }
  std::vector<std::wstring> favorites;
  Load(&favorites);
  const size_t added = MergeUnique(&favorites, imported);
  if (added != 0 && !Save(favorites)) {
    if (error) {
      *error = L"Failed to save favorites.";
    }
    return false;
  }
  if (imported_count) {
    *imported_count = added;
  }
  return true;
}

bool FavoritesStore::LoadRegEdit(
    std::vector<NamedFavorite>* favorites,
    std::wstring* error
) {
  favorites->clear();
  if (error) {
    error->clear();
  }
  util::UniqueHKey key;
  LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\RegEdit\\Favorites", 0, KEY_READ, key.put());
  DWORD value_count = 0;
  DWORD max_name = 0;
  DWORD max_data = 0;
  if (result == ERROR_SUCCESS) {
    result = RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &value_count, &max_name, &max_data, nullptr, nullptr);
  }
  if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) {
    return true;
  }
  if (result != ERROR_SUCCESS) {
    if (error) {
      *error = util::FormatWin32Error(result);
    }
    return false;
  }
  std::wstring name(max_name + 1, L'\0');
  std::vector<BYTE> data(max_data + sizeof(wchar_t));
  for (DWORD index = 0; index < value_count; ++index) {
    DWORD name_length = static_cast<DWORD>(name.size());
    DWORD data_length = static_cast<DWORD>(data.size());
    DWORD type = 0;
    if (RegEnumValueW(key.get(), index, name.data(), &name_length, nullptr, &type, data.data(), &data_length) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
      continue;
    }
    std::wstring value(reinterpret_cast<const wchar_t*>(data.data()), data_length / sizeof(wchar_t));
    value.resize(wcsnlen_s(value.c_str(), value.size()));
    if (type == REG_EXPAND_SZ) {
      std::wstring expanded = util::ExpandEnvironmentStringsDynamic(value);
      if (!expanded.empty()) {
        value = std::move(expanded);
      }
    }
    if (!value.empty()) {
      favorites->push_back({std::wstring(name.data(), name_length), std::move(value)});
    }
  }
  return true;
}

} // namespace regkit::workspace