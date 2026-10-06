// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/window_detail.h"
#include "frame/window_impl.h"

#include "dialogs/table_dialog.h"
#include "registry/key_access.h"
#include "registry/resource_list.h"
#include "registry/value_format.h"
#include "win32/shell_integration.h"
#include "win32/translation.h"

namespace regkit
{
using namespace window_detail;

void MainWindow::Impl::ShowPermissionsDialog(const RegistryNode& node)
{
    ShowRegistryPermissions(hwnd_, node);
}

namespace
{

std::wstring IntegrityLevelName(BYTE* descriptor)
{
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL sacl = nullptr;
    SYSTEM_MANDATORY_LABEL_ACE* ace = nullptr;
    // unlabeled keys are treated as medium integrity
    if (!GetSecurityDescriptorSacl(descriptor, &present, &sacl, &defaulted) || !present || !sacl ||
        !GetAce(sacl, 0, reinterpret_cast<void**>(&ace)) || ace->Header.AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
    {
        return util::Tr(L"Not set (medium)");
    }
    const PSID sid = &ace->SidStart;
    const DWORD rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1u);
    static constexpr std::pair<DWORD, const wchar_t*> kLevels[] = {
        {SECURITY_MANDATORY_UNTRUSTED_RID, util::TrNoop(L"Untrusted")},
        {SECURITY_MANDATORY_LOW_RID, util::TrNoop(L"Low")},
        {SECURITY_MANDATORY_MEDIUM_RID, util::TrNoop(L"Medium")},
        {SECURITY_MANDATORY_HIGH_RID, util::TrNoop(L"High")},
        {SECURITY_MANDATORY_SYSTEM_RID, util::TrNoop(L"System")},
        {SECURITY_MANDATORY_PROTECTED_PROCESS_RID, util::TrNoop(L"Protected process")},
    };
    for (const auto& [level, name] : kLevels)
    {
        if (rid == level)
        {
            return util::Tr(name);
        }
    }
    wchar_t text[16] = {};
    swprintf_s(text, L"0x%lX", rid);
    return text;
}

} // namespace

void MainWindow::Impl::ShowKeyInfoDialog(const RegistryNode& node)
{
    editors::TablesRequest request;
    request.title = util::Tr(L"Key Information");
    request.identifier = registry_path::DisplayName(registry_path::Build(node));
    KeyDetails details;
    if (!RegistryStore::QueryKeyDetails(node, &details))
    {
        ui::ShowError(hwnd_, util::Tr(L"The key information couldn't be read.") + std::wstring(L"\n") + request.identifier);
        return;
    }
    const auto yes_no = [](bool value) { return std::wstring(value ? util::Tr(L"Yes") : util::Tr(L"No")); };
    const auto bits = [](ULONG value, std::initializer_list<std::pair<ULONG, const wchar_t*>> names) {
        std::wstring text;
        for (const auto& [bit, name] : names)
        {
            if (value & bit)
            {
                text.append(text.empty() ? L"" : L", ").append(util::Tr(name));
            }
        }
        return text.empty() ? std::wstring(util::Tr(L"None")) : text;
    };
    const std::vector<std::wstring> property_columns = {util::Tr(L"Property"), util::Tr(L"Value")};
    records::Table general = {util::Tr(L"General"), property_columns, {}};
    records::Table contents = {util::Tr(L"Contents"), property_columns, {}};
    records::Table flags = {util::Tr(L"Flags"), property_columns, {}};
    const auto add = [](records::Table& table, const wchar_t* label, std::wstring value) { table.rows.push_back({label, std::move(value)}); };

    const util::NativeKeyInfo& native = details.native;
    add(general, util::Tr(L"Name"), registry_path::DisplayName(LeafName(node)));
    if (!native.native_name.empty())
    {
        add(general, util::Tr(L"Native name"), registry_path::DisplayName(native.native_name));
    }
    if (node.root == HKEY_CLASSES_ROOT && registry_path::ClassesSource(native.native_name) != ClassSource::kNone)
    {
        add(general, util::Tr(L"Source"), registry_path::ClassesSource(native.native_name) == ClassSource::kMachine ? util::Tr(L"Machine") : util::Tr(L"User"));
    }
    bool hive_root = false;
    const std::wstring hive = native.native_name.empty() ? LookupHivePath(node, &hive_root) : LookupNativeHivePath(native.native_name, &hive_root);
    if (!hive.empty())
    {
        add(general, util::Tr(L"Hive file"), hive);
        add(general, util::Tr(L"Hive root"), yes_no(hive_root));
    }
    std::wstring class_name = details.class_name.empty() ? std::wstring(util::Tr(L"None")) : registry_path::DisplayName(details.class_name);
    if (std::any_of(details.class_name.begin(), details.class_name.end(), [](wchar_t ch) { return !iswprint(ch); }))
    {
        class_name.append(L" (").append(util::ToHex(std::span(reinterpret_cast<const BYTE*>(details.class_name.data()), details.class_name.size() * sizeof(wchar_t)), L' ', true)).append(L")");
    }
    add(general, util::Tr(L"Class name"), class_name);
    FILETIME local = {};
    SYSTEMTIME time = {};
    if (FileTimeToLocalFileTime(&details.info.last_write, &local) && FileTimeToSystemTime(&local, &time))
    {
        add(general, util::Tr(L"Last write time"), util::FormatLocalTime(time, true));
    }
    if (FileTimeToSystemTime(&details.info.last_write, &time))
    {
        add(general, util::Tr(L"Last write time (UTC)"), util::FormatLocalTime(time, true));
    }

    add(contents, util::Tr(L"Subkeys"), std::to_wstring(details.info.subkey_count));
    add(contents, util::Tr(L"Values"), std::to_wstring(details.info.value_count));
    add(contents, util::Tr(L"Longest subkey name"), std::to_wstring(details.max_subkey_name));
    add(contents, util::Tr(L"Longest class name"), std::to_wstring(details.max_class));
    add(contents, util::Tr(L"Longest value name"), std::to_wstring(details.max_value_name));
    add(contents, util::Tr(L"Largest value data"), value_format::ByteCount(details.max_value_data));

    if (native.key_flags || !native.native_name.empty())
    {
        add(flags, util::Tr(L"Volatile"), yes_no((native.key_flags.value_or(0) & util::kKeyFlagVolatile) || registry_path::InVolatileHive(native.native_name)));
    }
    std::wstring target;
    add(flags, util::Tr(L"Symbolic link"), RegistryStore::QuerySymbolicLinkTarget(node, &target) ? target : yes_no(false));
    if (win32::HasAlternateView() && session_->mode == RegistryMode::kLocal && !native.native_name.empty())
    {
        // the same path opened in the other view lands on the same key unless wow64 redirects it
        RegistryNode other = node;
        other.view = node.view ? 0 : win32::kAlternateRegistryView;
        KeyDetails other_details;
        std::wstring wow64 = util::Tr(L"Not in the other view");
        if (RegistryStore::QueryKeyDetails(other, &other_details) && !other_details.native.native_name.empty())
        {
            const std::wstring& path32 = node.view ? native.native_name : other_details.native.native_name;
            wow64 = util::EqualsInsensitive(native.native_name, other_details.native.native_name) ? std::wstring(util::Tr(L"Shared by both views")) : util::TrLabel(L"32-bit view", registry_path::DisplayName(path32));
        }
        add(flags, L"WOW64", wow64);
    }
    static constexpr std::pair<ULONG, const wchar_t*> kControlFlags[] = {
        {util::kKeyDontVirtualize, util::TrNoop(L"Don't virtualize")}, {util::kKeyDontSilentFail, util::TrNoop(L"Don't silent fail")}, {util::kKeyRecurseFlag, util::TrNoop(L"Recurse")}};
    // reg flags only accepts HKLM\SOFTWARE keys, the same keys uac virtualizes
    const std::wstring store = registry_path::VirtualStorePath(native.native_name);
    const bool editable_flags = native.control_flags && !store.empty() && session_->mode == RegistryMode::kLocal && !settings_.read_only;
    if (native.control_flags && !editable_flags)
    {
        add(flags, util::Tr(L"Virtualization flags"), bits(*native.control_flags, {kControlFlags[0], kControlFlags[1], kControlFlags[2]}));
    }
    if (!store.empty())
    {
        RegistryNode store_node;
        KeyInfo store_info;
        add(flags, util::Tr(L"Virtual store copy"), registry_path::ParseRoot(store, &store_node) && RegistryStore::QueryKeyInfo(store_node, &store_info) ? store : std::wstring(util::Tr(L"None")));
    }
    if (native.virtualization)
    {
        add(flags, util::Tr(L"Virtualization state"), bits(*native.virtualization, {{0x1, util::TrNoop(L"Candidate")}, {0x2, util::TrNoop(L"Enabled")}, {0x4, util::TrNoop(L"Virtual target")}, {0x8, util::TrNoop(L"Virtual store")}, {0x10, util::TrNoop(L"Virtual source")}}));
    }
    if (native.trust)
    {
        add(flags, util::Tr(L"Trusted key"), yes_no(*native.trust & 0x1));
    }
    if (native.layer)
    {
        add(flags, util::Tr(L"Layer"), bits(*native.layer, {{0x1, util::TrNoop(L"Tombstone")}, {0x2, util::TrNoop(L"Supersede local")}, {0x4, util::TrNoop(L"Supersede tree")}, {0x8, util::TrNoop(L"Class is inherited")}}));
    }
    SECURITY_INFORMATION parts = LABEL_SECURITY_INFORMATION;
    std::vector<BYTE> descriptor;
    if (RegistryStore::ReadKeySecurity(node, &parts, &descriptor) && parts == LABEL_SECURITY_INFORMATION)
    {
        add(flags, util::Tr(L"Integrity level"), IntegrityLevelName(descriptor.data()));
    }
    request.tables = {std::move(general), std::move(contents), std::move(flags)};
    auto control_flags = std::make_shared<ULONG>(native.control_flags.value_or(0));
    if (editable_flags)
    {
        records::Table virtualization = {util::Tr(L"Virtualization flags"), {util::Tr(L"Flag")}, {}};
        for (const auto& [bit, name] : kControlFlags)
        {
            virtualization.rows.push_back({util::Tr(name)});
            request.checked.push_back((*control_flags & bit) != 0);
        }
        request.check_table = static_cast<int>(request.tables.size());
        request.tables.push_back(std::move(virtualization));
        request.on_check = [node, control_flags](HWND dialog, size_t row, bool checked) {
            const ULONG flags = checked ? *control_flags | kControlFlags[row].first : *control_flags & ~kControlFlags[row].first;
            const LONG status = RegistryStore::SetKeyControlFlags(node, flags);
            if (status != ERROR_SUCCESS)
            {
                ui::ShowError(dialog, FormatWin32Error(status));
                return false;
            }
            *control_flags = flags;
            return true;
        };
    }

    SECURITY_INFORMATION access_parts = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    std::vector<BYTE> access_descriptor;
    if (RegistryStore::ReadKeySecurity(node, &access_parts, &access_descriptor))
    {
        // which run level is needed to write here, so the user can pick it before trying
        records::Table access = {util::Tr(L"Access"), {util::Tr(L"Run level"), util::Tr(L"Access"), util::Tr(L"Can write")}, {}};
        for (const key_access::RunLevel& level : key_access::ForRunLevels(access_descriptor.data()))
        {
            access.rows.push_back({util::Tr(level.name), key_access::Describe(level.granted), yes_no(key_access::CanWrite(level.granted))});
        }
        request.tables.push_back(std::move(access));
    }
    request.action_label = util::Tr(L"Permissions...");
    if (!settings_.read_only)
    {
        request.action = [node](HWND owner) { ShowRegistryPermissions(owner, node); };
    }
    editors::ShowTables(hwnd_, request);
}

bool MainWindow::Impl::ShowResourceList(const RegistryValue& value)
{
    editors::TablesRequest request;
    request.title = value_format::TypeName(value.type);
    request.identifier = value.name.empty() ? std::wstring(util::Tr(L"(Default)")) : registry_path::DisplayName(value.name);
    auto tables = resource_list::Decode(value_format::NormalizeType(value.type), value.data.data(), value.data.size(), util::EndsWithInsensitive(value.name, L".Translated"));
    request.tables = tables ? std::move(*tables) : std::vector<records::Table>{{util::Tr(L"Error"), {util::Tr(L"Error")}, {{util::Tr(L"The resource data is malformed.")}}}};
    bool edit_binary = false;
    request.action_label = util::Tr(L"Edit Binary...");
    request.action = [&edit_binary](HWND dialog) {
        edit_binary = true;
        EndDialog(dialog, IDCANCEL);
    };
    editors::ShowTables(hwnd_, request);
    return edit_binary;
}

namespace
{

bool RestartExePath(HWND owner, std::wstring* exe_path)
{
    *exe_path = util::GetModulePath();
    if (!exe_path->empty())
    {
        return true;
    }
    ui::ShowError(owner, util::Tr(L"Failed to locate the executable path."));
    return false;
}

std::wstring WithErrorDetail(std::wstring message, DWORD error)
{
    const std::wstring detail = FormatWin32Error(error);
    return detail.empty() ? message : message + L"\n" + detail;
}

bool BeginRestart(HWND owner, const wchar_t* target_arg, const wchar_t* failure)
{
    std::wstring exe_path;
    if (!RestartExePath(owner, &exe_path))
    {
        return false;
    }
    // pass current PID so the replacement waits for this instance to exit
    const HRESULT hr =
        win32::LaunchElevated(owner, exe_path, win32::RestartArguments(target_arg, GetCurrentProcessId()));
    if (FAILED(hr))
    {
        if (!win32::DialogCancelled(hr))
        {
            ui::ShowError(owner, std::wstring(failure) + L"\n" + win32::FormatDialogError(hr));
        }
        return false;
    }
    PostMessageW(owner, WM_CLOSE, 0, 0);
    return true;
}

bool BrokerRestart(HWND owner, const wchar_t* target_arg, const wchar_t* failure, bool (*launch)(const std::wstring&, const std::wstring&, DWORD*, bool*))
{
    std::wstring exe_path;
    if (!RestartExePath(owner, &exe_path))
    {
        return false;
    }
    const std::wstring command_line =
        L"\"" + exe_path + L"\" " + win32::RestartArguments(target_arg, GetCurrentProcessId());
    DWORD error = 0;
    bool impersonation_lost = false;
    const bool launched = launch(command_line, L"", &error, &impersonation_lost);
    if (impersonation_lost)
    {
        ui::ShowError(owner, WithErrorDetail(util::Tr(L"RegKit couldn't restore its own security context and must close now."), error));
        ExitProcess(launched ? 0u : 1u);
    }
    if (!launched)
    {
        ui::ShowError(owner, WithErrorDetail(failure, error));
        return false;
    }
    PostMessageW(owner, WM_CLOSE, 0, 0);
    return true;
}

} // namespace

void MainWindow::Impl::PrepareSessionHandover()
{
    CaptureRegistryTabState(tab_ ? TabCtrl_GetCurSel(tab_) : -1);
    SaveSessionTabs();
    SaveSettings();
}

bool MainWindow::Impl::SaveSessionForRestart()
{
    CaptureRegistryTabState(tab_ ? TabCtrl_GetCurSel(tab_) : -1);
    if (SaveSessionTabs())
    {
        return true;
    }
    ui::ShowError(hwnd_, util::Tr(L"The current session couldn't be saved for the restart."));
    return false;
}

bool MainWindow::Impl::LaunchRestart(bool restore_session)
{
    if (ui::LaunchNewInstance(win32::RestartArguments(nullptr, GetCurrentProcessId(), restore_session)))
    {
        return true;
    }
    ui::ShowError(hwnd_, util::Tr(L"RegKit couldn't be restarted."));
    return false;
}

bool MainWindow::Impl::RestartCurrentInstance()
{
    if (!SaveSessionForRestart())
    {
        return false;
    }
    SaveSettings();
    return LaunchRestart(true);
}

bool MainWindow::Impl::RestartAfterCacheClear(CacheKind kind)
{
    // dont restore tab data when its cache was cleared
    const bool restore_session = kind != CacheKind::kAll && kind != CacheKind::kTabs;
    // tabs carry their own tree state
    if (kind == CacheKind::kTreeState)
    {
        for (TabEntry& entry : tabs_)
        {
            entry.selected_path.clear();
            entry.expanded_paths.clear();
        }
        ResetRegistryTreeState();
    }
    if (restore_session && !SaveSessionForRestart())
    {
        return false;
    }
    SaveSettings();
    if (!ClearCache(kind, false))
    {
        // continue tree state saving when the restart doesnt complete
        if ((kind == CacheKind::kAll || kind == CacheKind::kTreeState) && settings_.save_tree_state)
        {
            StartTreeStateWorker();
        }
        ui::ShowError(hwnd_, util::Tr(L"One or more cache files couldn't be removed."));
        return false;
    }
    if (!LaunchRestart(restore_session))
    {
        if ((kind == CacheKind::kAll || kind == CacheKind::kTreeState) && settings_.save_tree_state)
        {
            StartTreeStateWorker();
        }
        return false;
    }
    restart_on_close_ = true;
    return true;
}

bool MainWindow::Impl::RestartAfterSettingsReset()
{
    if (!SaveSessionForRestart())
    {
        return false;
    }
    const std::wstring path = SettingsPath();
    if (path.empty())
    {
        ui::ShowError(hwnd_, util::Tr(L"Failed to find the settings file."));
        return false;
    }
    if (!workspace::SaveSettings(path, workspace::DefaultOptions(CurrentSettings())))
    {
        ui::ShowError(hwnd_, util::Tr(L"The settings file couldn't be reset."));
        return false;
    }
    if (!LaunchRestart(true))
    {
        // recreate settings when the replacement process couldnt start
        SaveSettings();
        return false;
    }
    return true;
}

bool MainWindow::Impl::RestartAsAdmin()
{
    PrepareSessionHandover();
    if (util::IsProcessSystem() || util::IsProcessTrustedInstaller())
    {
        // return through the signed in shell before requesting admin access
        return BrokerRestart(hwnd_, kRestartAdminArg, util::Tr(L"Failed to restart with administrator rights."), util::LaunchProcessAsShellUser);
    }
    return BeginRestart(hwnd_, nullptr, util::Tr(L"Failed to restart with administrator rights."));
}

bool MainWindow::Impl::RestartAsUser()
{
    PrepareSessionHandover();
    return BrokerRestart(hwnd_, kRestartUserArg, util::Tr(L"Failed to restart as the signed-in user."), util::LaunchProcessAsShellUser);
}

bool MainWindow::Impl::RestartAsSystem()
{
    PrepareSessionHandover();
    if (!util::IsProcessElevated())
    {
        return BeginRestart(hwnd_, kRestartSystemArg, util::Tr(L"Failed to request SYSTEM restart."));
    }
    return BrokerRestart(hwnd_, kRestartSystemArg, util::Tr(L"Failed to restart with SYSTEM rights."), util::LaunchProcessAsSystem);
}

bool MainWindow::Impl::RestartAsTrustedInstaller()
{
    PrepareSessionHandover();
    if (!util::IsProcessElevated())
    {
        return BeginRestart(hwnd_, kRestartTiArg, util::Tr(L"Failed to request TrustedInstaller restart."));
    }
    return BrokerRestart(hwnd_, kRestartTiArg, util::Tr(L"Failed to restart with TrustedInstaller rights."), util::LaunchProcessAsTrustedInstaller);
}

void MainWindow::Impl::ReplaceRegEdit(bool enable)
{
    std::wstring exe_path = util::GetModulePath();
    if (exe_path.empty())
    {
        ui::ShowError(hwnd_, util::Tr(L"Failed to locate the executable path."));
        return;
    }
    // reject machine wide redirection to an executable another user can replace
    const bool writable_location = enable && util::IsWritableByNonAdmins(exe_path);
    if (writable_location &&
        ui::PromptKeyChoice(
            hwnd_,
            util::Tr(L"Replacing RegEdit registers this executable for every account on the machine.\n\n"
                     L"RegKit is running from a location that non administrators can write to, so a program "
                     L"without administrator rights could replace it and run whenever anyone starts RegEdit. "
                     L"Install RegKit for all users first, or move it somewhere only administrators can write.\n\n"
                     L"Replace anyway to apply it from this location."),
            exe_path,
            util::Tr(L"Replace RegEdit"),
            util::Tr(L"Replace Anyway"),
            L"",
            util::Tr(L"Cancel"),
            {110, 70, 70}
        ) != IDYES)
    {
        return;
    }

    bool conflict = false;
    LONG result = win32::SetRegEditReplacement(exe_path, enable, &conflict, false, writable_location);
    // dont overwrite another debugger registration without approval
    if (result != ERROR_SUCCESS && conflict && enable)
    {
        const int choice = ui::PromptChoice(hwnd_, util::Tr(L"RegEdit already has a Debugger entry owned by another program.\n\n"
                                                            L"Override the existing entry?"),
                                            util::Tr(L"Replace RegEdit"),
                                            util::Tr(L"Override"),
                                            L"",
                                            util::Tr(L"Cancel"),
                                            {80, 70, 70});
        if (choice == IDYES)
        {
            result = win32::SetRegEditReplacement(exe_path, true, nullptr, true, writable_location);
            conflict = false;
        }
        else
        {
            result = ERROR_CANCELLED;
        }
    }
    if (result != ERROR_SUCCESS)
    {
        if (result != ERROR_CANCELLED)
        {
            ui::ShowError(hwnd_, FormatWin32Error(result));
        }
    }
}

void MainWindow::Impl::SetEditContextMenu(bool enable)
{
    LONG cleanup_result = ERROR_SUCCESS;
    const LONG result = win32::SetRegFileEditMenu(util::GetModulePath(), enable, &cleanup_result);
    if (result != ERROR_SUCCESS)
    {
        std::wstring message = FormatWin32Error(result);
        if (cleanup_result != ERROR_SUCCESS)
        {
            message += L"\nThe incomplete context menu entry couldn't be removed:\n";
            message += FormatWin32Error(cleanup_result);
        }
        ui::ShowError(hwnd_, message);
    }
}

std::wstring MainWindow::Impl::ResolveSelectedHiveFilePath()
{
    if (session_->mode == RegistryMode::kRemote)
    {
        return L"";
    }
    RegistryNode* node = browse_.current_node();
    if (!node && browse_.tree().hwnd())
    {
        HTREEITEM selected = TreeView_GetSelection(browse_.tree().hwnd());
        if (selected)
        {
            node = browse_.tree().NodeFromItem(selected);
        }
    }
    if (!node)
    {
        return L"";
    }
    RegistryNode target = *node;
    int index = browse_.values().hwnd() ? ListView_GetNextItem(browse_.values().hwnd(), -1, LVNI_SELECTED) : -1;
    if (index >= 0)
    {
        const ListRow* row = browse_.values().RowAt(index);
        if (row && row->kind == rowkind::kKey && !row->extra.empty())
        {
            target = ChildNode(*node, row->extra);
        }
    }
    return LookupHivePath(target, nullptr);
}

void MainWindow::Impl::OpenHiveFileDir()
{
    if (session_->mode == RegistryMode::kRemote)
    {
        ui::ShowError(hwnd_, util::Tr(L"Hive files aren't available for remote registries."));
        return;
    }
    std::wstring hive_path = ResolveSelectedHiveFilePath();
    if (hive_path.empty())
    {
        ui::ShowError(hwnd_, util::Tr(L"No hive file was found for this key."));
        return;
    }
    const HRESULT hr = win32::RevealInExplorer(hive_path);
    if (FAILED(hr))
    {
        ui::ShowError(hwnd_, win32::FormatDialogError(hr));
    }
}

LOGFONTW MainWindow::Impl::DefaultLogFont() const
{
    return ui::SystemUIFontLogFont();
}

} // namespace regkit
