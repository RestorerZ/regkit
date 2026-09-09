// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace regkit::workspace {

struct PersistedTab {
  enum class Kind {
    kRegistry,
    kSearch,
    kRegFile,
  };

  Kind kind = Kind::kRegistry;
  std::wstring label;
  std::wstring selected_path;
  std::wstring selected_value;
  std::vector<std::wstring> selected_values;
  std::vector<std::wstring> expanded_paths;
  std::wstring search_cache_file;
  std::wstring compare_cache_file;
  std::wstring source_path;
  std::wstring remote_machine;
  int registry_mode = 0;
  int value_top_index = 0;
  bool is_compare = false;
  int first_source_kind = 0;
  std::wstring first_source_file;
  int second_source_kind = 0;
  std::wstring second_source_file;
};

struct TabState {
  static constexpr int kCurrentVersion = 3;
  int source_version = 1;
  int active_index = 0;
  std::vector<PersistedTab> tabs;
};

TabState ParseTabs(const std::wstring& content);
std::wstring SerializeTabs(const TabState& state);
bool LoadTabs(const std::wstring& path, TabState* state);
bool SaveTabs(const std::wstring& path, const TabState& state);

} // namespace regkit::workspace
