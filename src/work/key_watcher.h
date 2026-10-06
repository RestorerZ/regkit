// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/handle_owner.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace regkit::work
{

class KeyWatcher
{
  public:
    KeyWatcher() = default;
    ~KeyWatcher();
    KeyWatcher(const KeyWatcher&) = delete;
    KeyWatcher& operator=(const KeyWatcher&) = delete;

    void Watch(HWND window, UINT message, HKEY root, const std::wstring& subkey, REGSAM view);
    void Rearm();
    void Stop();
    uint64_t generation() const noexcept
    {
        return generation_.load();
    }

  private:
    void Run();

    std::thread thread_;
    std::mutex mutex_;
    HWND window_ = nullptr;
    UINT message_ = 0;
    HKEY root_ = nullptr;
    std::wstring subkey_;
    REGSAM view_ = 0;
    std::atomic<uint64_t> generation_{0};
    util::UniqueHandle stop_;
    util::UniqueHandle retarget_;
    util::UniqueHandle rearm_;
};

} // namespace regkit::work
