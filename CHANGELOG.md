# Changelog

## V0.0.1.0

### Added

- Bit editor for `REG_DWORD`, `REG_DWORD_BIG_ENDIAN`, `REG_QWORD`, `REG_BINARY` values
  - First few bit definitions (ShellState, UserPreferencesMask, NVIDIA RM values)

  
- **Decode Value**: Hex, Percent, Base64 and Base64URL transforms, plus decoders for raw bytes, UTF-8, UTF-16 LE/BE, ASCII, FILETIME, SYSTEMTIME, Unix seconds/milliseconds, GUID, SID, security descriptor and IPv4/IPv6. Known values (e.g. `ShutdownTime`, `InstallDate`) get a matching decoder pre-selected
- `Edit with RegKit` context menu for `.reg` files, via `Options > Add "Edit" Context Menu`, the installer or `--install-edit-context-menu`/`--uninstall-edit-context-menu`. `--edit-reg file.reg` opens a file in a tab, including in an already running instance
- `--install-regedit-replacement`/`--uninstall-regedit-replacement`, now used by the installer. Setup and uninstall stop instead of overwriting or orphaning another program's Regedit redirect
- Jump support for tools that open a key through Regedit, Regedit favorites listed in the Favorites menu, `reg:` paths in the address bar
- Find: `Skip symbolic links` option, `Browse...` button for excluded keys
- Compare Registries: result filter (both, only differences, only matching) with a `Result` column
- `Options > Clear Caches` (all or per cache), `Reset Settings...` and `Run As > Restart`
- Grid lines toggle on every list header, column menu (size to fit, show/hide) and sortable columns in all lists, including dialogs
- Windows 11 26H1 defaults

### Changed

- Menus regrouped: `Run As` and `Copy Other` submenus, settings moved from File to Options, Compare Registries moved to Tools, separators added and every menu has an Alt access key
- Read only mode opens the value editors for viewing instead of disabling them
- "Standard Hives" renamed to "Root Keys" (tree, Find, `View > Show Extra Root Keys`)
- Find no longer searches the `REGISTRY` root by default
- Compare Registries takes one key path per entry instead of separate root and path fields
- Replace window options reordered (#58)
- `Open File After Export` opens the file in a RegKit tab instead of merging it (#59)
- Grid lines use the theme's border color
- Manifest declares supported OS versions (Vista to 11), `asInvoker` and `PerMonitorV2`

### Fixed

- Value editor dialogs opened invisible, which made RegKit look frozen (#57)
- `.reg` files opened from the context menu didn't open in RegKit (#60)
- Comparison tabs showed the column titles of the first comparison in the session (#61)
- Creating the first subkey didn't expand its parent key
- Symbolic link keys were followed when they should be opened directly, and the `HKEY_CURRENT_CONFIG` target path was wrong
- Status bar paths longer than 255 characters overran the text buffer
- Find results were cleared when switching to another tab
- New key/value commands ran while a name was still being edited, and holding F7 created multiple keys
- Combo box drop-down lists had light scrollbars in dark mode
- Cell tooltips showed stale text in the value, search and history lists
- A header column stayed highlighted after clicking the grid lines toggle
- The `Bits...` button showed for all types and overlapped other controls in the value editors
- "Registry Comparision" typo in the comparison tab label
