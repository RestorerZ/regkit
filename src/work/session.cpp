// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "work/session.h"

#include "win32/system_api.h"

#include <system_error>

#include <windows.h>

namespace regkit::work
{

void NameThread(std::thread& thread, const wchar_t* name) noexcept
{
    static const auto set_description = win32::ImportProc<HRESULT(WINAPI*)(HANDLE, PCWSTR)>(L"kernel32.dll", "SetThreadDescription");
    if (set_description && thread.joinable())
    {
        set_description(thread.native_handle(), name);
    }
}

Session::~Session()
{
    CancelAndJoin();
}

uint64_t Session::Start(const wchar_t* name, Task task)
{
    CancelAndJoin();
    return StartPrepared(name, std::move(task));
}

bool Session::StartIfIdle(const wchar_t* name, Task task, uint64_t* generation)
{
    if (running_.load())
    {
        return false;
    }
    Join();
    const uint64_t started = StartPrepared(name, std::move(task));
    if (generation)
    {
        *generation = started;
    }
    return true;
}

void Session::Cancel() noexcept
{
    cancel_.store(true);
    generation_.fetch_add(1);
}

void Session::CancelAndJoin() noexcept
{
    Cancel();
    Join();
}

void Session::Join() noexcept
{
    if (thread_.joinable())
    {
        thread_.join();
    }
    running_.store(false);
}

bool Session::IsCurrent(uint64_t generation) const noexcept
{
    return generation_.load() == generation && !cancel_.load();
}

bool Session::running() const noexcept
{
    return running_.load();
}

uint64_t Session::StartPrepared(const wchar_t* name, Task task)
{
    cancel_.store(false);
    const uint64_t generation = generation_.fetch_add(1) + 1;
    running_.store(true);
    try
    {
        thread_ = std::thread([this, generation, task = std::move(task)]() mutable {
            try
            {
                if (task)
                {
                    task(generation, cancel_);
                }
            }
            catch (...)
            {
            }
            if (generation_.load() == generation)
            {
                running_.store(false);
            }
        });
        NameThread(thread_, name);
    }
    catch (const std::system_error&)
    {
        running_.store(false);
        cancel_.store(true);
    }
    return generation;
}

} // namespace regkit::work
