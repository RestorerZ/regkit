// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/tools/update_checker.h"

#include "frame/window/message_ids.h"
#include "win32/file_dialog.h"
#include "win32/file_text.h"
#include "win32/shell_paths.h"
#include "win32/system_error.h"
#include "win32/text_transform.h"
#include "ui/feedback.h"
#include "win32/translation.h"
#include "resource.h"

#include <win32ui/handle_owner.h>
#include <win32ui/json/reader.h>
#include <win32ui/system_api.h>

#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <stdlib.h>
#include <utility>

namespace regkit::frame
{

namespace
{

constexpr wchar_t kReleasesPage[] = L"https://github.com/nohuto/regkit/releases";
constexpr wchar_t kLatestReleaseUrl[] = L"https://api.github.com/repos/nohuto/regkit/releases/latest";
constexpr const wchar_t* kSetupSuffix = sizeof(void*) == 8 ? L"-x64.exe" : L"-x86.exe";
constexpr const wchar_t* kPortableSuffix = sizeof(void*) == 8 ? L"-x64.zip" : L"-x86.zip";
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\4678f42c-c6a2-4df9-bc2a-dddbd2613045_is1";
constexpr wchar_t kSetupArguments[] = L"/VERYSILENT /NORESTART /RELAUNCH=1 ";
constexpr DWORD kSetupCancelledBeforeInstall = 2;
constexpr size_t kMaxReleaseJsonBytes = 4ull * 1024ull * 1024ull;
constexpr size_t kMaxSetupBytes = 192ull * 1024ull * 1024ull;

const wchar_t* InstallScope()
{
    const std::wstring directory = util::GetModuleDirectory();
    for (const auto& [root, scope] : {std::pair{HKEY_LOCAL_MACHINE, L"/ALLUSERS"}, std::pair{HKEY_CURRENT_USER, L"/CURRENTUSER"}})
    {
        wchar_t location[MAX_PATH * 2] = {};
        DWORD size = sizeof(location);
        if (RegGetValueW(root, kUninstallKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, location, &size) != ERROR_SUCCESS)
        {
            continue;
        }
        std::wstring_view path(location);
        while (path.size() > 3 && path.back() == L'\\')
        {
            path.remove_suffix(1);
        }
        if (util::EqualsInsensitive(path, directory))
        {
            return scope;
        }
    }
    return nullptr;
}

std::wstring ReleasePage(const std::wstring& tag)
{
    const bool plain = !tag.empty() && tag.find_first_not_of(L"0123456789.-_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::wstring::npos;
    return plain ? std::wstring(kReleasesPage) + L"/tag/" + tag : kReleasesPage;
}

bool IsAllowedReleaseHost(const std::wstring& host)
{
    constexpr const wchar_t* kHosts[] = {L"github.com", L"api.github.com", L"objects.githubusercontent.com", L"release-assets.githubusercontent.com"};
    return std::any_of(std::begin(kHosts), std::end(kHosts), [&](const wchar_t* allowed) { return util::EqualsInsensitive(host, allowed); });
}

std::wstring ErrorText(DWORD code)
{
    if (code < WINHTTP_ERROR_BASE || code > WINHTTP_ERROR_LAST)
    {
        return util::FormatWin32Error(code);
    }
    wchar_t text[512] = {};
    DWORD length =
        FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, GetModuleHandleW(L"winhttp.dll"), code, 0, text, static_cast<DWORD>(_countof(text)), nullptr);
    while (length > 0 && iswspace(text[length - 1]))
    {
        --length;
    }
    return length > 0 ? std::wstring(text, length) : L"WinHTTP " + std::to_wstring(code);
}

std::wstring HttpGet(const std::wstring& url, const std::atomic_bool& cancel, std::string* body, size_t max_bytes)
{
    using Handle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
    URL_COMPONENTS parts = {sizeof(parts)};
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    {
        return ErrorText(GetLastError());
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS ||
        !IsAllowedReleaseHost(std::wstring(parts.lpszHostName, parts.dwHostNameLength)))
    {
        return util::Tr(L"The release location isn't a trusted RegKit download address.");
    }
    Handle session(
        WinHttpOpen(L"RegKit", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
        WinHttpCloseHandle
    );
    if (!session)
    {
        session.reset(WinHttpOpen(L"RegKit", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    }
    Handle connect(session ? WinHttpConnect(session.get(), std::wstring(parts.lpszHostName, parts.dwHostNameLength).c_str(), parts.nPort, 0) : nullptr, WinHttpCloseHandle);
    Handle request(connect ? WinHttpOpenRequest(connect.get(), L"GET", std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr, WinHttpCloseHandle);
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!request || !WinHttpSetTimeouts(request.get(), 5000, 10000, 10000, 30000) ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr) ||
        !WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
    {
        return ErrorText(GetLastError());
    }
    if (status != HTTP_STATUS_OK)
    {
        return L"HTTP " + std::to_wstring(status);
    }
    char buffer[64 * 1024];
    DWORD read = 0;
    while (!cancel.load())
    {
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &read))
        {
            return ErrorText(GetLastError());
        }
        if (read == 0)
        {
            return {};
        }
        if (body->size() + read > max_bytes)
        {
            return util::Tr(L"The download is larger than RegKit accepts.");
        }
        body->append(buffer, read);
    }
    return ErrorText(ERROR_CANCELLED);
}

bool ReadRelease(const std::string& body, UpdateCheckPayload* release, std::wstring* digest)
{
    const std::wstring text = util::Utf8ToWide(body);
    win32ui::JsonReader reader(text);
    const std::wstring_view prefix = release->installed ? L"RegKit-Setup-" : L"RegKit-Portable-";
    const std::wstring_view suffix = release->installed ? kSetupSuffix : kPortableSuffix;
    return reader.Object([&](const std::wstring& member) {
        if (member == L"tag_name")
        {
            return reader.String(&release->version, 64);
        }
        if (member != L"assets")
        {
            return reader.Skip();
        }
        return reader.Array([&] {
            std::wstring name;
            std::wstring link;
            std::wstring hash;
            const bool read = reader.Object([&](const std::wstring& field) {
                std::wstring* out = field == L"name"                   ? &name
                                    : field == L"browser_download_url" ? &link
                                    : field == L"digest"               ? &hash
                                                                       : nullptr;
                return out && reader.Next() == L'"' ? reader.String(out) : reader.Skip();
            });
            // portable name becomes the saved file name
            if (read && release->download_url.empty() && name.starts_with(prefix) && name.size() > prefix.size() + suffix.size() &&
                name.ends_with(suffix) && name.find_first_of(L"\\/:") == std::wstring::npos)
            {
                release->download_url = std::move(link);
                release->file_name = std::move(name);
                *digest = std::move(hash);
            }
            return read;
        });
    }) && reader.End();
}

std::array<int, 3> VersionParts(const std::wstring& text)
{
    std::array<int, 3> parts = {};
    const size_t start = text.find_first_of(L"0123456789");
    if (start != std::wstring::npos)
    {
        swscanf_s(text.c_str() + start, L"%d.%d.%d", &parts[0], &parts[1], &parts[2]);
    }
    return parts;
}

std::string Sha256(const std::string& data)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    UCHAR digest[32] = {};
    const bool hashed = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
                        BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)) &&
                        BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0)) &&
                        BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash)
    {
        BCryptDestroyHash(hash);
    }
    if (algorithm)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    std::string hex;
    for (size_t i = 0; hashed && i < sizeof(digest); ++i)
    {
        char pair[3] = {};
        sprintf_s(pair, "%02x", digest[i]);
        hex += pair;
    }
    return hex;
}

std::wstring RandomName(const wchar_t* prefix)
{
    // RandomFileSuffix includes a leading dot, omit it from the name
    return prefix + util::RandomFileSuffix(L"").substr(1);
}

std::wstring PrivateTempDirectory(std::wstring* directory)
{
    static const auto temp_path2 = win32ui::ImportProc<DWORD(WINAPI*)(DWORD, LPWSTR)>(L"kernel32.dll", "GetTempPath2W");
    wchar_t temp[MAX_PATH + 1] = {};
    const DWORD length = temp_path2 ? temp_path2(static_cast<DWORD>(_countof(temp)), temp)
                                    : GetTempPathW(static_cast<DWORD>(_countof(temp)), temp);
    if (length == 0)
    {
        return util::FormatWin32Error(GetLastError());
    }
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const std::wstring candidate = util::JoinPath(temp, RandomName(L"RegKit-"));
        if (CreateDirectoryW(candidate.c_str(), nullptr))
        {
            *directory = candidate;
            return {};
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return util::FormatWin32Error(GetLastError());
        }
    }
    return util::FormatWin32Error(ERROR_ALREADY_EXISTS);
}

std::wstring DownloadsDirectory(std::wstring* directory)
{
    win32ui::UniqueCoTask<PWSTR> path;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, path.put());
    if (FAILED(hr) || !path)
    {
        return util::FormatWin32Error(HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_PATH_NOT_FOUND);
    }
    *directory = path.get();
    return {};
}

void DeleteSetup(const std::wstring& path)
{
    DeleteFileW(path.c_str());
    RemoveDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str());
}

// setup uses a private folder, portable ZIP goes to Downloads
std::wstring SaveDownload(const std::wstring& url, const std::string& sha256, const std::wstring& file_name, bool installed, const std::atomic_bool& cancel, std::wstring* path)
{
    if (sha256.size() != 64 || sha256.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    {
        return util::Tr(L"The release didn't publish a checksum for this download.");
    }
    std::string data;
    const std::wstring error = HttpGet(url, cancel, &data, kMaxSetupBytes);
    if (!error.empty())
    {
        return error;
    }
    if (data.empty() || _stricmp(Sha256(data).c_str(), sha256.c_str()) != 0)
    {
        return util::Tr(L"The downloaded file doesn't match the release checksum.");
    }
    std::wstring directory;
    const std::wstring directory_error = installed ? PrivateTempDirectory(&directory) : DownloadsDirectory(&directory);
    if (!directory_error.empty())
    {
        return directory_error;
    }
    win32ui::UniqueHandle file;
    for (int copy = 0; copy < 100 && !file; ++copy)
    {
        std::wstring name = installed ? RandomName(L"RegKit-Setup-") + L".exe" : file_name;
        if (copy > 0)
        {
            name.insert(name.size() - 4, L" (" + std::to_wstring(copy) + L")");
        }
        *path = util::JoinPath(directory, name);
        file.reset(CreateFileW(path->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file && GetLastError() != ERROR_FILE_EXISTS)
        {
            break;
        }
    }
    DWORD written = 0;
    const bool saved = file && WriteFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                       written == data.size();
    const DWORD code = saved ? ERROR_SUCCESS : GetLastError();
    const bool created = static_cast<bool>(file);
    file.reset();
    if (!saved)
    {
        if (created)
        {
            DeleteFileW(path->c_str());
        }
        if (installed)
        {
            RemoveDirectoryW(directory.c_str());
        }
        path->clear();
        return util::FormatWin32Error(code != ERROR_SUCCESS ? code : ERROR_WRITE_FAULT);
    }
    return {};
}

win32ui::UniqueHandle OpenVerifiedSetup(const std::wstring& path, const std::string& sha256)
{
    win32ui::UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    LARGE_INTEGER size = {};
    if (!file || !GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaxSetupBytes)
    {
        return {};
    }
    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size() ||
        _stricmp(Sha256(data).c_str(), sha256.c_str()) != 0)
    {
        return {};
    }
    return file;
}

} // namespace

void UpdateChecker::Attach(HWND owner, StatusCallback status)
{
    owner_ = owner;
    status_ = std::move(status);
}

void UpdateChecker::SetStatus(const std::wstring& text) const
{
    if (status_)
    {
        status_(text);
    }
}

void UpdateChecker::Cancel()
{
    session_.CancelAndJoin();
}

void UpdateChecker::Check(bool silent)
{
    HWND owner = owner_;
    session_.StartIfIdle(L"UpdateCheckThread", [owner, silent](uint64_t, std::atomic_bool& cancel) {
        auto payload = std::make_unique<UpdateCheckPayload>();
        payload->silent = silent;
        payload->installed = InstallScope() != nullptr;
        std::wstring error;
        // failures still post a payload so the UI shows the error
        try
        {
            std::string json;
            error = HttpGet(kLatestReleaseUrl, cancel, &json, kMaxReleaseJsonBytes);
            std::wstring digest;
            if (error.empty() && (!ReadRelease(json, payload.get(), &digest) || payload->version.empty()))
            {
                error = util::Tr(L"The response didn't contain a release.");
            }
            payload->sha256 = digest.starts_with(L"sha256:") ? util::WideToUtf8(digest.substr(7)) : std::string();
        }
        catch (const std::exception&)
        {
            error = util::FormatWin32Error(ERROR_NOT_ENOUGH_MEMORY);
        }
        if (cancel.load())
        {
            return;
        }
        if (!error.empty())
        {
            payload->failed = true;
            payload->error = util::TrDetail(L"Failed to reach the update server.", error);
        }
        work::PostPayload(owner, frame::message_id::kUpdateCheckReady, 0, payload);
    });
}

void UpdateChecker::Download(const UpdateCheckPayload& release)
{
    HWND owner = owner_;
    const bool started = session_.StartIfIdle(
        L"UpdateDownloadThread",
        [owner, url = release.download_url, sha256 = release.sha256, file_name = release.file_name, installed = release.installed](uint64_t, std::atomic_bool& cancel) {
            auto payload = std::make_unique<UpdateCheckPayload>();
            payload->sha256 = sha256;
            payload->installed = installed;
            std::wstring error;
            try
            {
                error = SaveDownload(url, sha256, file_name, installed, cancel, &payload->download_path);
            }
            catch (const std::exception&)
            {
                error = util::FormatWin32Error(ERROR_NOT_ENOUGH_MEMORY);
            }
            if (cancel.load())
            {
                return;
            }
            if (!error.empty())
            {
                payload->failed = true;
                payload->download_path.clear();
                payload->error = util::TrDetail(L"The update couldn't be downloaded.", error);
            }
            work::PostPayload(owner, frame::message_id::kUpdateCheckReady, 0, payload);
        }
    );
    if (started)
    {
        SetStatus(util::TrLabel(L"Downloading", L"RegKit " + release.version));
    }
}

void UpdateChecker::Apply(UpdateCheckPayload* payload)
{
    SetStatus(std::wstring());
    if (!payload)
    {
        return;
    }
    if (payload->setup_exited)
    {
        setup_running_ = false;
        EnableWindow(owner_, TRUE);
        if (!payload->error.empty())
        {
            ui::ShowError(owner_, payload->error);
        }
        return;
    }
    if (payload->failed)
    {
        if (!payload->silent)
        {
            ui::ShowError(owner_, payload->error);
        }
        return;
    }
    if (!payload->download_path.empty())
    {
        if (!payload->installed)
        {
            ui::RevealFile(owner_, payload->download_path);
            return;
        }
        DropSetup();
        setup_path_ = std::move(payload->download_path);
        setup_sha256_ = std::move(payload->sha256);
        PostMessageW(owner_, WM_CLOSE, 0, 0);
        return;
    }
    if (VersionParts(payload->version) <= VersionParts(REGKIT_VERSION_STR_W))
    {
        if (!payload->silent)
        {
            ui::ShowInfo(owner_, util::Tr(L"RegKit is up to date."));
        }
        return;
    }
    const bool found = !payload->download_url.empty();
    const std::wstring message =
        L"RegKit " + payload->version + L" (" + util::TrLabel(L"installed", REGKIT_VERSION_STR_W) + L")\n\n" +
        (!found               ? util::Tr(L"No download was found for this build. Open the release page?")
         : payload->installed ? util::Tr(L"Download and install it now? RegKit restarts once the update is installed.")
                              : util::Tr(L"Download the new portable version?"));
    const std::wstring download = !found ? L"" : payload->installed ? util::Tr(L"Update")
                                                                    : util::Tr(L"Download Portable");
    const std::wstring release = found && payload->installed ? L"" : util::Tr(L"Open Release");
    const int choice = ui::PromptChoice(owner_, message, util::Tr(L"Update Available"), download, release, util::Tr(L"Close"), {70, 70, 70});
    if (choice == IDYES)
    {
        Download(*payload);
    }
    else if (choice == IDNO)
    {
        win32::ShellOpen(owner_, ReleasePage(payload->version).c_str());
    }
}

bool UpdateChecker::StartSetup()
{
    const std::wstring path = std::exchange(setup_path_, {});
    const std::string sha256 = std::exchange(setup_sha256_, {});
    const wchar_t* scope = InstallScope();
    // silent setup reuses prior choices and relaunches RegKit with saved session
    const std::wstring arguments = std::wstring(kSetupArguments) + (scope ? scope : L"");
    SHELLEXECUTEINFOW info = {sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner_;
    info.lpFile = path.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_SHOWNORMAL;
    bool started = false;
    {
        const win32ui::UniqueHandle verified = OpenVerifiedSetup(path, sha256);
        if (!verified)
        {
            ui::ShowError(owner_, util::Tr(L"The downloaded setup changed after RegKit verified it and wasn't started."));
        }
        else if (!ShellExecuteExW(&info) || !info.hProcess)
        {
            ui::ShowError(owner_, util::TrDetail(L"The setup couldn't be started.", util::FormatWin32Error(GetLastError())));
        }
        else
        {
            started = true;
        }
    }
    if (!started)
    {
        DeleteSetup(path);
        return false;
    }
    setup_running_ = true;
    session_.Start(L"UpdateSetupThread", [owner = owner_, process = info.hProcess](uint64_t, std::atomic_bool& cancel) {
        const win32ui::UniqueHandle owned(process);
        while (WaitForSingleObject(process, 200) == WAIT_TIMEOUT)
        {
            if (cancel.load())
            {
                return;
            }
        }
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        auto payload = std::make_unique<UpdateCheckPayload>();
        payload->setup_exited = true;
        if (code != kSetupCancelledBeforeInstall)
        {
            payload->error = util::TrDetail(L"The update couldn't be installed.", util::TrLabel(L"Exit code", std::to_wstring(code)));
        }
        work::PostPayload(owner, frame::message_id::kUpdateCheckReady, 0, payload);
    });
    return true;
}

void UpdateChecker::DropSetup()
{
    if (!setup_path_.empty())
    {
        DeleteSetup(std::exchange(setup_path_, {}));
        setup_sha256_.clear();
    }
}

} // namespace regkit::frame
