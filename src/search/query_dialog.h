// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <string>
#include <vector>

#include "search/search.h"

namespace regkit {

enum class SearchScope {
  kEntireRegistry,
  kCurrentKey,
};

enum class SearchResultMode {
  kReuseTab,
  kNewTab,
};

struct SearchSources {
  bool traces = false;
  bool registry_root = true;
  bool offline = false;
  bool reg_files = false;
  bool remote = false;
  bool extra_hives = false;
};

struct SearchDialogResult {
  search::Criteria criteria;
  std::wstring start_key;
  std::vector<std::wstring> root_paths;
  bool search_standard_hives = true;
  bool search_registry_root = true;
  bool search_trace_values = true;
  bool search_offline_hives = false;
  bool search_reg_files = false;
  bool search_remote_registry = false;
  SearchScope scope = SearchScope::kEntireRegistry;
  SearchResultMode result_mode = SearchResultMode::kNewTab;
  bool open_in_new_tab = false;
};

bool ShowSearchDialog(HWND owner, SearchDialogResult* result, const SearchSources& available);
bool ShowBrowseKeyDialog(HWND owner, std::wstring* selected_path);

} // namespace regkit
