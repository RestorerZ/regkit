// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "workspace/settings.h"

#include "records/escaped_fields.h"
#include "win32/file_text.h"
#include "win32/text_transform.h"

#include <algorithm>
#include <cwchar>
#include <span>

namespace regkit::workspace {
namespace {

struct BoolField {
  const wchar_t* key;
  bool Settings::*member;
};

struct IntField {
  const wchar_t* key;
  int Settings::*member;
};

struct TextField {
  const wchar_t* key;
  std::wstring Settings::*member;
};

constexpr BoolField kBoolFields[] = {
    {L"clear_history_on_exit", &Settings::clear_history_on_exit},
    {L"clear_tabs_on_exit", &Settings::clear_tabs_on_exit},
    {L"view_toolbar", &Settings::show_toolbar},
    {L"view_address_bar", &Settings::show_address_bar},
    {L"view_filter_bar", &Settings::show_filter_bar},
    {L"view_tab_control", &Settings::show_tab_control},
    {L"view_tree", &Settings::show_tree},
    {L"view_history", &Settings::show_history},
    {L"view_status_bar", &Settings::show_status_bar},
    {L"view_keys_in_list", &Settings::show_keys_in_list},
    {L"view_simulated_keys", &Settings::show_simulated_keys},
    {L"view_extra_hives", &Settings::show_extra_hives},
    {L"view_value_grid", &Settings::show_value_grid},
    {L"save_tree_state", &Settings::save_tree_state},
    {L"auto_check_updates", &Settings::auto_check_updates},
    {L"default_reset_enabled", &Settings::default_reset_enabled},
    {L"always_run_as_admin", &Settings::always_run_as_admin},
    {L"always_run_as_system", &Settings::always_run_as_system},
    {L"always_run_as_trustedinstaller", &Settings::always_run_as_trustedinstaller},
    {L"always_on_top", &Settings::always_on_top},
    {L"single_instance", &Settings::single_instance},
    {L"read_only", &Settings::read_only},
    {L"font_italic", &Settings::font_italic},
};

constexpr IntField kPositiveFields[] = {
    {L"tree_width", &Settings::tree_width},
    {L"history_height", &Settings::history_height},
    {L"font_size", &Settings::font_size},
    {L"font_weight", &Settings::font_weight},
};

constexpr IntField kPlacementFields[] = {
    {L"window_x", &Settings::window_x},
    {L"window_y", &Settings::window_y},
    {L"window_width", &Settings::window_width},
    {L"window_height", &Settings::window_height},
};

constexpr TextField kTextFields[] = {
    {L"theme_mode", &Settings::theme_mode},
    {L"theme_preset", &Settings::theme_preset},
    {L"icon_set", &Settings::icon_set},
    {L"font_face", &Settings::font_face},
};

template <typename Field>
const Field* FindField(
    const std::wstring& key,
    std::span<const Field> fields
) {
  for (const Field& field : fields) {
    if (util::EqualsInsensitive(key, field.key)) {
      return &field;
    }
  }
  return nullptr;
}

bool Indexed(
    const std::wstring& key,
    std::wstring_view prefix,
    size_t* index
) {
  if (key.size() <= prefix.size() || !util::StartsWithInsensitive(key, prefix)) {
    return false;
  }
  const wchar_t* start = key.c_str() + prefix.size();
  wchar_t* end = nullptr;
  const unsigned long value = wcstoul(start, &end, 10);
  if (end == start || *end != L'\0' || value > 4096) {
    return false;
  }
  *index = value;
  return true;
}

template <typename T>
void SetIndexed(
    std::vector<T>* items,
    size_t index,
    T value,
    T fill = T()
) {
  if (index >= items->size()) {
    items->resize(index + 1, fill);
  }
  (*items)[index] = value;
}

void Line(
    std::wstring* output,
    std::wstring_view key,
    std::wstring_view value
) {
  output->append(key).append(L"=").append(value).append(L"\n");
}

} // namespace

Settings ParseSettings(
    const std::wstring& content,
    Settings settings
) {
  for (const std::wstring& line : record_fields::Lines(content)) {
    const size_t separator = line.find(L'=');
    if (separator == std::wstring::npos) {
      continue;
    }
    const std::wstring key = util::TrimWhitespace(std::wstring_view(line).substr(0, separator));
    const std::wstring value = util::TrimWhitespace(std::wstring_view(line).substr(separator + 1));
    const int number = _wtoi(value.c_str());
    size_t index = 0;
    if (const auto* bool_field = FindField<BoolField>(key, kBoolFields)) {
      settings.*(bool_field->member) = util::ParseBool(value);
    } else if (const auto* positive_field = FindField<IntField>(key, kPositiveFields)) {
      if (number > 0) {
        settings.*(positive_field->member) = number;
      }
    } else if (const auto* placement_field = FindField<IntField>(key, kPlacementFields)) {
      settings.*(placement_field->member) = number;
      settings.window_placement_present = true;
    } else if (const auto* text_field = FindField<TextField>(key, kTextFields)) {
      settings.*(text_field->member) = value;
    } else if (util::EqualsInsensitive(key, L"window_maximized")) {
      settings.window_maximized = util::ParseBool(value);
      settings.window_placement_present = true;
    } else if (util::EqualsInsensitive(key, L"save_tabs")) {
      settings.save_tabs = util::ParseBool(value);
      settings.save_tab_kinds = settings.save_tabs ? kSaveTabsAll : 0;
    } else if (util::EqualsInsensitive(key, L"save_tab_types")) {
      settings.save_tab_kinds = number & kSaveTabsAll;
      settings.save_tabs = settings.save_tab_kinds != 0;
    } else if (util::EqualsInsensitive(key, L"font_use_default")) {
      settings.use_custom_font = !util::ParseBool(value);
    } else if (Indexed(key, L"trace_recent_", &index)) {
      SetIndexed(&settings.recent_traces, index, value);
    } else if (Indexed(key, L"default_recent_", &index)) {
      SetIndexed(&settings.recent_defaults, index, value);
    } else if (Indexed(key, L"value_column_width_", &index)) {
      SetIndexed(&settings.value_column_widths, index, number);
    } else if (Indexed(key, L"value_column_visible_", &index)) {
      SetIndexed(&settings.value_column_visible, index, util::ParseBool(value), true);
    }
  }
  if (settings.always_run_as_trustedinstaller) {
    settings.always_run_as_system = false;
    settings.always_run_as_admin = false;
  } else if (settings.always_run_as_system) {
    settings.always_run_as_admin = false;
  }
  return settings;
}

std::wstring SerializeSettings(
    const Settings& settings
) {
  std::wstring content;
  for (const BoolField& field : kBoolFields) {
    Line(&content, field.key, settings.*(field.member) ? L"1" : L"0");
  }
  for (const IntField& field : kPositiveFields) {
    if (settings.*(field.member) > 0) {
      Line(&content, field.key, std::to_wstring(settings.*(field.member)));
    }
  }
  for (const TextField& field : kTextFields) {
    Line(&content, field.key, settings.*(field.member));
  }
  if (settings.window_width > 0 && settings.window_height > 0) {
    for (const IntField& field : kPlacementFields) {
      Line(&content, field.key, std::to_wstring(settings.*(field.member)));
    }
    Line(&content, L"window_maximized", settings.window_maximized ? L"1" : L"0");
  }
  Line(&content, L"save_tabs", settings.save_tabs ? L"1" : L"0");
  Line(&content, L"save_tab_types", std::to_wstring(settings.save_tab_kinds));
  Line(&content, L"font_use_default", settings.use_custom_font ? L"0" : L"1");
  for (size_t index = 0; index < settings.recent_traces.size(); ++index) {
    if (!settings.recent_traces[index].empty()) {
      Line(&content, L"trace_recent_" + std::to_wstring(index), settings.recent_traces[index]);
    }
  }
  for (size_t index = 0; index < settings.recent_defaults.size(); ++index) {
    if (!settings.recent_defaults[index].empty()) {
      Line(&content, L"default_recent_" + std::to_wstring(index), settings.recent_defaults[index]);
    }
  }
  const size_t columns = std::max(settings.value_column_widths.size(), settings.value_column_visible.size());
  for (size_t index = 0; index < columns; ++index) {
    const bool visible = index >= settings.value_column_visible.size() || settings.value_column_visible[index];
    Line(&content, L"value_column_width_" + std::to_wstring(index), std::to_wstring(index < settings.value_column_widths.size() ? settings.value_column_widths[index] : 0));
    Line(&content, L"value_column_visible_" + std::to_wstring(index), visible ? L"1" : L"0");
  }
  return content;
}

bool LoadSettings(
    const std::wstring& path,
    Settings* settings
) {
  std::wstring content;
  if (!settings || !util::ReadTextFile(path, &content)) {
    return false;
  }
  *settings = ParseSettings(content, std::move(*settings));
  return true;
}

bool SaveSettings(
    const std::wstring& path,
    const Settings& settings
) {
  return !path.empty() && util::WriteTextFile(path, SerializeSettings(settings), false);
}

} // namespace regkit::workspace