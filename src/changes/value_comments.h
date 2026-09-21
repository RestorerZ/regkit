// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace regkit::changes {

enum class CommentKeyScope {
  kAny,
  kExact,
  kRecursive,
};

enum class CommentSource {
  kNone,
  kUserValue,
  kUserRule,
  kDefaultValue,
  kDefaultRule,
};

struct CommentEntry {
  std::wstring path;
  std::wstring name;
  DWORD type = 0;
  std::wstring text;
};

struct CommentRule {
  std::wstring id;
  std::wstring name;
  std::optional<DWORD> type;
  std::optional<uint64_t> data_size;
  CommentKeyScope key_scope = CommentKeyScope::kAny;
  std::wstring key_path;
  std::wstring text;
};

struct CommentDocument {
  static constexpr int kCurrentVersion = 2;
  int source_version = 1;
  std::vector<CommentEntry> value_entries;
  std::vector<CommentRule> rules;
};

struct CommentTarget {
  std::wstring path;
  std::wstring name;
  DWORD type = 0;
  uint64_t data_size = 0;
};

struct ResolvedComment {
  std::wstring text;
  CommentSource source = CommentSource::kNone;
  std::wstring rule_id;
};

class ValueComments {
public:
  bool Load(const std::wstring& path);
  bool Save(const std::wstring& path) const;
  void Clear();
  void Merge(const CommentDocument& document);

  const CommentEntry* FindValue(const CommentTarget& target) const;
  void SetValue(CommentEntry entry);
  bool EraseValue(const CommentTarget& target);

  const CommentRule* MatchRule(const CommentTarget& target) const;
  const CommentRule* FindRule(const std::wstring& id) const;
  const CommentRule* FindEquivalentRule(const CommentRule& rule) const;
  void SetRule(CommentRule rule);
  bool EraseRule(const std::wstring& id);

  const std::unordered_map<std::wstring, CommentEntry>& values() const noexcept;
  const std::vector<CommentRule>& rules() const noexcept;

private:
  void Reindex();

  std::unordered_map<std::wstring, CommentEntry> values_;
  std::vector<CommentRule> rules_;
  std::unordered_map<std::wstring, std::vector<size_t>> rule_index_;
};

bool ParseComments(const std::wstring& content, CommentDocument* out);
bool ValidateCatalog(const CommentDocument& document);
std::wstring SerializeComments(const ValueComments& comments);
std::wstring NormalizeKeyPath(std::wstring path);
ResolvedComment ResolveComment(const ValueComments& user, const ValueComments& defaults, const CommentTarget& target);

} // namespace regkit::changes
