// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#define _CRT_RAND_S

#include "win32/file_text.h"

#include "win32/system_error.h"
#include "win32/translation.h"

#include <win32ui/handle_owner.h>

#include <cstdlib>
#include <limits>

namespace regkit::util
{

std::wstring RandomFileSuffix(const wchar_t* extension)
{
    unsigned int high = 0;
    unsigned int low = 0;
    if (rand_s(&high) != 0 || rand_s(&low) != 0)
    {
        high = GetTickCount();
        low = GetCurrentProcessId();
    }
    wchar_t suffix[32] = {};
    swprintf_s(suffix, L".%08x%08x", high, low);
    return std::wstring(suffix) + extension;
}

std::string WideToNarrow(const std::wstring& text, UINT code_page)
{
    if (text.empty())
    {
        return {};
    }
    const int size =
        WideCharToMultiByte(code_page, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }
    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(code_page, 0, text.data(), static_cast<int>(text.size()), output.data(), size, nullptr, nullptr);
    return output;
}

std::string WideToUtf8(const std::wstring& text)
{
    return WideToNarrow(text, CP_UTF8);
}

std::wstring NarrowToWide(std::string_view text, UINT code_page, DWORD flags)
{
    const int size = text.empty() ? 0 : MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
    {
        return {};
    }
    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()), output.data(), size);
    return output;
}

std::wstring Utf8ToWide(std::string_view text)
{
    return NarrowToWide(text, CP_UTF8, MB_ERR_INVALID_CHARS);
}

bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>* output, uint64_t max_bytes, DWORD share_mode)
{
    if (!output)
    {
        return false;
    }
    output->clear();
    const win32ui::UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, share_mode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size = {};
    if (!file || !GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 || static_cast<uint64_t>(size.QuadPart) > max_bytes ||
        size.QuadPart > static_cast<LONGLONG>(std::numeric_limits<DWORD>::max()))
    {
        return false;
    }
    output->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.get(), output->data(), static_cast<DWORD>(output->size()), &read, nullptr) || read != output->size())
    {
        output->clear();
        return false;
    }
    return true;
}

bool ReadTextFile(const std::wstring& path, std::wstring* output, bool* utf16, uint64_t max_bytes, DWORD share_mode)
{
    if (!output)
    {
        return false;
    }
    output->clear();
    if (utf16)
    {
        *utf16 = false;
    }
    std::vector<BYTE> bytes;
    if (!ReadFileBytes(path, &bytes, max_bytes, share_mode))
    {
        return false;
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
    {
        if ((bytes.size() - 2) % sizeof(wchar_t) != 0)
        {
            return false;
        }
        output->assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / sizeof(wchar_t));
        if (utf16)
        {
            *utf16 = true;
        }
        return !output->empty();
    }
    size_t offset = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
    {
        offset = 3;
    }
    const std::string_view narrow(reinterpret_cast<const char*>(bytes.data() + offset), bytes.size() - offset);
    *output = Utf8ToWide(narrow);
    if (output->empty() && offset == 0)
    {
        *output = NarrowToWide(narrow, CP_ACP);
    }
    return !output->empty();
}

bool ReadStateFile(const std::wstring& path, std::wstring* output, std::wstring* error, uint64_t max_bytes)
{
    std::wstring reason;
    output->clear();
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
    {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        {
            return true;
        }
        reason = FormatWin32Error(code);
    }
    else if (const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow; size == 0)
    {
        return true;
    }
    else if (size > max_bytes)
    {
        reason = Tr(L"The file is too large.");
    }
    else
    {
        SetLastError(ERROR_SUCCESS);
        if (ReadTextFile(path, output, nullptr, max_bytes))
        {
            return true;
        }
        const DWORD code = GetLastError();
        // a byte order mark alone is an empty file
        if (code == ERROR_SUCCESS && output->empty() && size <= 3)
        {
            return true;
        }
        reason = code != ERROR_SUCCESS ? FormatWin32Error(code) : std::wstring();
        reason = reason.empty() ? std::wstring(Tr(L"The file isn't valid text.")) : reason;
    }
    if (error)
    {
        *error = std::move(reason);
    }
    return false;
}

std::wstring BadLineReason(size_t line)
{
    return TrLabel(L"Line", std::to_wstring(line));
}

namespace
{

bool WriteWholeFile(const std::wstring& path, const std::wstring& text, bool utf16, DWORD disposition = CREATE_ALWAYS)
{
    const win32ui::UniqueHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
    {
        return false;
    }
    DWORD written = 0;
    bool ok = true;
    if (utf16)
    {
        constexpr BYTE bom[] = {0xFF, 0xFE};
        ok = WriteFile(file.get(), bom, sizeof(bom), &written, nullptr) != 0 && written == sizeof(bom);
        if (ok && !text.empty())
        {
            const DWORD byte_count = static_cast<DWORD>(text.size() * sizeof(wchar_t));
            ok = WriteFile(file.get(), text.data(), byte_count, &written, nullptr) != 0 && written == byte_count;
        }
    }
    else
    {
        const std::string utf8 = WideToUtf8(text);
        const DWORD byte_count = static_cast<DWORD>(utf8.size());
        if (byte_count != 0)
        {
            ok = WriteFile(file.get(), utf8.data(), byte_count, &written, nullptr) != 0 && written == byte_count;
        }
    }
    return ok && FlushFileBuffers(file.get()) != 0;
}

} // namespace

bool WriteTextFile(const std::wstring& path, const std::wstring& text, bool utf16)
{
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const std::wstring temp_path = path + RandomFileSuffix(L".tmp");
        if (!WriteWholeFile(temp_path, text, utf16, CREATE_NEW))
        {
            if (GetLastError() == ERROR_FILE_EXISTS)
            {
                continue;
            }
            DeleteFileW(temp_path.c_str());
            return false;
        }
        for (int retry = 0; retry < 20; ++retry)
        {
            if (MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                return true;
            }
            const DWORD error = GetLastError();
            if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION)
            {
                break;
            }
            Sleep(25);
        }
        DeleteFileW(temp_path.c_str());
        return false;
    }
    return false;
}

} // namespace regkit::util
