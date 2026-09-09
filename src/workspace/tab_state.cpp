// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "workspace/tab_state.h"

#include "records/escaped_fields.h"
#include "win32/file_text.h"

namespace regkit::workspace {

namespace {

const wchar_t* KindTag(
    PersistedTab::Kind kind
) {
  switch (kind) {
  case PersistedTab::Kind::kSearch:
    return L"search";
  case PersistedTab::Kind::kRegFile:
    return L"regfile";
  default:
    return L"registry";
  }
}

void AppendField(
    std::wstring* content,
    const wchar_t* key,
    const std::wstring& value
) {
  if (value.empty()) {
    return;
  }
  content->push_back(L'\t');
  content->append(key);
  content->append(record_fields::Escape(value));
}

void AppendNumber(
    std::wstring* content,
    const wchar_t* key,
    int value
) {
  if (value == 0) {
    return;
  }
  content->push_back(L'\t');
  content->append(key);
  content->append(std::to_wstring(value));
}

void ParseLegacyRegistryTab(
    const std::vector<std::wstring>& fields,
    int source_version,
    PersistedTab* tab
) {
  if (fields.size() >= 4) {
    tab->selected_path = record_fields::Unescape(fields[3]);
  }
  size_t first_expanded = 4;
  if (source_version >= 2) {
    if (fields.size() >= 5) {
      tab->selected_value = record_fields::Unescape(fields[4]);
    }
    first_expanded = 5;
  }
  for (size_t index = first_expanded; index < fields.size(); ++index) {
    std::wstring path = record_fields::Unescape(fields[index]);
    if (!path.empty()) {
      tab->expanded_paths.push_back(std::move(path));
    }
  }
}

void ParseTaggedFields(
    const std::vector<std::wstring>& fields,
    PersistedTab* tab
) {
  for (size_t index = 3; index < fields.size(); ++index) {
    const std::wstring& field = fields[index];
    const size_t separator = field.find(L'=');
    if (separator == std::wstring::npos) {
      continue;
    }
    const std::wstring key = field.substr(0, separator);
    const std::wstring value = record_fields::Unescape(field.substr(separator + 1));
    if (key == L"path") {
      tab->selected_path = value;
    } else if (key == L"val") {
      tab->selected_value = value;
    } else if (key == L"sel") {
      tab->selected_values.push_back(value);
    } else if (key == L"exp") {
      tab->expanded_paths.push_back(value);
    } else if (key == L"cache") {
      tab->search_cache_file = value;
    } else if (key == L"ccache") {
      tab->compare_cache_file = value;
    } else if (key == L"src") {
      tab->source_path = value;
    } else if (key == L"machine") {
      tab->remote_machine = value;
    } else if (key == L"mode") {
      tab->registry_mode = _wtoi(value.c_str());
    } else if (key == L"top") {
      tab->value_top_index = _wtoi(value.c_str());
    } else if (key == L"cmp") {
      tab->is_compare = _wtoi(value.c_str()) != 0;
    } else if (key == L"s1k") {
      tab->first_source_kind = _wtoi(value.c_str());
    } else if (key == L"s1f") {
      tab->first_source_file = value;
    } else if (key == L"s2k") {
      tab->second_source_kind = _wtoi(value.c_str());
    } else if (key == L"s2f") {
      tab->second_source_file = value;
    } else if (key == L"srck") {
      tab->source_kinds.push_back(_wtoi(value.c_str()));
    } else if (key == L"srcn") {
      tab->source_names.push_back(value);
    }
  }
}

} // namespace

TabState ParseTabs(
    const std::wstring& content
) {
  TabState state;
  for (const std::wstring& line : record_fields::Lines(content)) {
    if (line.empty()) {
      continue;
    }
    if (line.rfind(L"version=", 0) == 0) {
      state.source_version = _wtoi(line.substr(8).c_str());
      continue;
    }
    if (line.rfind(L"active=", 0) == 0) {
      state.active_index = _wtoi(line.substr(7).c_str());
      continue;
    }
    const auto fields = record_fields::Split(line);
    if (fields.size() < 3 || _wcsicmp(fields[0].c_str(), L"tab") != 0) {
      continue;
    }
    PersistedTab tab;
    tab.label = record_fields::Unescape(fields[2]);
    if (_wcsicmp(fields[1].c_str(), L"registry") == 0) {
      tab.kind = PersistedTab::Kind::kRegistry;
    } else if (_wcsicmp(fields[1].c_str(), L"search") == 0) {
      tab.kind = PersistedTab::Kind::kSearch;
    } else if (_wcsicmp(fields[1].c_str(), L"regfile") == 0) {
      tab.kind = PersistedTab::Kind::kRegFile;
    } else {
      continue;
    }
    if (state.source_version >= 3) {
      ParseTaggedFields(fields, &tab);
    } else if (tab.kind == PersistedTab::Kind::kSearch) {
      if (fields.size() < 4) {
        continue;
      }
      tab.search_cache_file = record_fields::Unescape(fields[3]);
    } else {
      ParseLegacyRegistryTab(fields, state.source_version, &tab);
    }
    if (tab.kind == PersistedTab::Kind::kRegFile && tab.source_path.empty()) {
      continue;
    }
    state.tabs.push_back(std::move(tab));
  }
  return state;
}

std::wstring SerializeTabs(
    const TabState& state
) {
  std::wstring content = L"version=" +
                         std::to_wstring(TabState::kCurrentVersion) +
                         L"\nactive=" + std::to_wstring(state.active_index) +
                         L"\n";
  for (const PersistedTab& tab : state.tabs) {
    content.append(L"tab\t");
    content.append(KindTag(tab.kind));
    content.push_back(L'\t');
    content.append(record_fields::Escape(tab.label));
    AppendField(&content, L"path=", tab.selected_path);
    AppendField(&content, L"val=", tab.selected_value);
    for (const std::wstring& value : tab.selected_values) {
      AppendField(&content, L"sel=", value);
    }
    for (const std::wstring& path : tab.expanded_paths) {
      AppendField(&content, L"exp=", path);
    }
    AppendField(&content, L"cache=", tab.search_cache_file);
    AppendField(&content, L"ccache=", tab.compare_cache_file);
    AppendField(&content, L"src=", tab.source_path);
    AppendField(&content, L"machine=", tab.remote_machine);
    AppendNumber(&content, L"mode=", tab.registry_mode);
    AppendNumber(&content, L"top=", tab.value_top_index);
    AppendNumber(&content, L"cmp=", tab.is_compare ? 1 : 0);
    AppendNumber(&content, L"s1k=", tab.first_source_kind);
    AppendField(&content, L"s1f=", tab.first_source_file);
    AppendNumber(&content, L"s2k=", tab.second_source_kind);
    AppendField(&content, L"s2f=", tab.second_source_file);
    for (size_t i = 0; i < tab.source_kinds.size(); ++i) {
      content.push_back(L'\t');
      content.append(L"srck=");
      content.append(std::to_wstring(tab.source_kinds[i]));
      content.push_back(L'\t');
      content.append(L"srcn=");
      content.append(record_fields::Escape(
          i < tab.source_names.size() ? tab.source_names[i] : std::wstring()
      ));
    }
    content.push_back(L'\n');
  }
  return content;
}

bool LoadTabs(
    const std::wstring& path,
    TabState* state
) {
  if (!state) {
    return false;
  }
  std::wstring content;
  if (!util::ReadTextFile(path, &content)) {
    return false;
  }
  *state = ParseTabs(content);
  return true;
}

bool SaveTabs(
    const std::wstring& path,
    const TabState& state
) {
  return !path.empty() &&
         util::WriteTextFile(path, SerializeTabs(state), false);
}

} // namespace regkit::workspace
