// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "win32/windows_config.h"

#include <windows.h>

#include <algorithm>
#include <commctrl.h>
#include <cwctype>
#include <limits>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <uxtheme.h>
#include <vector>

#include "appearance/feedback.h"
#include "appearance/presets.h"
#include "appearance/theme.h"
#include "cli/reg_command.h"
#include "frame/main_window.h"
#include "frame/message_ids.h"
#include "regfile/registry_transfer.h"
#include "registry/registry_path.h"
#include "registry/registry_store.h"
#include "win32/file_text.h"
#include "win32/handle_owner.h"
#include "win32/process_rights.h"
#include "win32/registry_native.h"
#include "win32/restart.h"
#include "win32/shell_integration.h"
#include "win32/shell_paths.h"
#include "win32/system_error.h"
#include "win32/text_transform.h"
#include "workspace/settings.h"

namespace
{

using regkit::frame::message_id::kEditRegFileCopyDataId;
using regkit::frame::message_id::kExternalJumpCopyDataId;
using regkit::frame::message_id::kRegKitWindowProperty;
using regkit::win32::kRestartAdminArg;
using regkit::win32::kRestartSystemArg;
using regkit::win32::kRestartTiArg;
using regkit::win32::kRestartUserArg;
constexpr wchar_t kEditRegFileArg[] = L"--edit-reg";
constexpr wchar_t kInstallEditContextMenuArg[] = L"--install-edit-context-menu";
constexpr wchar_t kUninstallEditContextMenuArg[] = L"--uninstall-edit-context-menu";
constexpr wchar_t kInstallRegEditReplacementArg[] = L"--install-regedit-replacement";
constexpr wchar_t kUninstallRegEditReplacementArg[] = L"--uninstall-regedit-replacement";
constexpr wchar_t kOverrideArg[] = L"--override";

constexpr const wchar_t* kRegEditNames[] = {L"regedit.exe", L"regedit", L"regedt32.exe", L"regedt32"};

using util::FormatWin32Error;

std::vector<std::wstring> GetCommandLineArgs()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (!argv)
    {
        return args;
    }
    for (int i = 1; i < argc; ++i)
    {
        args.emplace_back(argv[i]);
    }
    LocalFree(argv);
    return args;
}

void ApplyDataDirOverride(const std::vector<std::wstring>& args)
{
    const std::wstring dir = regkit::win32::RestartDataDir(args);
    if (dir.empty())
    {
        return;
    }
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const std::wstring probe = util::JoinPath(dir, L"session" + util::RandomFileSuffix(L".probe"));
    const util::UniqueHandle handle(
        CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)
    );
    if (handle)
    {
        SetEnvironmentVariableW(L"REGKIT_DATA_DIR", dir.c_str());
    }
}

bool HasCommandLineArg(const std::vector<std::wstring>& args, const wchar_t* arg)
{
    return std::any_of(args.begin(), args.end(), [&](const std::wstring& entry) { return util::EqualsInsensitive(entry, arg); });
}

bool IsRegEditLaunchArg(const std::wstring& arg)
{
    const bool drive_absolute =
        arg.size() >= 3 && iswalpha(arg[0]) && arg[1] == L':' && (arg[2] == L'\\' || arg[2] == L'/');
    const bool unc_absolute =
        arg.size() >= 3 && ((arg[0] == L'\\' && arg[1] == L'\\') || (arg[0] == L'/' && arg[1] == L'/'));
    const std::wstring name = regkit::registry_path::Leaf(arg);
    return (drive_absolute || unc_absolute) &&
           std::any_of(std::begin(kRegEditNames), std::end(kRegEditNames), [&](const wchar_t* regedit) { return util::EqualsInsensitive(name, regedit); });
}

bool IsInterceptedRegEditLaunch(const std::vector<std::wstring>& args)
{
    for (const auto& arg : args)
    {
        if (IsRegEditLaunchArg(arg))
        {
            return true;
        }
    }
    return false;
}

std::vector<std::wstring> StripRegEditLaunchArg(const std::vector<std::wstring>& args)
{
    std::vector<std::wstring> stripped;
    stripped.reserve(args.size());
    for (const auto& arg : args)
    {
        if (!IsRegEditLaunchArg(arg))
        {
            stripped.push_back(arg);
        }
    }
    return stripped;
}

std::vector<std::wstring> RegFilesFromArgs(const std::vector<std::wstring>& args)
{
    std::vector<std::wstring> files;
    for (const auto& arg : args)
    {
        if (arg.empty() || arg[0] == L'-' || arg[0] == L'/' || IsRegEditLaunchArg(arg))
        {
            continue;
        }
        if (util::HasFileExtension(arg, L".reg"))
        {
            files.push_back(arg);
        }
    }
    return files;
}

bool LooksLikeRegistryPath(const std::wstring& arg)
{
    regkit::RegistryNode node;
    return regkit::registry_path::ParseRoot(arg, &node);
}

bool ResolveExternalJumpTarget(const std::vector<std::wstring>& args, std::wstring* out)
{
    if (!out)
    {
        return false;
    }
    out->clear();
    bool intercepted_regedit = false;
    std::wstring explicit_key_path;
    for (size_t index = 0; index < args.size(); ++index)
    {
        const std::wstring& arg = args[index];
        if (util::EqualsInsensitive(arg, L"--goto") || util::EqualsInsensitive(arg, L"/goto"))
        {
            if (index + 1 < args.size())
            {
                explicit_key_path = args[++index];
            }
            continue;
        }
        if (IsRegEditLaunchArg(arg))
        {
            intercepted_regedit = true;
            continue;
        }
        if (arg.empty())
        {
            continue;
        }
        if (arg[0] == L'-' || arg[0] == L'/')
        {
            if (regkit::win32::ArgTakesValue(arg))
            {
                ++index;
            }
            continue;
        }
        if (LooksLikeRegistryPath(arg))
        {
            explicit_key_path = arg;
            continue;
        }
        if (!explicit_key_path.empty())
        {
            *out = explicit_key_path + L"\\" + arg;
            return true;
        }
    }
    if (!explicit_key_path.empty())
    {
        *out = explicit_key_path;
        return true;
    }
    if (!intercepted_regedit)
    {
        return false;
    }
    return util::ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\RegEdit", L"LastKey", out) == ERROR_SUCCESS &&
           !out->empty();
}

bool IsOwnRegKitWindow(HWND hwnd)
{
    // accept only this executable running in the same sign in session
    DWORD process_id = 0;
    if (!GetWindowThreadProcessId(hwnd, &process_id) || process_id == 0 || process_id == GetCurrentProcessId())
    {
        return false;
    }
    DWORD our_session = 0;
    DWORD their_session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &our_session) ||
        !ProcessIdToSessionId(process_id, &their_session) || our_session != their_session)
    {
        return false;
    }
    const std::wstring theirs = util::GetProcessImagePath(process_id);
    if (theirs.empty())
    {
        return false;
    }
    const std::wstring ours = util::GetModulePath();
    return !ours.empty() && util::EqualsInsensitive(theirs, ours);
}

BOOL CALLBACK FindRegKitWindowProc(HWND hwnd, LPARAM lparam)
{
    if (!GetPropW(hwnd, kRegKitWindowProperty))
    {
        return TRUE;
    }
    if (!IsOwnRegKitWindow(hwnd))
    {
        return TRUE;
    }
    auto* found = reinterpret_cast<HWND*>(lparam);
    *found = hwnd;
    return FALSE;
}

HWND FindRunningRegKitWindow()
{
    HWND found = nullptr;
    EnumWindows(FindRegKitWindowProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool SendTextToRegKit(HWND window, HWND sender, ULONG_PTR message_id, const std::wstring& text)
{
    if (!window || !sender || text.empty())
    {
        return false;
    }
    DWORD window_pid = 0;
    if (GetWindowThreadProcessId(window, &window_pid))
    {
        AllowSetForegroundWindow(window_pid);
    }
    COPYDATASTRUCT data = {};
    data.dwData = message_id;
    data.cbData = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<wchar_t*>(text.c_str());
    DWORD_PTR accepted = 0;
    // stop waiting if existing instance is frozen
    return SendMessageTimeoutW(window, WM_COPYDATA, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 1500, &accepted) != 0 &&
           accepted != 0;
}

regkit::workspace::Settings LoadStartupSettings()
{
    regkit::workspace::Settings settings;
    const std::wstring folder = util::GetAppDataFolder();
    if (!folder.empty())
    {
        regkit::workspace::LoadSettings(util::JoinPath(folder, L"settings.ini"), &settings);
    }
    return settings;
}

void ApplyStartupTheme(const regkit::workspace::Settings& settings)
{
    const regkit::ThemeMode mode = regkit::ParseThemeMode(settings.theme_mode);
    if (mode == regkit::ThemeMode::kCustom)
    {
        std::vector<regkit::ThemePreset> presets;
        if (!regkit::ThemePresetStore::Load(&presets) || presets.empty())
        {
            presets = regkit::ThemePresetStore::BuiltInPresets();
        }
        if (const regkit::ThemePreset* preset = regkit::FindThemePreset(presets, settings.theme_preset))
        {
            regkit::Theme::SetCustomColors(preset->colors, preset->is_dark);
        }
    }
    regkit::Theme::SetMode(mode);
}

struct RestartTarget
{
    const wchar_t* arg;
    bool (*is_current)();
    bool (*launch)(const std::wstring&, const std::wstring&, DWORD*, bool*);
    const wchar_t* request_failure;
    const wchar_t* launch_failure;
};

constexpr RestartTarget kSystemTarget = {kRestartSystemArg, util::IsProcessSystem, util::LaunchProcessAsSystem, L"Failed to request SYSTEM restart.", L"Failed to restart with SYSTEM rights."};
constexpr RestartTarget kTrustedInstallerTarget = {
    kRestartTiArg,
    util::IsProcessTrustedInstaller,
    util::LaunchProcessAsTrustedInstaller,
    L"Failed to request TrustedInstaller restart.",
    L"Failed to restart with TrustedInstaller rights."
};

bool RestartAs(const RestartTarget& target, DWORD parent_pid, const std::vector<std::wstring>& original_args, int* exit_code)
{
    if (target.is_current())
    {
        return false;
    }
    const std::wstring exe_path = util::GetModulePath();
    if (exe_path.empty())
    {
        regkit::ui::ShowError(nullptr, L"Failed to locate the executable path.");
        return false;
    }
    // keep user arguments while replacing old internal restart flags
    const std::wstring arguments = regkit::win32::RestartArguments(target.arg, parent_pid, original_args);
    *exit_code = 0;
    if (!util::IsProcessElevated())
    {
        if (SUCCEEDED(regkit::win32::LaunchElevated(nullptr, exe_path, arguments)))
        {
            return true;
        }
        regkit::ui::ShowError(nullptr, target.request_failure);
        return false;
    }
    DWORD error = 0;
    bool impersonation_lost = false;
    const bool launched = target.launch(L"\"" + exe_path + L"\" " + arguments, L"", &error, &impersonation_lost);
    if (impersonation_lost)
    {
        regkit::ui::ShowError(nullptr, L"RegKit couldn't restore its own security context and must close now.");
        *exit_code = launched ? 0 : 1;
        return true;
    }
    if (!launched)
    {
        const std::wstring detail = FormatWin32Error(error);
        regkit::ui::ShowError(nullptr, detail.empty() ? target.launch_failure : std::wstring(target.launch_failure) + L"\n" + detail);
    }
    return launched;
}

} // namespace

void ApplySafeDllSearchPolicy()
{
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    using SetDefaultDllDirectoriesFn = BOOL(WINAPI*)(DWORD);
    // resolve this at runtime as older winvers may not export it
    const auto set_directories =
        kernel ? reinterpret_cast<SetDefaultDllDirectoriesFn>(GetProcAddress(kernel, "SetDefaultDllDirectories"))
               : nullptr;
    if (set_directories)
    {
        set_directories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS);
    }
    // remove current dir from legacy DLL search path
    SetDllDirectoryW(L"");
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int cmd_show)
{
    ApplySafeDllSearchPolicy();
    const auto args = GetCommandLineArgs();
    if (HasCommandLineArg(args, kInstallEditContextMenuArg) || HasCommandLineArg(args, kUninstallEditContextMenuArg) ||
        HasCommandLineArg(args, kInstallRegEditReplacementArg) ||
        HasCommandLineArg(args, kUninstallRegEditReplacementArg))
    {
        const std::wstring exe_path = util::GetModulePath();
        if (exe_path.empty())
        {
            return 1;
        }
        LONG result = ERROR_SUCCESS;
        if (HasCommandLineArg(args, kInstallEditContextMenuArg))
        {
            result = regkit::win32::SetRegFileEditMenu(exe_path, true);
        }
        else if (HasCommandLineArg(args, kUninstallEditContextMenuArg))
        {
            result = regkit::win32::RemoveRegFileEditMenuIfOwned(exe_path);
        }
        else
        {
            bool conflict = false;
            result = regkit::win32::SetRegEditReplacement(exe_path, HasCommandLineArg(args, kInstallRegEditReplacementArg), &conflict, HasCommandLineArg(args, kOverrideArg));
            if (result != ERROR_SUCCESS && conflict)
            {
                return 2;
            }
        }
        return result == ERROR_SUCCESS ? 0 : 1;
    }

    regkit::Theme::InitializeDarkModeSupport();
    util::ComInit com;
    if (!com.ok())
    {
        regkit::ui::ShowError(nullptr, L"COM initialization failed.");
        return 1;
    }

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_DATE_CLASSES |
                ICC_COOL_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);
    BufferedPaintInit();

    ApplyDataDirOverride(args);
    const bool regedit_compat_requested = IsInterceptedRegEditLaunch(args);
    int cli_exit = 0;
    if (regkit::cli::Execute(regedit_compat_requested ? StripRegEditLaunchArg(args) : args, &cli_exit))
    {
        return cli_exit;
    }
    const regkit::workspace::Settings startup_settings = LoadStartupSettings();
    ApplyStartupTheme(startup_settings);
    std::wstring startup_jump_target;
    const bool external_jump_requested = ResolveExternalJumpTarget(args, &startup_jump_target);
    const bool edit_reg_file_requested = HasCommandLineArg(args, kEditRegFileArg);
    const std::vector<std::wstring> reg_files = RegFilesFromArgs(args);
    const bool stay_as_user = HasCommandLineArg(args, kRestartUserArg);
    const DWORD restart_parent_pid = regkit::win32::RestartParentPid(args);
    const DWORD handoff_pid = restart_parent_pid != 0 ? restart_parent_pid : GetCurrentProcessId();
    const RestartTarget* restart_target =
        HasCommandLineArg(args, kRestartTiArg)       ? &kTrustedInstallerTarget
        : HasCommandLineArg(args, kRestartSystemArg) ? &kSystemTarget
        : !stay_as_user && startup_settings.always_run_as_trustedinstaller && !util::IsProcessTrustedInstaller()
            ? &kTrustedInstallerTarget
        : !stay_as_user && startup_settings.always_run_as_system && !util::IsProcessSystem() ? &kSystemTarget
                                                                                             : nullptr;
    int restart_exit = 0;
    if (restart_target)
    {
        if (RestartAs(*restart_target, handoff_pid, args, &restart_exit))
        {
            return restart_exit;
        }
    }
    else if ((HasCommandLineArg(args, kRestartAdminArg) || (!stay_as_user && startup_settings.always_run_as_admin)) &&
             !util::IsProcessElevated())
    {
        const std::wstring exe_path = util::GetModulePath();
        if (!exe_path.empty() && SUCCEEDED(regkit::win32::LaunchElevated(
                                     nullptr,
                                     exe_path,
                                     regkit::win32::RestartArguments(nullptr, handoff_pid, args)
                                 )))
        {
            return 0;
        }
        regkit::ui::ShowError(nullptr, L"Administrator restart was cancelled.");
    }

    if (!edit_reg_file_requested && !reg_files.empty())
    {
        for (const auto& path : reg_files)
        {
            if (!regkit::ui::ConfirmRegFileMerge(nullptr, path))
            {
                return 0;
            }
            std::wstring error;
            if (!regkit::ImportRegFileFromPath(path, &error))
            {
                regkit::ui::ShowRegFileMergeFailed(nullptr, path, error);
                return 1;
            }
            regkit::ui::ShowRegFileMergeSucceeded(nullptr, path);
        }
        return 0;
    }

    regkit::win32::WaitForParentExit(restart_parent_pid);

    util::UniqueHandle instance_mutex;
    if (startup_settings.single_instance)
    {
        instance_mutex.reset(CreateMutexW(nullptr, TRUE, L"RegKit.SingleInstance"));
        const DWORD mutex_error = GetLastError();
        if (mutex_error == ERROR_ALREADY_EXISTS || mutex_error == ERROR_ACCESS_DENIED)
        {
            HWND existing = FindRunningRegKitWindow();
            if (existing)
            {
                bool handed_off = true;
                const bool has_handoff_data = (external_jump_requested && !startup_jump_target.empty()) ||
                                              (edit_reg_file_requested && !reg_files.empty());
                HWND sender = nullptr;
                if (has_handoff_data)
                {
                    sender =
                        CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
                    if (!sender)
                    {
                        handed_off = false;
                    }
                }
                if (sender && external_jump_requested && !startup_jump_target.empty() &&
                    !SendTextToRegKit(existing, sender, kExternalJumpCopyDataId, startup_jump_target))
                {
                    handed_off = false;
                }
                if (sender && edit_reg_file_requested)
                {
                    for (const auto& path : reg_files)
                    {
                        if (!SendTextToRegKit(existing, sender, kEditRegFileCopyDataId, path))
                        {
                            handed_off = false;
                        }
                    }
                }
                if (sender)
                {
                    DestroyWindow(sender);
                }
                if (handed_off)
                {
                    ShowWindow(existing, SW_RESTORE);
                    SetForegroundWindow(existing);
                    return 0;
                }
            }
        }
    }

    regkit::MainWindow window;
    if (!window.Create(instance))
    {
        regkit::ui::ShowError(nullptr, L"Failed to create the main window.");
        return 1;
    }
    if (external_jump_requested && !startup_jump_target.empty())
    {
        window.QueueExternalJump(startup_jump_target);
    }
    if (edit_reg_file_requested)
    {
        for (const auto& path : reg_files)
        {
            window.OpenRegFileTab(path);
        }
    }
    window.Show(cmd_show);

    MSG msg = {};
    while (true)
    {
        const BOOL available = GetMessageW(&msg, nullptr, 0, 0);
        if (available == 0)
        {
            break;
        }
        if (available == -1)
        {
            regkit::ui::ShowError(nullptr, L"Message loop failed unexpectedly.");
            break;
        }
        if (window.TranslateAccelerator(msg))
        {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    BufferedPaintUnInit();
    return static_cast<int>(msg.wParam);
}
