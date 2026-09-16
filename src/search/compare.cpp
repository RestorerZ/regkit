// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search/compare.h"
#include "records/escaped_fields.h"
#include "win32/file_text.h"
#include "win32/text_transform.h"

#include "regfile/reg_file.h"
#include "registry/registry_path.h"
#include "registry/value_format.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <utility>

namespace regkit::search::compare {

namespace {

bool Cancelled(
    const std::atomic_bool* cancel
) {
  return cancel && cancel->load();
}

bool IsWithin(
    const std::wstring& path,
    const std::wstring& base,
    bool recursive
) {
  return util::EqualsInsensitive(path, base) ||
         (recursive && path.size() > base.size() && path[base.size()] == L'\\' && util::StartsWithInsensitive(path, base));
}

std::wstring Combine(
    const std::wstring& base,
    const std::wstring& relative
) {
  return registry_path::JoinSubkey(base, registry_path::DisplayName(relative));
}

std::wstring DataText(
    const Value& value
) {
  if (value.data.empty()) {
    return L"";
  }
  return value_format::DisplayData(
      value.type,
      value.data.data(),
      static_cast<DWORD>(value.data.size())
  );
}

std::wstring EntryText(
    const Value* value
) {
  if (!value) {
    return L"(Missing)";
  }
  const std::wstring type = value_format::TypeName(value->type);
  const std::wstring data = DataText(*value);
  return data.empty() ? type : type + L": " + data;
}

template <typename Map>
void AppendKeys(
    const Map& source,
    std::unordered_set<std::wstring>* seen,
    std::vector<std::wstring>* target
) {
  for (const auto& pair : source) {
    if (seen->insert(pair.first).second) {
      target->push_back(pair.first);
    }
  }
}

} // namespace

bool CaptureRegistry(
    const std::wstring& base_path,
    const RegistryNode& base_node,
    bool recursive,
    Snapshot* snapshot,
    std::wstring* error,
    std::atomic_bool* cancel
) {
  if (!snapshot) {
    return false;
  }
  snapshot->label = base_path;
  snapshot->base_path = base_path;
  snapshot->keys.clear();

  std::vector<std::pair<RegistryNode, std::wstring>> stack;
  stack.emplace_back(base_node, L"");
  while (!stack.empty()) {
    if (Cancelled(cancel)) {
      return false;
    }
    RegistryNode node = std::move(stack.back().first);
    std::wstring relative = std::move(stack.back().second);
    stack.pop_back();

    Key key;
    key.relative_path = relative;
    RegistryStore::KeyEnumResult enumeration;
    bool reserved = false;
    std::vector<std::wstring> children;
    const bool enumerated = RegistryStore::EnumKeyStreaming(
        node,
        true,
        true,
        recursive,
        &enumeration,
        [&](const ValueInfo& value, const BYTE* data, DWORD size) {
          if (Cancelled(cancel)) {
            return false;
          }
          if (!reserved) {
            if (enumeration.info_valid) {
              key.values.reserve(enumeration.info.value_count);
            }
            reserved = true;
          }
          Value captured;
          captured.name = value.name;
          captured.type = value.type;
          if (data && size > 0) {
            captured.data.assign(data, data + size);
          }
          key.values[util::ToLower(captured.name)] = std::move(captured);
          return true;
        },
        recursive ? RegistryStore::SubkeyStreamCallback(
                        [&](const std::wstring& name) {
                          children.push_back(name);
                          return true;
                        }
                    )
                  : RegistryStore::SubkeyStreamCallback()
    );
    if (Cancelled(cancel)) {
      return false;
    }
    if (!enumerated) {
      if (error) {
        *error = L"Could not read the registry key.\n" +
                 (relative.empty() ? base_path : base_path + L"\\" + relative);
      }
      return false;
    }
    snapshot->keys[util::ToLower(relative)] = std::move(key);

    for (const auto& name : children) {
      stack.emplace_back(registry_path::ChildNode(node, name), registry_path::JoinSubkey(relative, name));
    }
  }
  return true;
}

bool LoadRegFile(
    const std::wstring& file_path,
    const std::wstring& base_path,
    bool recursive,
    const NormalizePath& normalize,
    Snapshot* snapshot,
    std::wstring* error,
    std::atomic_bool* cancel
) {
  if (!snapshot || !normalize) {
    return false;
  }
  regfile::Document document;
  bool cancelled = false;
  if (!regfile::Load(file_path, &document, error, cancel, &cancelled)) {
    return false;
  }
  if (document.keys.empty()) {
    if (error) {
      *error = L"No registry keys were found in the .reg file.";
    }
    return false;
  }

  snapshot->base_path = base_path;
  snapshot->label =
      std::filesystem::path(file_path).filename().wstring();
  if (!base_path.empty()) {
    snapshot->label += L": " + base_path;
  }
  snapshot->keys.clear();

  bool matched = false;
  for (const auto& original_path : document.key_order) {
    if (Cancelled(cancel)) {
      return false;
    }
    const std::wstring normalized = normalize(original_path);
    if (normalized.empty() ||
        !IsWithin(normalized, base_path, recursive)) {
      continue;
    }
    matched = true;
    std::wstring relative;
    if (normalized.size() > base_path.size()) {
      relative = normalized.substr(base_path.size() + 1);
    }
    auto source = document.keys.find(util::ToLower(original_path));
    if (source == document.keys.end()) {
      source = document.keys.find(util::ToLower(normalized));
    }

    Key key;
    key.relative_path = relative;
    if (source != document.keys.end()) {
      key.values.reserve(source->second.values.size());
      for (const auto& pair : source->second.values) {
        Value value;
        value.name = pair.second.name;
        value.type = pair.second.type;
        value.data = pair.second.data;
        key.values[util::ToLower(value.name)] = std::move(value);
      }
    }
    snapshot->keys[util::ToLower(relative)] = std::move(key);
  }

  if (!matched) {
    if (error) {
      *error = L"No matching keys were found for the selected path.";
    }
    return false;
  }
  return true;
}

void SortRows(
    std::vector<Row>* rows,
    int column,
    bool ascending
) {
  if (!rows || rows->size() < 2) {
    return;
  }
  if (column == 4) {
    std::stable_sort(rows->begin(), rows->end(), [ascending](const Row& left, const Row& right) {
      return left.matches != right.matches &&
             (ascending ? !left.matches : left.matches);
    });
    return;
  }
  auto field = [column](const Row& row) -> const std::wstring& {
    switch (column) {
    case 1:
      return row.value_name;
    case 2:
      return row.first_text;
    case 3:
      return row.second_text;
    default:
      return row.key_path;
    }
  };
  std::stable_sort(rows->begin(), rows->end(), [&](const Row& left, const Row& right) {
                     const int result =
                         util::CompareListText(field(left), field(right));
                     return result != 0 && (ascending ? result < 0 : result > 0); });
}

std::vector<Row> BuildRows(
    const Snapshot& first,
    const Snapshot& second,
    RowFilter filter,
    std::atomic_bool* cancel
) {
  const bool include_differences = filter != RowFilter::kMatches;
  const bool include_matches = filter != RowFilter::kDifferences;
  std::vector<std::wstring> keys;
  keys.reserve(first.keys.size() + second.keys.size());
  std::unordered_set<std::wstring> seen;
  seen.reserve(keys.capacity());
  AppendKeys(first.keys, &seen, &keys);
  AppendKeys(second.keys, &seen, &keys);

  auto key_display = [&](const std::wstring& lower) -> const std::wstring& {
    const auto first_key = first.keys.find(lower);
    if (first_key != first.keys.end()) {
      return first_key->second.relative_path;
    }
    return second.keys.find(lower)->second.relative_path;
  };
  std::sort(keys.begin(), keys.end(), [&](const std::wstring& left, const std::wstring& right) { return util::CompareInsensitive(key_display(left), key_display(right)) < 0; });

  std::vector<Row> results;
  for (const auto& key_name : keys) {
    if (Cancelled(cancel)) {
      break;
    }
    const auto first_it = first.keys.find(key_name);
    const auto second_it = second.keys.find(key_name);
    const Key* first_key =
        first_it == first.keys.end() ? nullptr : &first_it->second;
    const Key* second_key =
        second_it == second.keys.end() ? nullptr : &second_it->second;
    const std::wstring relative = key_display(key_name);
    const std::wstring first_path = Combine(first.base_path, relative);
    const std::wstring second_path = Combine(second.base_path, relative);

    if (!first_key || !second_key) {
      if (!include_differences) {
        continue;
      }
      Row result;
      result.is_key = true;
      result.key_path = first_key ? first_path : second_path;
      result.first_key_path = first_key ? first_path : std::wstring();
      result.second_key_path = second_key ? second_path : std::wstring();
      result.first_text = first_key ? L"Present" : L"(Missing)";
      result.second_text = second_key ? L"Present" : L"(Missing)";
      results.push_back(std::move(result));
      continue;
    }

    if (include_matches) {
      Row result;
      result.is_key = true;
      result.matches = true;
      result.key_path = first_path;
      result.first_key_path = first_path;
      result.second_key_path = second_path;
      result.first_text = L"Present";
      result.second_text = L"Present";
      results.push_back(std::move(result));
    }

    std::vector<std::wstring> values;
    values.reserve(first_key->values.size() + second_key->values.size());
    std::unordered_set<std::wstring> seen_values;
    seen_values.reserve(values.capacity());
    AppendKeys(first_key->values, &seen_values, &values);
    AppendKeys(second_key->values, &seen_values, &values);
    std::sort(values.begin(), values.end(), [](const std::wstring& left, const std::wstring& right) { return util::CompareInsensitive(left, right) < 0; });

    for (const auto& value_name : values) {
      if (Cancelled(cancel)) {
        return results;
      }
      const auto first_value = first_key->values.find(value_name);
      const auto second_value = second_key->values.find(value_name);
      const Value* left =
          first_value == first_key->values.end()
              ? nullptr
              : &first_value->second;
      const Value* right =
          second_value == second_key->values.end()
              ? nullptr
              : &second_value->second;
      const bool matches = left && right && left->type == right->type &&
                           left->data == right->data;
      if ((matches && !include_matches) ||
          (!matches && !include_differences)) {
        continue;
      }

      Row result;
      result.matches = matches;
      result.key_path = first_path;
      result.first_key_path = first_path;
      result.second_key_path = second_path;
      result.value_name = left ? left->name : right->name;
      result.first_text = EntryText(left);
      result.second_text = EntryText(right);
      results.push_back(std::move(result));
    }
  }
  return results;
}

std::wstring SerializeRows(
    const std::vector<Row>& rows
) {
  std::wstring content = L"version=2\n";
  for (const Row& row : rows) {
    content += record_fields::Escape(row.key_path);
    content += L'\t';
    content += record_fields::Escape(row.first_key_path);
    content += L'\t';
    content += record_fields::Escape(row.second_key_path);
    content += L'\t';
    content += record_fields::Escape(row.value_name);
    content += L'\t';
    content += record_fields::Escape(row.first_text);
    content += L'\t';
    content += record_fields::Escape(row.second_text);
    content += L'\t';
    content += (row.is_key ? L"1" : L"0");
    content += L'\t';
    content += (row.matches ? L"1" : L"0");
    content += L'\n';
  }
  return content;
}

bool ParseRows(
    const std::wstring& content,
    std::vector<Row>* rows
) {
  if (!rows) {
    return false;
  }
  std::vector<Row> parsed;
  for (const std::wstring& line : record_fields::Lines(content)) {
    if (line.empty() || line.rfind(L"version=", 0) == 0) {
      continue;
    }
    const auto fields = record_fields::Split(line);
    if (fields.size() < 7) {
      continue;
    }
    Row row;
    row.key_path = record_fields::Unescape(fields[0]);
    row.first_key_path = record_fields::Unescape(fields[1]);
    row.second_key_path = record_fields::Unescape(fields[2]);
    row.value_name = record_fields::Unescape(fields[3]);
    row.first_text = record_fields::Unescape(fields[4]);
    row.second_text = record_fields::Unescape(fields[5]);
    row.is_key = _wtoi(fields[6].c_str()) != 0;
    row.matches = fields.size() >= 8 && _wtoi(fields[7].c_str()) != 0;
    parsed.push_back(std::move(row));
  }
  *rows = std::move(parsed);
  return true;
}

bool SaveRows(
    const std::wstring& path,
    const std::vector<Row>& rows
) {
  return !path.empty() && util::WriteTextFile(path, SerializeRows(rows), false);
}

bool LoadRows(
    const std::wstring& path,
    std::vector<Row>* rows
) {
  if (!rows || path.empty()) {
    return false;
  }
  std::wstring content;
  if (!util::ReadTextFile(path, &content)) {
    return false;
  }
  return ParseRows(content, rows);
}

} // namespace regkit::search::compare
