// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "win32/windows_config.h"

#include <windows.h>

#include <objbase.h>

#include <utility>

namespace util
{

class ComInit
{
  public:
    explicit ComInit(DWORD flags = COINIT_APARTMENTTHREADED) noexcept
        : hr_(CoInitializeEx(nullptr, flags))
    {
    }
    ~ComInit()
    {
        if (SUCCEEDED(hr_))
        {
            CoUninitialize();
        }
    }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;

    bool ok() const noexcept
    {
        return SUCCEEDED(hr_);
    }

  private:
    HRESULT hr_;
};

template <typename T, typename Close>
class UniqueResource
{
  public:
    UniqueResource() noexcept = default;
    explicit UniqueResource(T value) noexcept
        : value_(value)
    {
    }
    ~UniqueResource()
    {
        reset();
    }
    UniqueResource(const UniqueResource&) = delete;
    UniqueResource& operator=(const UniqueResource&) = delete;
    UniqueResource(UniqueResource&& other) noexcept
        : value_(other.release())
    {
    }
    UniqueResource& operator=(UniqueResource&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }

    T get() const noexcept
    {
        return value_;
    }
    T* put() noexcept
    {
        reset();
        return &value_;
    }
    T release() noexcept
    {
        return std::exchange(value_, T{});
    }
    void reset(T value = T{}) noexcept
    {
        if (*this)
        {
            Close{}(value_);
        }
        value_ = value;
    }
    explicit operator bool() const noexcept
    {
        return value_ && static_cast<void*>(value_) != INVALID_HANDLE_VALUE;
    }

  private:
    T value_ = T{};
};

struct CloseKey
{
    void operator()(HKEY key) const noexcept
    {
        RegCloseKey(key);
    }
};

struct CloseKernelHandle
{
    void operator()(HANDLE handle) const noexcept
    {
        CloseHandle(handle);
    }
};

struct DeleteGdiObject
{
    void operator()(HGDIOBJ object) const noexcept
    {
        DeleteObject(object);
    }
};

using UniqueHKey = UniqueResource<HKEY, CloseKey>;
using UniqueHandle = UniqueResource<HANDLE, CloseKernelHandle>;

template <typename T>
using UniqueGdiObject = UniqueResource<T, DeleteGdiObject>;

} // namespace util
