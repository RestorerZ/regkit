// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "workspace/tab_state.h"

#include "records/escaped_fields.h"
#include "win32/file_text.h"
#include "win32/text_transform.h"

#include <climits>

namespace regkit::workspace {

namespace {

struct KindName {
  PersistedTab::Kind kind;
  const wchar_t* tag;
};

constexpr KindName kKindNames[] = {
    {PersistedTab::Kind::kRegistry, L"registry"},
    {PersistedTab::Kind::kSearch, L"search"},
    {PersistedTab::Kind::kRegFile, L"regfile"},
};

const wchar_t* KindTag(
    PersistedTab::Kind kind
) {
  for (const KindName& name : kKindNames) {
    if (name.kind == kind) {
      return name.tag;
    }
  }
  return kKindNames[0].tag;
}

const KindName* FindKind(
    std::wstring_view tag
) {
  for (const KindName& name : kKindNames) {
    if (util::EqualsInsensitive(tag, name.tag)) {
      return &name;
    }
  }
  return nullptr;
}

void AppendField(
    std::vector<std::wstring>* fields,
    const wchar_t* key,
    const std::wstring& value
) {
  if (!value.empty()) {
    fields->push_back(key + value);
  }
}

void AppendNumber(
    std::vector<std::wstring>* fields,
    const wchar_t* key,
    int value
) {
  if (value != 0) {
    fields->push_back(key + std::to_wstring(value));
  }
}

int Number(
    std::wstring_view text
) {
  uint64_t value = 0;
  return record_fields::ParseUnsigned(text, INT_MAX, &value) ? static_cast<int>(value) : 0;
}

void ParseLegacyRegistryTab(
    const std::vector<std::wstring>& fields,
    int source_version,
    PersistedTab* tab
) {
  if (fields.size() >= 4) {
    tab->selected_path = fields[3];
  }
  size_t first_expanded = 4;
  if (source_version >= 2) {
    if (fields.size() >= 5) {
      tab->selected_value = fields[4];
    }
    first_expanded = 5;
  }
  for (size_t index = first_expanded; index < fields.size(); ++index) {
    if (!fields[index].empty()) {
      tab->expanded_paths.push_back(fields[index]);
    }
  }
}

void ParseTaggedFields(
    const std::vector<std::wstring>& fields,
    PersistedTab* tab
) {
  for (size_t index = 3; index < fields.size(); ++index) {
    const std::wstring_view field = fields[index];
    const size_t separator = field.find(L'=');
    if (separator == std::wstring_view::npos) {
      continue;
    }
    const std::wstring_view key = field.substr(0, separator);
    const std::wstring value(field.substr(separator + 1));
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
      tab->registry_mode = Number(value);
    } else if (key == L"top") {
      tab->value_top_index = Number(value);
    } else if (key == L"cmp") {
      tab->is_compare = Number(value) != 0;
    } else if (key == L"cmpf") {
      tab->compare_filter = Number(value);
    } else if (key == L"s1k") {
      tab->first_source_kind = Number(value);
    } else if (key == L"s1f") {
      tab->first_source_file = value;
    } else if (key == L"s2k") {
      tab->second_source_kind = Number(value);
    } else if (key == L"s2f") {
      tab->second_source_file = value;
    } else if (key == L"srck") {
      tab->source_kinds.push_back(Number(value));
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
  for (const std::wstring_view line : record_fields::Lines(content)) {
    if (line.empty()) {
      continue;
    }
    if (line.starts_with(L"version=")) {
      state.source_version = Number(line.substr(8));
      continue;
    }
    if (line.starts_with(L"active=")) {
      state.active_index = Number(line.substr(7));
      continue;
    }
    const auto fields = record_fields::DecodeRecord(line);
    const KindName* kind = fields.size() >= 3 && util::EqualsInsensitive(fields[0], L"tab") ? FindKind(fields[1]) : nullptr;
    if (!kind) {
      continue;
    }
    PersistedTab tab;
    tab.kind = kind->kind;
    tab.label = fields[2];
    if (state.source_version >= 3) {
      ParseTaggedFields(fields, &tab);
    } else if (tab.kind == PersistedTab::Kind::kSearch) {
      if (fields.size() < 4) {
        continue;
      }
      tab.search_cache_file = fields[3];
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
    std::vector<std::wstring> fields = {L"tab", KindTag(tab.kind), tab.label};
    AppendField(&fields, L"path=", tab.selected_path);
    AppendField(&fields, L"val=", tab.selected_value);
    for (const std::wstring& value : tab.selected_values) {
      AppendField(&fields, L"sel=", value);
    }
    for (const std::wstring& path : tab.expanded_paths) {
      AppendField(&fields, L"exp=", path);
    }
    AppendField(&fields, L"cache=", tab.search_cache_file);
    AppendField(&fields, L"ccache=", tab.compare_cache_file);
    AppendField(&fields, L"src=", tab.source_path);
    AppendField(&fields, L"machine=", tab.remote_machine);
    AppendNumber(&fields, L"mode=", tab.registry_mode);
    AppendNumber(&fields, L"top=", tab.value_top_index);
    AppendNumber(&fields, L"cmp=", tab.is_compare ? 1 : 0);
    AppendNumber(&fields, L"cmpf=", tab.compare_filter);
    AppendNumber(&fields, L"s1k=", tab.first_source_kind);
    AppendField(&fields, L"s1f=", tab.first_source_file);
    AppendNumber(&fields, L"s2k=", tab.second_source_kind);
    AppendField(&fields, L"s2f=", tab.second_source_file);
    for (size_t i = 0; i < tab.source_kinds.size(); ++i) {
      fields.push_back(L"srck=" + std::to_wstring(tab.source_kinds[i]));
      fields.push_back(L"srcn=" + (i < tab.source_names.size() ? tab.source_names[i] : std::wstring()));
    }
    record_fields::AppendRecord(&content, fields);
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
  if (!util::ReadTextFile(path, &content, nullptr, util::kMaxStateFileBytes)) {
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
