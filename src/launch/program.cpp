// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "win32/windows_config.h"

#include <windows.h>

#include <algorithm>
#include <commctrl.h>
#include <cwctype>
#include <shellapi.h>
#include <shlobj.h>
#include <optional>
#include <string>
#include <uxtheme.h>
#include <vector>

#include "cli/reg_command.h"
#include "frame/main_window.h"
#include "frame/window/message_ids.h"
#include "regfile/registry_transfer.h"
#include "registry/registry_path.h"
#include "registry/registry_store.h"
#include "ui/feedback.h"
#include "ui/presets.h"
#include "ui/theme.h"
#include "win32/file_text.h"
#include "win32/handle_owner.h"
#include "win32/process_rights.h"
#include "win32/registry_native.h"
#include "win32/restart.h"
#include "win32/shell_integration.h"
#include "win32/shell_paths.h"
#include "win32/system_api.h"
#include "win32/system_error.h"
#include "win32/text_transform.h"
#include "win32/translation.h"
#include "workspace/settings.h"

namespace regkit
{
namespace
{

using frame::message_id::kEditRegFileCopyDataId;
using frame::message_id::kExternalJumpCopyDataId;
using frame::message_id::kRegKitWindowProperty;
using win32::kRestartAdminArg;
using win32::kRestartSystemArg;
using win32::kRestartTiArg;
using win32::kRestartUserArg;
constexpr wchar_t kEditRegFileArg[] = L"--edit-reg";
constexpr wchar_t kInstallEditContextMenuArg[] = L"--install-edit-context-menu";
constexpr wchar_t kUninstallEditContextMenuArg[] = L"--uninstall-edit-context-menu";
constexpr wchar_t kInstallRegEditReplacementArg[] = L"--install-regedit-replacement";
constexpr wchar_t kUninstallRegEditReplacementArg[] = L"--uninstall-regedit-replacement";
constexpr wchar_t kOverrideArg[] = L"--override";

constexpr const wchar_t* kRegEditNames[] = {L"regedit.exe", L"regedit", L"regedt32.exe", L"regedt32"};

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
    const std::wstring dir = win32::RestartDataDir(args);
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
    const std::wstring name = util::FileName(arg);
    return (drive_absolute || unc_absolute) &&
           std::any_of(std::begin(kRegEditNames), std::end(kRegEditNames), [&](const wchar_t* regedit) { return util::EqualsInsensitive(name, regedit); });
}

bool IsInterceptedRegEditLaunch(const std::vector<std::wstring>& args)
{
    return std::any_of(args.begin(), args.end(), IsRegEditLaunchArg);
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

bool LooksLikeRegistryPath(const std::wstring& arg)
{
    RegistryNode node;
    return registry_path::ParseRoot(arg, &node);
}

// only the win32 roots can be checked before the window exists, other paths count as found
bool JumpTargetFound(const std::wstring& target)
{
    RegistryNode node;
    if (!registry_path::ParseRoot(target, &node) || !node.root)
    {
        return true;
    }
    const std::wstring sid = util::GetCurrentUserSidString();
    std::wstring key_path;
    std::wstring value_name;
    bool value_missing = false;
    return registry_path::ResolveJumpTarget(
               target, [&](const std::wstring& path) { return registry_path::Normalize(path, sid); },
               [](const std::wstring& path, RegistryNode* key) { return registry_path::ParseRoot(path, key) && key->root && RegistryStore::KeyExists(*key); },
               &key_path, &value_name, &value_missing) &&
           !value_missing;
}

struct LaunchArgs
{
    std::wstring jump_target;
    std::vector<std::wstring> reg_files;
    std::vector<std::wstring> hive_files;
    bool edit_reg = false;
    std::wstring error;
};

LaunchArgs ParseLaunchArgs(const std::vector<std::wstring>& args)
{
    LaunchArgs launch;
    bool intercepted_regedit = false;
    std::wstring unquoted;
    for (size_t index = 0; index < args.size(); ++index)
    {
        const std::wstring& arg = args[index];
        if (arg.empty())
        {
            continue;
        }
        if (IsRegEditLaunchArg(arg))
        {
            intercepted_regedit = true;
            continue;
        }
        const bool goto_arg = util::EqualsInsensitive(arg, L"--goto") || util::EqualsInsensitive(arg, L"/goto");
        if (goto_arg || win32::ArgTakesValue(arg))
        {
            if (++index >= args.size())
            {
                launch.error = util::TrLabel(L"Missing argument", arg);
                return launch;
            }
            if (goto_arg)
            {
                launch.jump_target = args[index];
            }
            continue;
        }
        if (arg[0] == L'-' || arg[0] == L'/')
        {
            // RegEdit's own switches are accepted and ignored
            const std::wstring_view name = std::wstring_view(arg).substr(1);
            const bool regedit_switch = intercepted_regedit || util::EqualsInsensitive(name, L"m") || util::EqualsInsensitive(name, L"c") ||
                                        util::StartsWithInsensitive(name, L"l:") || util::StartsWithInsensitive(name, L"r:");
            launch.edit_reg = launch.edit_reg || util::EqualsInsensitive(arg, kEditRegFileArg);
            if (!regedit_switch && !launch.edit_reg && !win32::IsInternalRestartArg(arg))
            {
                launch.error = util::TrLabel(L"Invalid option", arg);
                return launch;
            }
            continue;
        }
        if (LooksLikeRegistryPath(arg))
        {
            launch.jump_target = arg;
            continue;
        }
        const bool reg_file = util::HasFileExtension(arg, L".reg") && GetFileAttributesW(arg.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (reg_file || IsHiveFile(arg))
        {
            (reg_file ? launch.reg_files : launch.hive_files).push_back(arg);
            continue;
        }
        if (!launch.jump_target.empty())
        {
            unquoted = (unquoted.empty() ? launch.jump_target : unquoted) + L" " + arg;
            continue;
        }
        launch.error = util::StartsWithInsensitive(arg, L"HK")                      ? util::TrDetail(L"Registry path not found.", arg)
                       : GetFileAttributesW(arg.c_str()) == INVALID_FILE_ATTRIBUTES ? util::TrDetail(L"Registry file not found.", arg)
                                                                                   : util::TrLabel(L"Invalid argument", arg);
        return launch;
    }
    if (!unquoted.empty())
    {
        launch.error = util::TrDetail(L"Registry paths with spaces must be in quotes.", unquoted);
    }
    else if (launch.edit_reg && launch.reg_files.empty() && launch.hive_files.empty())
    {
        launch.error = util::TrLabel(L"Missing argument", kEditRegFileArg);
    }
    std::wstring last_key;
    // RegEdit opens its last key, unless it was deleted since
    if (launch.jump_target.empty() && intercepted_regedit &&
        util::ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\RegEdit", L"LastKey", &last_key) == ERROR_SUCCESS &&
        JumpTargetFound(last_key))
    {
        launch.jump_target = std::move(last_key);
    }
    return launch;
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

workspace::Settings LoadStartupSettings()
{
    workspace::Settings settings;
    const std::wstring folder = util::GetAppDataFolder();
    if (!folder.empty())
    {
        workspace::LoadSettings(util::JoinPath(folder, L"settings.ini"), &settings);
    }
    return settings;
}

void ApplyStartupTheme(const workspace::Settings& settings)
{
    const ThemeMode mode = ParseThemeMode(settings.theme_mode);
    if (mode == ThemeMode::kCustom)
    {
        std::vector<ThemePreset> presets;
        if (!ThemePresetStore::Load(&presets) || presets.empty())
        {
            presets = ThemePresetStore::BuiltInPresets();
        }
        if (const ThemePreset* preset = FindThemePreset(presets, settings.theme_preset))
        {
            Theme::SetCustomColors(preset->colors, preset->is_dark);
        }
    }
    Theme::SetMode(mode);
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

void ShowStartupError(const std::wstring& message)
{
    if (!cli::PrintErrorToTerminal(message))
    {
        ui::ShowError(nullptr, message);
    }
}

bool RestartAs(const RestartTarget& target, DWORD parent_pid, const std::vector<std::wstring>& original_args, int* exit_code)
{
    if (target.is_current())
    {
        return false;
    }
    const std::wstring exe_path = util::GetModulePath();
    if (exe_path.empty())
    {
        ShowStartupError(util::Tr(L"Failed to locate the executable path."));
        return false;
    }
    // keep user arguments while replacing old internal restart flags
    const std::wstring arguments = win32::RestartArguments(target.arg, parent_pid, original_args);
    *exit_code = 0;
    if (!util::IsProcessElevated())
    {
        if (SUCCEEDED(win32::LaunchElevated(nullptr, exe_path, arguments)))
        {
            return true;
        }
        ShowStartupError(util::Tr(target.request_failure));
        return false;
    }
    DWORD error = 0;
    bool impersonation_lost = false;
    const bool launched = target.launch(L"\"" + exe_path + L"\" " + arguments, L"", &error, &impersonation_lost);
    if (impersonation_lost)
    {
        ShowStartupError(util::Tr(L"RegKit couldn't restore its own security context and must close now."));
        *exit_code = launched ? 0 : 1;
        return true;
    }
    if (!launched)
    {
        const std::wstring detail = util::FormatWin32Error(error);
        ShowStartupError(detail.empty() ? util::Tr(target.launch_failure) : util::TrDetail(target.launch_failure, detail));
    }
    return launched;
}

std::optional<int> RunShellIntegrationCommand(const std::vector<std::wstring>& args)
{
    const bool install_menu = HasCommandLineArg(args, kInstallEditContextMenuArg);
    const bool uninstall_menu = HasCommandLineArg(args, kUninstallEditContextMenuArg);
    const bool install_replacement = HasCommandLineArg(args, kInstallRegEditReplacementArg);
    if (!install_menu && !uninstall_menu && !install_replacement && !HasCommandLineArg(args, kUninstallRegEditReplacementArg))
    {
        return std::nullopt;
    }
    const std::wstring exe_path = util::GetModulePath();
    if (exe_path.empty())
    {
        return 1;
    }
    LONG result = ERROR_SUCCESS;
    if (install_menu)
    {
        result = win32::SetEditMenu(exe_path, true);
    }
    else if (uninstall_menu)
    {
        result = win32::RemoveEditMenuIfOwned(exe_path);
    }
    else
    {
        bool conflict = false;
        result = win32::SetRegEditReplacement(exe_path, install_replacement, &conflict, HasCommandLineArg(args, kOverrideArg));
        if (result != ERROR_SUCCESS && conflict)
        {
            return 2;
        }
    }
    return result == ERROR_SUCCESS ? 0 : 1;
}

int MergeRegFiles(const std::vector<std::wstring>& reg_files)
{
    for (const auto& path : reg_files)
    {
        if (!ui::ConfirmRegFileMerge(nullptr, path))
        {
            return 0;
        }
        std::wstring error;
        if (!ImportRegFileFromPath(path, &error))
        {
            ui::ShowRegFileMergeFailed(nullptr, path, error);
            return 1;
        }
        ui::ShowRegFileMergeSucceeded(nullptr, path);
    }
    return 0;
}

bool HandOffToRunningInstance(HINSTANCE instance, const std::wstring& jump_target, const std::vector<std::wstring>& edit_files)
{
    HWND existing = FindRunningRegKitWindow();
    if (!existing)
    {
        return false;
    }
    bool handed_off = true;
    if (!jump_target.empty() || !edit_files.empty())
    {
        HWND sender = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        handed_off = sender != nullptr;
        if (!jump_target.empty())
        {
            handed_off = SendTextToRegKit(existing, sender, kExternalJumpCopyDataId, jump_target) && handed_off;
        }
        for (const auto& path : edit_files)
        {
            handed_off = SendTextToRegKit(existing, sender, kEditRegFileCopyDataId, path) && handed_off;
        }
        if (sender)
        {
            DestroyWindow(sender);
        }
    }
    if (handed_off)
    {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
    }
    return handed_off;
}

int RunMessageLoop(MainWindow& window)
{
    MSG msg = {};
    for (BOOL available; (available = GetMessageW(&msg, nullptr, 0, 0)) != 0;)
    {
        if (available == -1)
        {
            ui::ShowError(nullptr, util::Tr(L"Message loop failed unexpectedly."));
            return 1;
        }
        if (!window.TranslateAccelerator(msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

void ApplySafeDllSearchPolicy()
{
    // resolve this at runtime as older winvers may not export it
    const auto set_directories = win32::ImportProc<BOOL(WINAPI*)(DWORD)>(L"kernel32.dll", "SetDefaultDllDirectories");
    if (set_directories)
    {
        set_directories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS);
    }
    // remove current dir from legacy DLL search path
    SetDllDirectoryW(L"");
}

int Run(HINSTANCE instance, int cmd_show)
{
    ApplySafeDllSearchPolicy();
    const auto args = GetCommandLineArgs();
    if (const std::optional<int> exit_code = RunShellIntegrationCommand(args))
    {
        return *exit_code;
    }

    Theme::InitializeDarkModeSupport();
    util::ComInit com;
    if (!com.ok())
    {
        ShowStartupError(L"COM initialization failed.");
        return 1;
    }

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_DATE_CLASSES |
                ICC_COOL_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);
    BufferedPaintInit();

    ApplyDataDirOverride(args);
    int cli_exit = 0;
    if (cli::Execute(IsInterceptedRegEditLaunch(args) ? StripRegEditLaunchArg(args) : args, &cli_exit))
    {
        return cli_exit;
    }
    const workspace::Settings startup_settings = LoadStartupSettings();
    util::LoadLanguage(startup_settings.language);
    ApplyStartupTheme(startup_settings);
    LaunchArgs launch = ParseLaunchArgs(args);
    if (!launch.error.empty())
    {
        ShowStartupError(launch.error);
        return 1;
    }
    // without a terminal the window offers the nearest key instead
    if (!launch.jump_target.empty() && !JumpTargetFound(launch.jump_target) &&
        cli::PrintErrorToTerminal(util::TrDetail(L"Registry path not found.", launch.jump_target)))
    {
        return 1;
    }
    const bool stay_as_user = HasCommandLineArg(args, kRestartUserArg);
    const DWORD restart_parent_pid = win32::RestartParentPid(args);
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
        if (!exe_path.empty() && SUCCEEDED(win32::LaunchElevated(nullptr, exe_path, win32::RestartArguments(nullptr, handoff_pid, args))))
        {
            return 0;
        }
        ShowStartupError(util::Tr(L"Administrator restart was cancelled."));
    }

    if (!launch.edit_reg && !launch.reg_files.empty())
    {
        return MergeRegFiles(launch.reg_files);
    }

    win32::WaitForParentExit(restart_parent_pid);

    std::vector<std::wstring>& edit_files = launch.hive_files;
    if (launch.edit_reg)
    {
        edit_files.insert(edit_files.begin(), launch.reg_files.begin(), launch.reg_files.end());
    }
    util::UniqueHandle instance_mutex;
    if (startup_settings.single_instance)
    {
        instance_mutex.reset(CreateMutexW(nullptr, TRUE, L"RegKit.SingleInstance"));
        const DWORD mutex_error = GetLastError();
        if ((mutex_error == ERROR_ALREADY_EXISTS || mutex_error == ERROR_ACCESS_DENIED) &&
            HandOffToRunningInstance(instance, launch.jump_target, edit_files))
        {
            return 0;
        }
    }

    MainWindow window;
    if (!window.Create(instance))
    {
        ShowStartupError(util::Tr(L"Failed to create the main window."));
        return 1;
    }
    if (!launch.jump_target.empty())
    {
        window.QueueExternalJump(launch.jump_target);
    }
    for (const auto& path : edit_files)
    {
        window.OpenFile(path);
    }
    window.Show(cmd_show);
    const int exit_code = RunMessageLoop(window);
    BufferedPaintUnInit();
    return exit_code;
}

} // namespace
} // namespace regkit

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int cmd_show)
{
    return regkit::Run(instance, cmd_show);
}
