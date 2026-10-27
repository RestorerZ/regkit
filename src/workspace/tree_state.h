// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace regkit::workspace
{

struct TreeState
{
    std::wstring selected_path;
    std::vector<std::wstring> expanded_paths;

    void Clear();
    void Normalize();
};

// empty state and the reason in error when a line isnt a tree state line
TreeState ParseTreeState(const std::wstring& content, std::wstring* error = nullptr);
std::wstring SerializeTreeState(const TreeState& state);
// false for a missing file, false with the reason for one that cant be read
bool LoadTreeState(const std::wstring& path, TreeState* state, std::wstring* error = nullptr);
bool SaveTreeState(const std::wstring& path, const TreeState& state);

} // namespace regkit::workspace
