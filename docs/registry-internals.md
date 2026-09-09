# Registry Internals

## Registry Fundamentals

### Standard hives & REGISTRY Comparison

RegEdit shows five common hives `HKEY_LOCAL_MACHINE`, `HKEY_USERS`, `HKEY_CURRENT_USER`, `HKEY_CLASSES_ROOT`, and `HKEY_CURRENT_CONFIG`. Internally, all registry keys are rooted at a single object named `\REGISTRY` in the Object Manager namespace, native APIs (`NtOpenKey`/`ZwOpenKey`) can access paths under `\REGISTRY` directly. The registry actually exposes nine root keys (including performance and local settings roots) but most tools only show the common five.

You can query the REGISTRY key using WinDbg `!reg query \REGISTRY`.

### REGISTRY only Keys

Keys that exist in the real REGISTRY view but are not reachable from standard hives:

- `\REGISTRY\A` - private keys used by some processes, including UWP apps
- `\REGISTRY\WC` - Windows Containers / silos, used by modern registry virtualization and differencing hives

### Keys, values, naming

The registry is a database that looks a lot like a filesystem, keys are like directories, values are like files, and a key can contain both subkeys and values. Values are typed, have a name, and live under a key. Each key also has one unnamed value, displayed as `(Default)`.

### Registry value types

Most values are `REG_DWORD`, `REG_BINARY`, or `REG_SZ`, but the registry supports 12 value types.

Some values are stored with extra flag bits in the upper 16 bits (e.g. `0x20000`, `0x40000`). These aren't new base types, the actual base type is `type & 0xFFFF`, and regkit displays them as `REG_* (0xXXXX)` (for example `0x20001` is `REG_SZ` with a flag, `0x20004` is `REG_DWORD`, and `0x40007` is `REG_MULTI_SZ`). These flagged types are included in the '*Find > Data Types filter*'. Note that this is currently my personal assumption and isn't validated by any official documentation (CM doesn't seem to handle them like that).

| Type | Description |
| --- | --- |
| `REG_NONE` | No value type |
| `REG_SZ` | Fixed length Unicode string |
| `REG_MULTI_SZ` | Array of Unicode NULL terminated strings |
| `REG_EXPAND_SZ` | Variable length Unicode string with embedded environment variables |
| `REG_BINARY` | Arbitrary length binary data |
| `REG_DWORD` | 32 bit number |
| `REG_QWORD` | 64 bit number |
| `REG_DWORD_BIG_ENDIAN` | 32 bit number, high byte first |
| `REG_LINK` | Unicode symbolic link |
| `REG_RESOURCE_LIST` | Hardware resource description |
| `REG_FULL_RESOURCE_DESCRIPTOR` | Hardware resource description |
| `REG_RESOURCE_REQUIREMENTS_LIST` | Resource requirements |

### Root keys and logical structure

There are nine root keys, their names start with `HKEY` as they represent handles (H) to keys (KEY), some are links or merged views.

| Root key | Abbreviation | Description | Link |
| --- | --- | --- | --- |
| `HKEY_CURRENT_USER` | `HKCU` | Per-user preferences (current logged-on user) | `HKEY_USERS\<SID>` (SID of current logged-on user) |
| `HKEY_CURRENT_USER_LOCAL_SETTINGS` | `HKCULS` | Per-user settings local to the machine | `HKCU\Software\Classes\Local Settings` |
| `HKEY_USERS` | `HKU` | All loaded user profiles (including `.DEFAULT` for the system account) | - |
| `HKEY_CLASSES_ROOT` | `HKCR` | "Stores file association and Component Object Model (COM) object registration information" | Merged view of `HKLM\SOFTWARE\Classes` and `HKEY_USERS\<SID>\SOFTWARE\Classes` |
| `HKEY_LOCAL_MACHINE` | `HKLM` | Machine-wide configuration (BCD, COMPONENTS, HARDWARE, SAM, SECURITY, SOFTWARE, SYSTEM) | - |
| `HKEY_CURRENT_CONFIG` | `HKCC` | Stores some information about the current hardware profile (deprecated, "Hardware profiles are no longer supported in Windows, but the key still exists to support legacy applications that might depend on its presence.") | `HKLM\SYSTEM\CurrentControlSet\Hardware Profiles\Current` (legacy, Yosifovich shows `Hardware\Profiles\Current`, but that's a typo in his blog) |
| `HKEY_PERFORMANCE_DATA` | `HKPD` | Live performance counter data, available only via APIs | - |
| `HKEY_PERFORMANCE_TEXT` | `HKPT` | Performance counter names/descriptions in US English | - |
| `HKEY_PERFORMANCE_NLSTEXT` | `HKPNT` | Performance counter names/descriptions in the OS language | - |

Notes:
- `HKEY_CURRENT_USER` maps to the logged on user hive (`Ntuser.dat`) and is created per user at logon
- `HKEY_CLASSES_ROOT` also contains UAC VirtualStore data, it isn't a simple link
- `HKEY_PERFORMANCE_*` keys aren't stored in hive files and aren't visible in Regedit, they're provided by Perflib through registry APIs like `RegQueryValueEx`
- SYSTEM = `S-1-5-18`, LocalService = `S-1-5-19`, NetworkService = `S-1-5-20`

Low level view of the REGISTRY ([*](https://projectzero.google/2024/10/the-windows-registry-adventure-4-hives.html)):

![](https://github.com/nohuto/regkit/blob/main/assets/images/REGISTRYview.png?raw=true)

### Hives and on-disk files

On disk, the registry is a set of hive files, the Configuration Manager records loaded hive paths under `HKLM\SYSTEM\CurrentControlSet\Control\Hivelist` (WinDbg cmd to get HiceAddr etc. = `!reg hivelist`) as they are mounted. Each (nonvolatile) hive is a PRIMARY file plus `.LOG<1/2>` (also possible to only be a `.LOG` if REG_HIVE_SINGLE_LOG) used during flushing/crash recovery.

| Hive registry path | Hive file path |
| --- | --- |
| `HKEY_LOCAL_MACHINE\BCD00000000` | `\EFI\Microsoft\Boot\BCD` |
| `HKEY_LOCAL_MACHINE\COMPONENTS` | `%SystemRoot%\System32\Config\Components` |
| `HKEY_LOCAL_MACHINE\SYSTEM` | `%SystemRoot%\System32\Config\System` |
| `HKEY_LOCAL_MACHINE\SAM` | `%SystemRoot%\System32\Config\Sam` |
| `HKEY_LOCAL_MACHINE\SECURITY` | `%SystemRoot%\System32\Config\Security` |
| `HKEY_LOCAL_MACHINE\SOFTWARE` | `%SystemRoot%\System32\Config\Software` |
| `HKEY_LOCAL_MACHINE\HARDWARE` | Volatile hive (memory only) |
| `HKEY_LOCAL_MACHINE\WindowsAppLockerCache` | `%SystemRoot%\System32\AppLocker\AppCache.dat` |
| `HKEY_LOCAL_MACHINE\ELAM` | `%SystemRoot%\System32\Config\Elam` |
| `HKEY_USERS\<SID of LocalService>` | `%SystemRoot%\ServiceProfiles\LocalService\Ntuser.dat` |
| `HKEY_USERS\<SID of NetworkService>` | `%SystemRoot%\ServiceProfiles\NetworkService\Ntuser.dat` |
| `HKEY_USERS\<SID of username>` | `\Users\<username>\Ntuser.dat` |
| `HKEY_USERS\<SID>_Classes` | `\Users\<username>\AppData\Local\Microsoft\Windows\Usrclass.dat` |
| `HKEY_USERS\.DEFAULT` | `%SystemRoot%\System32\Config\Default` |
| Virtualized `HKLM\SOFTWARE` | `\ProgramData\Packages\<PackageFullName>\<UserSid>\SystemAppData\Helium\Cache\<RandomName>.dat` |
| Virtualized `HKCU` | `\ProgramData\Packages\<PackageFullName>\<UserSid>\SystemAppData\Helium\User.dat` |
| Virtualized `HKLM\SOFTWARE\Classes` | `\ProgramData\Packages\<PackageFullName>\<UserSid>\SystemAppData\Helium\UserClasses.dat` |

Volatile hives (like `HKLM\HARDWARE`) are created at boot and never written to disk (in paged pool, lost after reboot), virtualized hives are mounted on demand for packaged apps. Hives are loaded at boot or explicitly via `NtLoadKey` / `RegLoadKey` (SeRestorePrivilege required).

## Registry Value Details

These include details on keys/values which are either poorly documented or not documented/mentioned anywhere at all, and some miscellanous section which shows captures of e.g. SystemSettings. The [win-config docs](https://noverse.dev/docs/win-config) include more information on several values, these are just the "big list" sections.

- [MMCSS Values](https://noverse.dev/docs/win-config/system/mmcss-values/#registry-values)
- [DWM Values](https://noverse.dev/docs/win-config/system/dwm-values/#registry-values)
- [Session Manager Values](https://noverse.dev/docs/win-config/system/kernel-values/) (kernel, MM, executive, power, segment heap, I/O, CM...)
- [DXG Kernel Values](https://www.noverse.dev/docs/win-config/system/dxg-kernel-values/#registry-values)
- [BCD Edits](https://www.noverse.dev/docs/win-config/system/bcd-edits/#registry-values)
- [Accessibility Values](https://noverse.dev/docs/win-config/system/disable-accessibility-features/#systemsettings-captures)
- [Explorer Values](https://noverse.dev/docs/win-config/visibility/explorer-options/#explorer-captures)
- [Taskbar Values](https://noverse.dev/docs/win-config/visibility/taskbar-settings/#systemsettings-captures)
- [Mouse Values](https://noverse.dev/docs/win-config/peripheral/mouse-values/#rawmousethrottle-details)
- [USBFLAGS Values](https://www.noverse.dev/docs/win-config/peripheral/usbflags-values/#registry-values)
- [USB Values](https://www.noverse.dev/docs/win-config/peripheral/usb-values/#registry-values)
- [USBHUB Values](https://www.noverse.dev/docs/win-config/peripheral/usbhub-values/#registry-values)
- [Audio Values](https://noverse.dev/docs/win-config/peripheral/audio-values/#registry-values)
- [StorNVMe Values](https://www.noverse.dev/docs/win-config/peripheral/stornvme-values/#registry-values)
- [StorPort Values](https://noverse.dev/docs/win-config/peripheral/storport-values/#registry-values)
- [PnP Device Values](https://www.noverse.dev/docs/win-config/power/pnp-device-values/#registry-values)
- [Power Values](https://www.noverse.dev/docs/win-config/power/power-values/#registry-values)
- [Windows Security Values](https://noverse.dev/docs/win-config/security/windows-defender/#windows-security-captures)

## Capture Table

Most activities were captured during boot, there are some others such as `Steam.txt`, `TLOU2.txt`, `StartAllBack.txt`, and `Lighshot.txt`, that were captured using Procmon during use. The records of specific keys are based on `24H2`.

| File | Path(s) |
|------|---------|
| [23H2.txt](https://github.com/nohuto/regkit/blob/main/assets/records/23H2.txt) | System trace of 23H2, used for [regkit](https://github.com/nohuto/regkit) (see [trace-menu](https://github.com/nohuto/regkit#trace-menu)) |
| [24H2.txt](https://github.com/nohuto/regkit/blob/main/assets/records/24H2.txt) | System trace of 24H2, used for [regkit](https://github.com/nohuto/regkit) (see [trace-menu](https://github.com/nohuto/regkit#trace-menu)) |
| [25H2.txt](https://github.com/nohuto/regkit/blob/main/assets/records/25H2.txt) | System trace of 25H2, used for [regkit](https://github.com/nohuto/regkit) (see [trace-menu](https://github.com/nohuto/regkit#trace-menu)) |
| [ACPI.txt](https://github.com/nohuto/regkit/blob/main/assets/records/ACPI.txt) | `HKLM\SYSTEM\ControlSet001\Services\ACPI`<br>`HKLM\SYSTEM\ControlSet001\Services\acpiex`<br>`HKLM\SYSTEM\ControlSet001\Services\AcpiDev`<br>`HKLM\SYSTEM\ControlSet001\Services\acpipagr`<br>`HKLM\SYSTEM\ControlSet001\Services\AcpiPmi`<br>`HKLM\SYSTEM\ControlSet001\Services\acpitime` |
| [AFD-Parameters.txt](https://github.com/nohuto/regkit/blob/main/assets/records/AFD-Parameters.txt) | `HKLM\SYSTEM\ControlSet001\Services\AFD\Parameters` |
| [Accessibility.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Accessibility.txt) | `HKCU\Control Panel\Accessibility` |
| [Audio.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Audio.txt) | `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio` |
| [BFE.txt](https://github.com/nohuto/regkit/blob/main/assets/records/BFE.txt) | `HKLM\SYSTEM\ControlSet001\Services\BFE` |
| [BrokerInfrastructure.txt](https://github.com/nohuto/regkit/blob/main/assets/records/BrokerInfrastructure.txt) | `HKLM\SYSTEM\ControlSet001\Services\BrokerInfrastructure` |
| [CV-Explorer.txt](https://github.com/nohuto/regkit/blob/main/assets/records/CV-Explorer.txt) | `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer` |
| [Classpnp.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Classpnp.txt) | `HKLM\SYSTEM\ControlSet001\Control\Classpnp` |
| [Control-Wdf.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Control-Wdf.txt) | `HKLM\SYSTEM\ControlSet001\Control\Wdf` |
| [ControlPanel-Desktop.txt](https://github.com/nohuto/regkit/blob/main/assets/records/ControlPanel-Desktop.txt) | `HKCU\Control Panel\Desktop` |
| [ControlPanel-Mouse.txt](https://github.com/nohuto/regkit/blob/main/assets/records/ControlPanel-Mouse.txt) | `HKCU\Control Panel\Mouse` |
| [CrashControl.txt](https://github.com/nohuto/regkit/blob/main/assets/records/CrashControl.txt) | `HKLM\SYSTEM\ControlSet001\Control\CrashControl` |
| [Cryptography.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Cryptography.txt) | `HKLM\SOFTWARE\Microsoft\Cryptography` |
| [Disk-Storport-(990Pro).txt](https://github.com/nohuto/regkit/blob/main/assets/records/Disk-Storport-(990Pro).txt) | `HKLM\SYSTEM\ControlSet001\Enum\SCSI\Disk&Ven_NVMe&Prod_Samsung_SSD_990\5&33c33320&0&000000\Device Parameters\StorPort` (your path will be different) |
| [Dnscache-Parameters.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Dnscache-Parameters.txt) | `HKLM\SYSTEM\ControlSet001\Services\Dnscache\Parameters` |
| [Enum-USB.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Enum-USB.txt) | `HKLM\SYSTEM\ControlSet001\Enum\USB` |
| [Error-Reporting.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Error-Reporting.txt) | `HKLM\SOFTWARE\Microsoft\WINDOWS\Windows Error Reporting` |
| [FileSystem.txt](https://github.com/nohuto/regkit/blob/main/assets/records/FileSystem.txt) | `HKLM\SYSTEM\ControlSet001\Control\FileSystem` |
| [GRE-INITIALIZE.txt](https://github.com/nohuto/regkit/blob/main/assets/records/GRE-INITIALIZE.txt) | `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\GRE_INITIALIZE` |
| [Graphics-Drivers.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Graphics-Drivers.txt) | `HKLM\SYSTEM\ControlSet001\Control\GraphicsDrivers` |
| [Input.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Input.txt) | `HKLM\SYSTEM\INPUT` |
| [Intel-00XX.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Intel-00XX.txt) | `HKLM\SYSTEM\ControlSet001\Control\Class\{4D36E968-E325-11CE-BFC1-08002BE10318}\00XX` (Intel) |
| [Intel.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Intel.txt) | `HKLM\Software\Intel` |
| [Internet-Settings.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Internet-Settings.txt) | `\Software\Microsoft\Windows\CurrentVersion\Internet Settings` |
| [LanmanServer.txt](https://github.com/nohuto/regkit/blob/main/assets/records/LanmanServer.txt) | `HKLM\SYSTEM\ControlSet001\Services\LanmanServer` |
| [Lighshot.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Lighshot.txt) | `HKCU\Software\SkillBrains\Lightshot` |
| [Lsa.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Lsa.txt) | `HKLM\SYSTEM\ControlSet001\Control\Lsa` |
| [MultiMedia.txt](https://github.com/nohuto/regkit/blob/main/assets/records/MultiMedia.txt) | `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\MultiMedia` |
| [NDIS-Parameters.txt](https://github.com/nohuto/regkit/blob/main/assets/records/NDIS-Parameters.txt) | `HKLM\SYSTEM\ControlSet001\Services\NDIS\Parameters` |
| [NetBT.txt](https://github.com/nohuto/regkit/blob/main/assets/records/NetBT.txt) | `HKLM\SYSTEM\ControlSet001\Services\NetBT` |
| [NIC-Intel.txt](https://github.com/nohuto/regkit/blob/main/assets/records/NIC-Intel.txt) | `HKLM\SYSTEM\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\00XX` (Intel) |
| [NIC-Intel-IDA.txt](https://github.com/nohuto/regkit/blob/main/assets/records/NIC-Intel-IDA.txt) | Same path as above, but values were found via decompiling (some may not get read) |
| [NVIDIA-DispGUID.txt](https://github.com/nohuto/regkit/blob/main/assets/records/NVIDIA-DispGUID.txt) | `HKLM\SYSTEM\ControlSet001\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\00XX` |
| [NVIDIA-Corp.txt](https://github.com/nohuto/regkit/blob/main/assets/records/NVIDIA-Corp.txt) | `HKLM\SOFTWARE\NVIDIA Corporation` |
| [NlaSvc.txt](https://github.com/nohuto/regkit/blob/main/assets/records/NlaSvc.txt) | `HKLM\SYSTEM\ControlSet001\Services\NlaSvc` |
| [OLE.txt](https://github.com/nohuto/regkit/blob/main/assets/records/OLE.txt) | `HKLM\SOFTWARE\Microsoft\OLE` ([*](https://learn.microsoft.com/en-us/windows/win32/com/hkey-local-machine-software-microsoft-ole)) |
| [PnP.txt](https://github.com/nohuto/regkit/blob/main/assets/records/PnP.txt) | `HKLM\SYSTEM\ControlSet001\Control\PnP` |
| [Policies-System.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Policies-System.txt) | `HKLM\SOFTWARE\Policies\Microsoft\WINDOWS\SYSTEM` |
| [Policies.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Policies.txt) | `HKLM\SYSTEM\ControlSet001\Policies` |
| [Policies.txt](https://github.com/nohuto/regkit/blob/main/assets/records/CV-Policies.txt) | `HKLM\Software\Microsoft\Windows\CurrentVersion\Policies` |
| [Power.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Power.txt) | `HKLM\SYSTEM\ControlSet001\Control\Power` |
| [Session-Manager.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Session-Manager.txt) | `HKLM\SYSTEM\ControlSet001\Control\Session Manager`<br>`HKLM\SYSTEM\ControlSet001\Control\Session Manager\Memory Management`<br>`HKLM\SYSTEM\ControlSet001\Control\Session Manager\Power`<br>`HKLM\SYSTEM\ControlSet001\Control\Session Manager\Quota System` |
| [StartAllBack.txt](https://github.com/nohuto/regkit/blob/main/assets/records/StartAllBack.txt) | `HKCU\Software\StartIsBack` |
| [Steam.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Steam.txt) | `HKCU\Software\Valve\Steam` |
| [StorPort.txt](https://github.com/nohuto/regkit/blob/main/assets/records/StorPort.txt) | `HKLM\SYSTEM\ControlSet001\Control\StorPort` |
| [TLOU2.txt](https://github.com/nohuto/regkit/blob/main/assets/records/TLOU2.txt) | `HKCU\Software\Naughty Dog` |
| [Tcpip-Parameters.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Tcpip-Parameters.txt) | `HKLM\SYSTEM\ControlSet001\Services\Tcpip\Parameters` |
| [Terminal-Server.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Terminal-Server.txt) | `HKLM\SYSTEM\ControlSet001\Control\Terminal Server` |
| [USB-Flags.txt](https://github.com/nohuto/regkit/blob/main/assets/records/USB-Flags.txt) | `HKLM\SYSTEM\ControlSet001\Control\usbflags` |
| [USBHUB3.txt](https://github.com/nohuto/regkit/blob/main/assets/records/USBHUB3.txt) | `HKLM\SYSTEM\ControlSet001\Services\USBHUB3` |
| [WHEA.txt](https://github.com/nohuto/regkit/blob/main/assets/records/WHEA.txt) | `HKLM\SYSTEM\ControlSet001\Control\WHEA` |
| [Windows-Defender.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Windows-Defender.txt) | `HKLM\SOFTWARE\Policies\Microsoft\Windows Defender` |
| [Windows-Dwm.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Windows-Dwm.txt) | `HKLM\SOFTWARE\Microsoft\Windows\Dwm` |
| [Winows-NT.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Winows-NT.txt) | `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Windows` |
| [Wisp.txt](https://github.com/nohuto/regkit/blob/main/assets/records/Wisp.txt) | `HKCU\Software\Microsoft\Wisp` |
| [disk.txt](https://github.com/nohuto/regkit/blob/main/assets/records/disk.txt) | `HKLM\SYSTEM\ControlSet001\Services\disk` |
| [kbdclass.txt](https://github.com/nohuto/regkit/blob/main/assets/records/kbdclass.txt) | `HKLM\SYSTEM\ControlSet001\Services\kbdclass` |
| [kbdhid.txt](https://github.com/nohuto/regkit/blob/main/assets/records/kbdhid.txt) | `HKLM\SYSTEM\ControlSet001\Services\kbdhid` |
| [monitor.txt](https://github.com/nohuto/regkit/blob/main/assets/records/monitor.txt) | `HKLM\SYSTEM\ControlSet001\Services\monitor` |
| [mouclass.txt](https://github.com/nohuto/regkit/blob/main/assets/records/mouclass.txt) | `HKLM\SYSTEM\ControlSet001\Services\mouclass` |
| [mouhid.txt](https://github.com/nohuto/regkit/blob/main/assets/records/mouhid.txt) | `HKLM\SYSTEM\ControlSet001\Services\mouhid` |
| [nvlddmkm.txt](https://github.com/nohuto/regkit/blob/main/assets/records/nvlddmkm.txt) | `HKLM\SYSTEM\ControlSet001\Services\nvlddmkm` |
| [pci.txt](https://github.com/nohuto/regkit/blob/main/assets/records/pci.txt) | `HKLM\SYSTEM\ControlSet001\Enum\pci` |
| [stornvme.txt](https://github.com/nohuto/regkit/blob/main/assets/records/stornvme.txt) | `HKLM\SYSTEM\ControlSet001\Services\stornvme\Parameters` |
| [usbhub.txt](https://github.com/nohuto/regkit/blob/main/assets/records/usbhub.txt) | `HKLM\SYSTEM\ControlSet001\Services\usbhub` |
| [wbem.txt](https://github.com/nohuto/regkit/blob/main/assets/records/wbem.txt) | `HKLM\SOFTWARE\Microsoft\wbem` |

## References

[Mysteries of the Registry](https://scorpiosoftware.net/2022/04/15/mysteries-of-the-registry/), [Windows Internals, Seventh Edition, Part 2](https://github.com/nohuto/windows-books/releases/download/7th-Edition/Windows-Internals-E7-P2.pdf), [NT Registry Implementation](https://empyreal96.github.io/nt-info-depot/Windows-Kernel-Internals/NTRegistryImplimentation.pdf), and [The Windows Registry Adventure](https://projectzero.google/2024/10/the-windows-registry-adventure-4-hives.html) were used for understanding the Registry and preparing this documentation. This document summarizes selected topics rather than providing complete Registry documentation.
