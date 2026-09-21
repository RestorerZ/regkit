// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "changes/value_comments.h"

#include "records/escaped_fields.h"
#include "win32/file_text.h"
#include "win32/text_transform.h"

#include <algorithm>
#include <tuple>

namespace regkit::changes {

namespace {

constexpr wchar_t kHeaderTag[] = L"regkit-comments";

std::wstring ValueKey(
    const std::wstring& path,
    const std::wstring& name,
    DWORD type
) {
  return util::ToLower(path) + L'\t' + util::ToLower(name) + L'\t' + std::to_wstring(type);
}

bool KeyMatches(
    const CommentRule& rule,
    const std::wstring& path
) {
  switch (rule.key_scope) {
  case CommentKeyScope::kExact:
    return util::EqualsInsensitive(path, rule.key_path);
  case CommentKeyScope::kRecursive:
    return util::StartsWithInsensitive(path, rule.key_path) &&
           (path.size() == rule.key_path.size() || path[rule.key_path.size()] == L'\\');
  default:
    return true;
  }
}

bool Matches(
    const CommentRule& rule,
    const CommentTarget& target
) {
  return (!rule.type || *rule.type == target.type) &&
         (!rule.data_size || *rule.data_size == target.data_size) &&
         KeyMatches(rule, target.path);
}

auto Specificity(
    const CommentRule& rule
) {
  const int scope = rule.key_scope == CommentKeyScope::kExact ? 3 : rule.key_scope == CommentKeyScope::kRecursive ? 2
                                                                                                                  : 1;
  const size_t depth = rule.key_scope == CommentKeyScope::kRecursive ? rule.key_path.size() : 0;
  return std::tuple(scope, depth, static_cast<int>(rule.type.has_value()) + static_cast<int>(rule.data_size.has_value()));
}

bool SameConditions(
    const CommentRule& left,
    const CommentRule& right
) {
  return util::EqualsInsensitive(left.name, right.name) && left.type == right.type &&
         left.data_size == right.data_size && left.key_scope == right.key_scope &&
         util::EqualsInsensitive(left.key_path, right.key_path);
}

bool ParseOptional(
    std::wstring_view field,
    uint64_t maximum,
    std::optional<uint64_t>* value
) {
  uint64_t parsed = 0;
  if (field == L"*") {
    value->reset();
    return true;
  }
  if (!record_fields::ParseUnsigned(field, maximum, &parsed)) {
    return false;
  }
  *value = parsed;
  return true;
}

std::wstring Optional(
    const std::optional<uint64_t>& value
) {
  return value ? std::to_wstring(*value) : L"*";
}

bool ParseRule(
    std::vector<std::wstring>& fields,
    CommentRule* rule
) {
  static constexpr std::pair<const wchar_t*, CommentKeyScope> kScopes[] = {{L"any", CommentKeyScope::kAny}, {L"key", CommentKeyScope::kExact}, {L"tree", CommentKeyScope::kRecursive}};
  if (fields.size() != 8) {
    return false;
  }
  const auto scope = std::find_if(std::begin(kScopes), std::end(kScopes), [&](const auto& entry) { return fields[5] == entry.first; });
  std::optional<uint64_t> type;
  std::optional<uint64_t> size;
  if (fields[1].empty() || scope == std::end(kScopes) ||
      !ParseOptional(fields[3], MAXDWORD, &type) || !ParseOptional(fields[4], UINT64_MAX, &size)) {
    return false;
  }
  rule->id = std::move(fields[1]);
  rule->name = std::move(fields[2]);
  if (type) {
    rule->type = static_cast<DWORD>(*type);
  }
  rule->data_size = size;
  rule->key_scope = scope->second;
  rule->key_path = NormalizeKeyPath(std::move(fields[6]));
  rule->text = std::move(fields[7]);
  return (rule->key_scope == CommentKeyScope::kAny) == rule->key_path.empty();
}

const wchar_t* ScopeName(
    CommentKeyScope scope
) {
  return scope == CommentKeyScope::kExact ? L"key" : scope == CommentKeyScope::kRecursive ? L"tree"
                                                                                          : L"any";
}

} // namespace

std::wstring NormalizeKeyPath(
    std::wstring path
) {
  while (!path.empty() && path.back() == L'\\') {
    path.pop_back();
  }
  return path;
}

bool ValueComments::Load(
    const std::wstring& path
) {
  std::wstring content;
  CommentDocument document;
  if (!util::ReadTextFile(path, &content, nullptr, util::kMaxCommentFileBytes) || !ParseComments(content, &document)) {
    return false;
  }
  Clear();
  Merge(document);
  return true;
}

bool ValueComments::Save(
    const std::wstring& path
) const {
  return !path.empty() &&
         util::WriteTextFile(path, SerializeComments(*this), false);
}

void ValueComments::Clear() {
  values_.clear();
  rules_.clear();
  rule_index_.clear();
}

void ValueComments::Merge(
    const CommentDocument& document
) {
  for (const CommentEntry& entry : document.value_entries) {
    values_[ValueKey(entry.path, entry.name, entry.type)] = entry;
  }
  for (CommentRule rule : document.rules) {
    const CommentRule* existing = FindRule(rule.id);
    if (existing && !SameConditions(*existing, rule)) {
      rule.id.clear();
    }
    SetRule(std::move(rule));
  }
}

const CommentEntry* ValueComments::FindValue(
    const CommentTarget& target
) const {
  const auto found = values_.find(ValueKey(target.path, target.name, target.type));
  return found == values_.end() ? nullptr : &found->second;
}

void ValueComments::SetValue(
    CommentEntry entry
) {
  std::wstring key = ValueKey(entry.path, entry.name, entry.type);
  values_[std::move(key)] = std::move(entry);
}

bool ValueComments::EraseValue(
    const CommentTarget& target
) {
  return values_.erase(ValueKey(target.path, target.name, target.type)) != 0;
}

const CommentRule* ValueComments::MatchRule(
    const CommentTarget& target
) const {
  const auto candidates = rule_index_.find(util::ToLower(target.name));
  if (candidates == rule_index_.end()) {
    return nullptr;
  }
  const CommentRule* best = nullptr;
  for (const size_t index : candidates->second) {
    const CommentRule& rule = rules_[index];
    if (Matches(rule, target) && (!best || Specificity(rule) >= Specificity(*best))) {
      best = &rule;
    }
  }
  return best;
}

const CommentRule* ValueComments::FindRule(
    const std::wstring& id
) const {
  const auto found = std::find_if(rules_.begin(), rules_.end(), [&](const CommentRule& rule) { return rule.id == id; });
  return id.empty() || found == rules_.end() ? nullptr : &*found;
}

const CommentRule* ValueComments::FindEquivalentRule(
    const CommentRule& rule
) const {
  const auto found = std::find_if(rules_.begin(), rules_.end(), [&](const CommentRule& existing) { return SameConditions(existing, rule); });
  return found == rules_.end() ? nullptr : &*found;
}

void ValueComments::SetRule(
    CommentRule rule
) {
  if (rule.id.empty()) {
    rule.id = util::RandomFileSuffix(L"").substr(1);
  }
  rule.key_path = NormalizeKeyPath(std::move(rule.key_path));
  const auto found = std::find_if(rules_.begin(), rules_.end(), [&](const CommentRule& existing) { return existing.id == rule.id; });
  if (found != rules_.end()) {
    rules_.erase(found);
  }
  rules_.push_back(std::move(rule));
  Reindex();
}

bool ValueComments::EraseRule(
    const std::wstring& id
) {
  const auto removed = std::erase_if(rules_, [&](const CommentRule& rule) { return rule.id == id; });
  Reindex();
  return removed != 0;
}

const std::unordered_map<std::wstring, CommentEntry>& ValueComments::values() const noexcept {
  return values_;
}

const std::vector<CommentRule>& ValueComments::rules() const noexcept {
  return rules_;
}

void ValueComments::Reindex() {
  rule_index_.clear();
  for (size_t index = 0; index < rules_.size(); ++index) {
    rule_index_[util::ToLower(rules_[index].name)].push_back(index);
  }
}

bool ParseComments(
    const std::wstring& content,
    CommentDocument* out
) {
  CommentDocument document;
  bool first = true;
  for (const std::wstring_view line : record_fields::Lines(content)) {
    if (line.empty()) {
      continue;
    }
    if (first && line.starts_with(kHeaderTag)) {
      uint64_t version = 0;
      if (!record_fields::ParseHeader(line, kHeaderTag, &version) || version < 2 || version > CommentDocument::kCurrentVersion) {
        return false;
      }
      document.source_version = static_cast<int>(version);
      first = false;
      continue;
    }
    first = false;
    auto fields = record_fields::DecodeRecord(line);
    if (util::IsBlank(fields.back())) {
      fields.back().clear();
    }
    if (fields[0] == L"rule" && document.source_version >= 2) {
      CommentRule rule;
      if (!ParseRule(fields, &rule)) {
        return false;
      }
      document.rules.push_back(std::move(rule));
      continue;
    }
    uint64_t type = 0;
    const bool value_entry = fields[0] == L"value";
    if (fields.size() != 5 || (!value_entry && fields[0] != L"name") ||
        !record_fields::ParseUnsigned(fields[3], MAXDWORD, &type)) {
      return false;
    }
    if (value_entry) {
      document.value_entries.push_back({std::move(fields[1]), std::move(fields[2]), static_cast<DWORD>(type), std::move(fields[4])});
    } else {
      CommentRule rule;
      rule.id = util::RandomFileSuffix(L"").substr(1);
      rule.name = std::move(fields[2]);
      rule.type = static_cast<DWORD>(type);
      rule.text = std::move(fields[4]);
      document.rules.push_back(std::move(rule));
    }
  }
  *out = std::move(document);
  return true;
}

bool ValidateCatalog(
    const CommentDocument& document
) {
  for (const CommentEntry& entry : document.value_entries) {
    if (util::IsBlank(entry.text)) {
      return false;
    }
  }
  for (size_t left = 0; left < document.rules.size(); ++left) {
    const CommentRule& rule = document.rules[left];
    if (util::IsBlank(rule.text)) {
      return false;
    }
    for (size_t right = left + 1; right < document.rules.size(); ++right) {
      const CommentRule& other = document.rules[right];
      const bool overlap = util::EqualsInsensitive(rule.name, other.name) && Specificity(rule) == Specificity(other) &&
                           (!rule.type || !other.type || rule.type == other.type) &&
                           (!rule.data_size || !other.data_size || rule.data_size == other.data_size) &&
                           (rule.key_scope == CommentKeyScope::kAny || util::EqualsInsensitive(rule.key_path, other.key_path));
      if (overlap) {
        return false;
      }
    }
  }
  return true;
}

std::wstring SerializeComments(
    const ValueComments& comments
) {
  std::wstring content;
  record_fields::AppendHeader(&content, kHeaderTag, CommentDocument::kCurrentVersion);
  std::vector<const std::pair<const std::wstring, CommentEntry>*> values;
  for (const auto& pair : comments.values()) {
    values.push_back(&pair);
  }
  std::sort(values.begin(), values.end(), [](const auto* left, const auto* right) { return left->first < right->first; });
  for (const auto* pair : values) {
    const CommentEntry& entry = pair->second;
    record_fields::AppendRecord(&content, {L"value", entry.path, entry.name, std::to_wstring(entry.type), entry.text});
  }
  for (const CommentRule& rule : comments.rules()) {
    const std::optional<uint64_t> type = rule.type ? std::optional<uint64_t>(*rule.type) : std::nullopt;
    record_fields::AppendRecord(&content, {L"rule", rule.id, rule.name, Optional(type), Optional(rule.data_size), ScopeName(rule.key_scope), rule.key_path, rule.text});
  }
  return content;
}

ResolvedComment ResolveComment(
    const ValueComments& user,
    const ValueComments& defaults,
    const CommentTarget& target
) {
  const std::pair<const ValueComments*, CommentSource> layers[] = {{&user, CommentSource::kUserValue}, {&defaults, CommentSource::kDefaultValue}};
  for (const auto& [comments, source] : layers) {
    if (const CommentEntry* entry = comments->FindValue(target)) {
      return {entry->text, source, {}};
    }
    if (const CommentRule* rule = comments->MatchRule(target)) {
      return {rule->text, static_cast<CommentSource>(static_cast<int>(source) + 1), rule->id};
    }
  }
  return {};
}

} // namespace regkit::changes
