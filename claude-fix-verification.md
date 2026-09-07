# Independent verification of the todo fixes

## Verdict

I reviewed every finding in the ten applicable files under `todo`. The two files whose names start with `!` were excluded as requested.

Claude marked 121 findings as fixed. The current source supports 103 of those claims. Eighteen fixed annotations are incomplete or incorrect. The five `PARTIALLY FIXED` annotations and nineteen `NOT IMPLEMENTED` annotations were not counted as failures in this audit.

The current working tree builds successfully for x64 Release and x86 Release. CTest reports that neither build contains automated tests. Two focused CLI checks reproduced defects in the new `reg compare /s` implementation.

No source code was changed during this verification.

## Post-implementation verification

Claude subsequently implemented the remediation plan in this document. A second independent review found that eleven of the seventeen grouped areas are now implemented correctly and six are only partially complete.

### Correctly implemented

- The SYSTEM and TrustedInstaller environment blocks now use the target tokens
- Service startup now follows pending states, checkpoints, and wait hints
- Restart PID parsing is strict and range checked
- Relaunch preserves the first real argument
- `RegSetKeySecurity` now converts its direct result
- Offline key creation checks the disposition
- History append failures are shown to the user
- Recursive CLI comparison now uses a parsed recursive helper
- Partial offline unload rebuilds the visible root model
- The update worker is cancelled and joined before message draining
- Theme preset fields now validate known values while ignoring unknown keys

### Still incomplete

#### Incomplete key snapshots can still be deleted

`src/frame/mutation_commands.cpp:675-680` and `src/frame/mutation_commands.cpp:772-778` still offer “Delete anyway?” when `snapshot.complete` is false. Choosing it preserves the original data-loss risk. The deletion must stop when a complete undo snapshot cannot be captured, or the operation must be explicitly irreversible and must not create a misleading undo entry.

There is also a symbolic-link security problem. `CaptureKey` reads security before detecting a link, but `ReadKeySecurity` opens the path normally in `src/registry/live_registry.cpp:409`. That follows a registry link and captures the target key's security rather than opening the link object with `REG_OPTION_OPEN_LINK`. Restoring that descriptor onto the new link is incorrect.

Offline and virtual backends return false for `ReadKeySecurity` in `src/registry/registry_store.cpp:236-248`. Every offline snapshot is therefore incomplete. This needs an explicit product rule instead of the current warning override.

#### Invalid UTF-8 search caches are still accepted as empty

Numeric and enum validation in `src/search/result_file.cpp` is now strict. However, `Utf8ToWide` returns an empty string for malformed nonempty UTF-8, and `LoadResults` passes that empty result to `ParseResults`, which succeeds with zero rows. Reject a nonempty byte payload when UTF-8 decoding produces no text, or change the decoder to return an explicit success result.

`FileTimeFromString` at `src/search/result_file.cpp:21` is also dead after the strict parser rewrite and should be removed.

#### Impersonation restoration failures remain partly ignored

The original thread token is now captured correctly. In both cleanup blocks in `src/win32/process_rights.cpp`, restoration failure is only recorded when the process launch itself succeeded. If the launch already failed, a failed token restoration is silently ignored. `RevertToSelf` is also unchecked. A restoration failure matters independently because it can leave the calling thread under the wrong identity.

#### Restricted-handle initialization has a failure-path cleanup gap

The child no longer inherits unrelated handles, and the added `NUL` input handle is correct. If `InitializeProcThreadAttributeList` succeeds but `UpdateProcThreadAttribute` fails, `drop_capture` clears the initialized buffer without first calling `DeleteProcThreadAttributeList`. Track successful initialization and destroy the list before clearing its storage.

The code also silently runs without output capture when restricted-list setup fails. This is secure because `bInheritHandles` becomes false, but it differs from the audit's fail-closed behavior and reduces error detail to a generic message.

#### Offline rename cannot report failed rollback

`src/registry/offline_registry.cpp` now checks the destination and attempts to delete the new value if deleting the old value fails. The rollback deletion result is ignored. If that deletion also fails, both values remain and the caller receives only the same generic failure. Return a richer result or error code so the partial mutation can be reported accurately.

#### Case normalization still has redundant wrappers

The remaining registry identity paths now use Windows case mapping, so the functional defect is fixed. The minimal cleanup requested in the audit is incomplete:

- `src/defaults/default_data.cpp` retains a local `Lower` wrapper
- `src/defaults/default_loader.cpp` retains the same wrapper
- `src/search/compare.cpp` retains the same wrapper
- `src/regfile/reg_file.cpp` duplicates `CharLowerBuffW` instead of using `util::ToLower`

Use `util::ToLower` directly and remove the now-unused includes where applicable.

### Minor new persistence edge

`history_cache_failed_` is set after the first append failure but never cleared after a later successful append. A second failure after recovery will therefore not notify the user. Reset the flag on successful persistence.

### Second validation results

- x64 Release build passed
- x86 Release build passed
- `git diff --check` passed
- No non-exempt source comments were found
- CTest still reports no tests for either architecture
- Recursive compare with a missing child returned the correct exit code 2
- Recursive compare with `/s` before the operands and a deep difference returned the correct exit code 2
- Identical trees returned exit code 0
- Different values returned exit code 2
- Two missing root keys returned exit code 1

## Third verification after the follow-up implementation

This section supersedes the remaining-item list in the post-implementation verification above. The follow-up corrected most of that list, but the implementation is not fully complete yet.

### Now fixed

- An incomplete key snapshot can be deleted only after an explicit irreversible-operation warning, and no undo entry is created for that deletion. This is correct in both delete paths at `src/frame/mutation_commands.cpp:674-696` and `src/frame/mutation_commands.cpp:774-795`.
- A malformed nonempty UTF-8 result payload is now rejected at `src/search/result_file.cpp:199-206`. The dead `FileTimeFromString` helper is gone.
- Restricted handle-list initialization now tracks successful initialization and calls `DeleteProcThreadAttributeList` on both the fallback and normal paths at `src/regfile/registry_transfer.cpp:61-140`.
- A successful history append resets `history_cache_failed_` at `src/frame/history_workspace.cpp:162-172`.
- Registry identity normalization in defaults, comparison, and the value table now uses `util::ToLower`.

### Still incomplete

#### 1. Snapshot security is still incorrect for offline hives and has a live-link fallback hazard

`CaptureKey` declares security unsupported for every offline root at `src/changes/key_snapshot.cpp:24-29`. `RegistryStore::ReadKeySecurity` and `WriteKeySecurity` still return false for the offline backend at `src/registry/registry_store.cpp:236-248`.

That assumption is incorrect. Offreg exports `ORGetKeySecurity` and `ORSetKeySecurity` specifically to read and write an offline key's security descriptor. The current `OffregApi` does not load either function at `src/registry/offline_registry.cpp:19-40` and `src/registry/offline_registry.cpp:58-71`. An offline delete can therefore be marked fully restorable while its restored key receives inherited security instead of the deleted key's owner, group, and DACL.

The live backend now opens registry links with `REG_OPTION_OPEN_LINK`, but `ReadKeySecurity` falls back to an ordinary open at `src/registry/live_registry.cpp:423-426`, and `WriteKeySecurity` does the same at `src/registry/live_registry.cpp:452-455`. If the no-follow open fails while the ordinary open succeeds, that fallback operates on the link target. It can capture or restore the target's descriptor instead of the link object's descriptor.

Required change:

- Load and require `ORGetKeySecurity` and `ORSetKeySecurity` in `OffregApi`
- Add offline `ReadKeySecurity` and `WriteKeySecurity` implementations and dispatch to them through `RegistryStore`
- Keep virtual roots exempt because their `.reg`-style data model has no security descriptors
- Remove the ordinary-open fallback from live security access. `REG_OPTION_OPEN_LINK` requests the source object for a symbolic link and also avoids silently changing the object being secured

Microsoft documents the relevant native functions in [ORGetKeySecurity](https://learn.microsoft.com/en-us/windows/win32/devnotes/orgetkeysecurity), [ORSetKeySecurity](https://learn.microsoft.com/en-us/windows/win32/devnotes/orsetkeysecurity), and [RegOpenKeyExW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regopenkeyexw).

#### 2. Thread-token restoration failure can still be hidden or misreported

Both launch functions now attempt and check restoration at `src/win32/process_rights.cpp:536-550` and `src/win32/process_rights.cpp:666-680`. However, a restoration error replaces the reported error only when process creation succeeded or the earlier error is `ERROR_SUCCESS`. If process creation already failed with another error, the failed restoration is still discarded even though the calling thread may remain impersonated.

There is also an ambiguous success state. When child creation succeeds but token restoration fails, the function returns false after the child has already been created. `BrokerRestart` then leaves the old process open and reports that launch failed, while the child may already exist and be waiting for the old process to exit.

Required change:

- Preserve the launch outcome separately so a created child is never reported as though creation did not happen
- Treat a restoration failure as fatal regardless of the earlier operation result
- Do not return to the application UI after a restoration failure. Microsoft explicitly says the process should shut down if `RevertToSelf` fails because it otherwise continues under the impersonated identity
- If child creation succeeded, retain that fact so shutdown also releases the already-created replacement from its parent wait

See [RevertToSelf](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-reverttoself).

#### 3. The richer rename failure is not propagated through every caller

Live and offline rename now reject an existing destination, try to remove the new value when deleting the old value fails, and set `both_names_left` when that rollback also fails. The direct value-list rename reports that condition correctly at `src/frame/control_lifetime.cpp:755-763`.

Two mutation paths discard the new signal:

- Search and replace calls `RenameValue` without `both_names_left` at `src/frame/search_window.cpp:861`
- Undo and redo call it without `both_names_left` at `src/frame/undo_menus.cpp:112`

Search and replace reduces the result to a generic failure count. Undo and redo fail silently and move the operation back between stacks. In either path, both names can remain without the user being told about the partial mutation.

Required change:

- Consume `both_names_left` in search and replace and carry a partial-mutation message in `ReplacePayload`
- Consume it in undo and redo and show the same explicit warning used by direct rename
- Refresh the affected view after this partial result because the registry did change even though `RenameValue` returned false

### Cleanup leftovers

The functional case-normalization change is correct, but the helper removal left empty anonymous namespaces in:

- `src/defaults/default_data.cpp:13-15`
- `src/defaults/default_loader.cpp:14-16`
- `src/frame/browse_view.cpp:9-11`

`src/defaults/default_data.cpp` also retains an unused `<algorithm>` include. `src/regfile/reg_file.cpp:31-33` retains a local `Lower(std::wstring_view)` adapter that creates a temporary string before `util::ToLower` copies it again. A `std::wstring_view` overload in `text_transform` would remove the duplicate helper and the extra copy.

### Third validation results

- x64 Release build passed
- x86 Release build passed
- `git diff --check` passed
- CTest still reports no tests for either architecture
- Recursive compare returned 0 for identical trees
- Recursive compare returned 2 for a missing child
- Recursive compare returned 2 for a deep difference with `/s` before the operands
- Recursive compare returned 2 for different values
- Recursive compare returned 1 when both root keys were missing
- A direct `RegOpenKeyExW` probe opened the ordinary `HKCU\Software` key successfully with `REG_OPTION_OPEN_LINK`, confirming that the no-follow path does not need an ordinary-open fallback for normal keys
- No non-exempt source comments were found. The remaining `//` matches outside copyright and namespace endings are URL string literals

No source code was changed during this third verification.

## Fourth verification after the latest implementation

This is the current authoritative result. The snapshot-security and process-shutdown corrections are implemented. Rename rollback handling still has state-management defects, so the remediation is not completely finished.

### Correctly implemented now

- Offline security descriptors are read with `ORGetKeySecurity` and restored with `ORSetKeySecurity` in `src/registry/offline_registry.cpp:624-679`.
- `RegistryStore` dispatches security operations to the offline backend at `src/registry/registry_store.cpp:236-253`.
- Offline security failure now makes a snapshot incomplete, while only the virtual `.reg`-style model remains exempt at `src/changes/key_snapshot.cpp:24-26`.
- Live security access no longer falls back from `REG_OPTION_OPEN_LINK` to an ordinary open at `src/registry/live_registry.cpp:423-425` and `src/registry/live_registry.cpp:449-451`.
- A failed impersonation restoration is propagated independently through `impersonation_lost`. Every current caller closes the process instead of continuing under the wrong identity at `src/frame/launch_bridge.cpp:51-64` and `src/launch/program.cpp:583-641`.
- Search replacement and undo or redo now consume the `both_names_left` signal and display the partial mutation.
- The empty namespaces, redundant lowercase adapter, extra lowercase copy, and previously identified unused defaults include are gone.

### Still incomplete

#### Partial renames are reported but aren't fully committed as mutations

The backend contract is that `RenameValue` returns false when rollback fails while `both_names_left` reports that the registry nevertheless changed. Callers therefore have to treat this as a mutation for dirty state, refresh, and undo-stack consistency.

The direct value-list path at `src/frame/control_lifetime.cpp:755-763` shows the warning and returns immediately. It does not call `MarkOfflineDirty()` or `UpdateValueListForNode()`. For an offline hive, the duplicate value can therefore remain hidden in the current list while RegKit does not know that the hive needs saving.

Search replacement refreshes the list, but `src/frame/search_window.cpp:992-994` marks the offline hive dirty only when `payload->changes` is nonempty. If every attempted operation ends as a partial rename, `partial_renames` is nonzero while the changed offline hive is still considered clean.

Undo and redo correctly mark dirty and refresh at `src/frame/undo_menus.cpp:122-127`, but they return false for the partial mutation. `src/frame/clipboard_commands.cpp:426-450` consequently puts that operation back on the stack as though nothing changed. The operation is now invalid because both names exist. Retrying it fails again and can leave the invalid operation blocking older undo history.

Required change:

- In direct rename, call `MarkOfflineDirty()` and refresh the current value list before reporting `both_names_left`
- In search replacement, mark dirty when either `changes` or `partial_renames` is nonempty
- Return a three-state undo result such as success, unchanged failure, and partial mutation
- On a partial undo or redo, refresh and mark dirty as already done, then discard the now-invalid operation instead of putting it back on either stack

### Remaining minor cleanup

`src/browse/value_table.cpp:7` still includes `<cwctype>` even though the removed lowercase helper was its only user. `<algorithm>` is still required there for `std::fill`.

The process-restoration path is safe because all callers terminate when `impersonation_lost` is true. One diagnostic detail remains imprecise. At `src/win32/process_rights.cpp:545-551` and `src/win32/process_rights.cpp:677-683`, the restoration error replaces an earlier error only if launch succeeded or no earlier error was stored. When launch and restoration both fail, the shutdown warning can therefore format the launch error rather than the restoration error. Capture `GetLastError()` immediately after restoration fails and store it unconditionally. The process creation result is already kept separately in `result`.

### Fourth validation results

- x64 Release build passed
- x86 Release build passed
- `git diff --check` passed
- The x64 and x86 bundled `offreg.dll` files both export `ORGetKeySecurity` and `ORSetKeySecurity`
- A temporary hive probe successfully read and wrote its security descriptor through both bundled Offreg architectures
- All temporary probe registry data and hive files were removed
- Recursive comparison again returned the expected codes 0, 2, 2, 2, and 1 for the five focused cases
- CTest still reports no tests for either build
- No empty anonymous namespaces or duplicate lowercase helpers remain
- No non-exempt source comments were found

No source code was changed during this fourth verification.

## Fifth and final verification

This is the current authoritative result and supersedes every historical incomplete list below it. All remediation items identified by the follow-up audits are now implemented correctly.

### Final corrections verified

- Direct partial renames now mark offline state dirty and refresh the value list before reporting the duplicate names at `src/frame/control_lifetime.cpp:755-767`.
- Search replacement marks offline state dirty when either a completed change or a partial rename occurred at `src/frame/search_window.cpp:988-996`.
- Undo and redo now return `ReplayResult::kPartial` for a partial mutation at `src/frame/undo_menus.cpp:122-142`.
- The clipboard command handlers discard a partial operation instead of putting the invalid operation back on either stack at `src/frame/clipboard_commands.cpp:426-460`.
- Both privileged launch paths capture the restoration error immediately and report it as the primary error at `src/win32/process_rights.cpp:539-550` and `src/win32/process_rights.cpp:670-681`.
- The unused `<cwctype>` include was removed from `src/browse/value_table.cpp`.

### Final validation

- x64 Release build passed
- x86 Release build passed
- `git diff --check` passed
- Recursive comparison returned the expected exit codes for identical trees, a missing child, a deep option-first difference, different values, and two missing roots
- CTest still reports no registered automated tests for either architecture
- No empty anonymous namespaces, duplicate lowercase helpers, or non-exempt source comments remain

No source code was changed during this final verification.

## Report coverage

| Report | Marked fixed | Supported | Incomplete or incorrect |
| --- | ---: | ---: | ---: |
| `appearance.md` | 10 | 9 | 1 |
| `browse.md` | 10 | 10 | 0 |
| `changes-cli-defaults-launch.md` | 22 | 16 | 6 |
| `editors.md` | 7 | 7 | 0 |
| `frame.md` | 10 | 8 | 2 |
| `records-trace-work.md` | 13 | 13 | 0 |
| `regfile.md` | 13 | 12 | 1 |
| `registry.md` | 12 | 10 | 2 |
| `search.md` | 11 | 10 | 1 |
| `win32.md` | 13 | 8 | 5 |
| **Total** | **121** | **103** | **18** |

The case-normalization defect is claimed as fixed in two reports. The offline `CreateKey` defect is an additional backend gap in the same operation that was fixed for the live and virtual backends. The findings below group closely related snapshot defects together.

## Findings that still require changes

### 1. Privileged children still receive the caller's environment

Status: not fixed

Evidence:

- `src/win32/process_rights.cpp:488`
- `src/win32/process_rights.cpp:598`

Both SYSTEM and TrustedInstaller launch paths call `CreateEnvironmentBlock` with `current_token.get()`. Passing `FALSE` only prevents the current process environment from being merged. It does not change which user's environment is created. Microsoft defines `hToken` as the token for the user whose environment is requested in [CreateEnvironmentBlock](https://learn.microsoft.com/en-us/windows/win32/api/userenv/nf-userenv-createenvironmentblock).

Required change:

- Pass `target_token.get()` in both calls
- Keep `bInherit` as `FALSE`
- If user-specific variables are required, load the target profile before creating the environment

### 2. TrustedInstaller service waiting still uses a fixed five-second poll

Status: partially fixed

Evidence: `src/win32/process_rights.cpp:221-255`

The code now avoids calling `StartServiceW` while the service is stop-pending and checks the start error. It still polls fifty times at 100 ms and ignores `dwCheckPoint` and `dwWaitHint`. A valid slow stop or start can therefore fail after five seconds. Microsoft demonstrates tracking the checkpoint and wait hint in [Starting a Service](https://learn.microsoft.com/en-us/windows/win32/services/starting-a-service).

Required change:

- Add one small helper that waits for a pending service state
- Derive the poll interval from `dwWaitHint`
- Reset the timeout when `dwCheckPoint` advances
- Stop immediately on a terminal state or reported service error

### 3. Previous thread impersonation is not captured and restored reliably

Status: partially fixed

Evidence:

- `src/win32/process_rights.cpp:437-438`
- `src/win32/process_rights.cpp:504-505`
- `src/win32/process_rights.cpp:543-544`
- `src/win32/process_rights.cpp:614-615`

Both `OpenThreadToken` calls ignore failure. A failure other than `ERROR_NO_TOKEN` does not stop the launch, and cleanup then calls `SetThreadToken` with a null saved token. The restore call is also unchecked.

Required change:

- Record whether the thread originally had a token
- Treat only `ERROR_NO_TOKEN` as the valid no-token case
- Abort on every other capture error
- Restore the saved token when one existed, otherwise call `RevertToSelf`
- Preserve and report a restore failure

### 4. Restart parent PID parsing still accepts overflow

Status: partially fixed

Evidence: `src/win32/restart.cpp:103-114`

The parse checks full consumption but does not reset or test `errno`. On overflow, `wcstoul` can return `ULONG_MAX`, which becomes an attempted wait on PID `0xffffffff`.

Required change:

- Set `errno` to zero before conversion
- Require at least one digit and full consumption
- Reject `ERANGE`, zero, and values above `DWORD_MAX`

### 5. Project-wide Windows case normalization was not applied

Status: not fixed in both reports that claim it

Evidence:

- `src/defaults/default_loader.cpp:17-21`
- `src/defaults/default_data.cpp:15-19`
- `src/regfile/reg_file.cpp:30-35`
- `src/search/compare.cpp:23-28`
- `src/browse/value_table.cpp:20-26`

These identity and lookup paths still use CRT `towlower`. The fixed annotations in `win32.md` and the defaults section therefore overstate the implementation.

Required change:

- Remove the local lowercasing helpers
- Use the shared Windows-aware operation consistently
- For registry identity, prefer one shared ordinal case-insensitive equality and compatible hash rather than repeatedly allocating normalized strings

### 6. Relaunch drops the first real command-line argument

Status: not fixed

Evidence:

- `src/launch/program.cpp:47-58` already removes the executable from the argument vector
- `src/win32/restart.cpp:66` starts copying at index one again

`original_args[0]` is the first user argument, not the executable. A one-argument `.reg` path is lost entirely when startup settings trigger elevation. Other commands can lose their first switch and accidentally change meaning.

Required change:

- Iterate from index zero
- Continue removing the internal restart switches and the value following `--restart-parent`
- Add direct checks for a single `.reg` path, `--goto <path>`, and each identity-restart chain

### 7. Restricted child-handle inheritance fails open

Status: not fixed safely

Evidence:

- `src/regfile/registry_transfer.cpp:79-98`
- `src/regfile/registry_transfer.cpp:100-103`

If attribute-list allocation, initialization, or update fails, the code clears the storage and continues with ordinary `CreateProcessW` while passing `bInheritHandles = TRUE`. That fallback again gives the child every inheritable handle in the process. Microsoft recommends an explicit handle list for this case in [Create processes](https://learn.microsoft.com/en-us/windows/win32/procthread/creating-processes).

Required change:

- Fail the operation if the explicit handle list cannot be created
- Close the pipe handles through RAII on every exit
- Supply a valid inheritable standard-input handle, such as an opened `NUL` handle, and include it in the allowed list if `STARTF_USESTDHANDLES` remains enabled

### 8. Versioned search-cache records are not validated

Status: partially fixed

Evidence: `src/search/result_file.cpp:66-88` and `src/search/result_file.cpp:118-124`

The parser now rejects a record with fewer than eleven fields, but all numeric fields still use unchecked `_wtoi` or `_wcstoui64`. Invalid text becomes zero, overflow can narrow into `DWORD` or `uint32_t`, and invalid enum values are silently replaced with defaults. Invalid UTF-8 can also become empty input and be accepted as an empty result set.

Required change:

- Make `ParseVersionedRecord` return `bool`
- Parse every numeric field with one strict helper that checks digits, complete consumption, `ERANGE`, and destination range
- Reject invalid enum values instead of substituting them
- Reject a decode failure independently from a valid empty cache
- Decide whether extra fields are allowed and enforce that rule consistently

### 9. The security editor reports the wrong failure code

Status: the API replacement is present, but the implementation is defective

Evidence: `src/registry/security_dialog.cpp:109-115`

`RegSetKeySecurity` returns its Win32 error code directly. The failure branch instead converts `GetLastError`, which can contain an unrelated stale value. Microsoft documents the direct nonzero return in [RegSetKeySecurity](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regsetkeysecurity).

Required change:

- Store the `LSTATUS` returned by `RegSetKeySecurity`
- Return `S_OK` for `ERROR_SUCCESS`
- Otherwise return `HRESULT_FROM_WIN32(status)`

### 10. Offline `CreateKey` still treats an existing key as newly created

Status: additional backend gap

Evidence: `src/registry/offline_registry.cpp:599-611`

The implementation receives the Offreg disposition and ignores it. It returns success for both a newly created key and an existing key. This preserves the same undo hazard that was fixed in the live and virtual backends.

Required change:

- Return success only when the call succeeds and disposition reports a newly created key

### 11. Offline value rename still overwrites and cannot roll back

Status: not fixed

Evidence: `src/registry/offline_registry.cpp:654-677`

The offline path reads the old value, writes the destination without checking whether it exists, then deletes the old value. A destination can be overwritten. If deletion fails, both names remain and the operation reports failure without restoring the previous state. The fixed annotation explicitly covered both live and offline behavior, but only the live backend was corrected.

Required change:

- Reject an existing destination immediately before the write
- Write the new value
- If deleting the old value fails, delete the newly written destination
- Return failure if rollback also fails and make that partial state visible to the caller

### 12. Key snapshots do not reliably preserve security or completeness

Status: two fixed annotations remain incomplete

Evidence:

- `src/changes/key_snapshot.cpp:24-27`
- `src/changes/key_snapshot.cpp:30-60`
- `src/changes/key_snapshot.cpp:76-78`

`ReadKeySecurity` is ignored, so a failed capture can still leave `complete = true`. Symbolic-link snapshots return before attempting security capture. `RestoreKey` ignores `WriteKeySecurity`, so it can report success after failing to restore owner, group, or DACL. Child discovery still uses `EnumSubKeyNames`, which has no error result. The deletion UI also allows “Delete anyway” after an incomplete capture, despite the original finding requiring deletion to abort.

Required change:

- Return an explicit capture result containing success, snapshot, and error
- Include security-capture success in completeness
- Capture and restore link-key security too
- Enumerate children through an error-reporting path
- Check `WriteKeySecurity`
- Do not delete when the snapshot is incomplete

### 13. History append failures are still ignored

Status: partially fixed

Evidence: `src/frame/history_workspace.cpp:162-164`

The full history rewrite is now atomic. `AppendHistoryFile` still returns a result that `AppendHistoryCache` discards, which was part of the original finding.

Required change:

- Return the append result through `AppendHistoryCache`
- Report persistence failure without discarding the in-memory history
- Alternatively replace append with the already atomic full-history writer if the configured row cap keeps that cost acceptable

### 14. Recursive CLI comparison is structurally incorrect

Status: not fixed

Evidence: `src/cli/reg_command.cpp:1147-1172`

The implementation recursively calls the full command parser and rewrites `child_args[1]` and `child_args[2]` as though the two operands always occupy those slots. This has two failures.

- A child present on only one side is treated as an unreadable key and returns exit code 1 instead of a valid difference with exit code 2
- If `/s` appears before the operands, rewriting those slots removes `/s`, so deeper recursion stops and differences can be missed

Reproduced behavior:

- Identical roots with `OnlyLeft` beneath the left root returned exit code 1
- With `/s` before the operands, a difference two levels down returned exit code 0

Required change:

- Parse the command once
- Compare through a recursive helper that receives two optional key references and the parsed options
- Treat one missing side as a difference, then enumerate the existing side as needed
- Propagate actual access or enumeration errors separately
- Print the final result once instead of once per child

### 15. Partial offline unload leaves closed roots in the visible browse model

Status: partially fixed

Evidence: `src/frame/registry_sessions.cpp:45-77`

The global offline-root registry is updated to retain only failed handles. However, when earlier roots close and a later root fails, `browse_.roots()` is not rebuilt before returning. The UI model can therefore still contain `RegistryRootEntry` objects that reference handles already closed in the same loop.

Required change:

- Rebuild the browse roots from `remaining_roots`, labels, and paths before returning failure
- Clear the current selection and value model if they reference a closed root
- Keep dirty state only for the remaining open hives

### 16. The update worker is drained before it is stopped

Status: not fixed

Evidence:

- `src/frame/control_lifetime.cpp:1699-1714`
- `src/frame/update_check.cpp:242-265`

`OnDestroy` drains `kUpdateCheckReady` but never cancels and joins `update_session_` first. The session destructor joins later, after window teardown. The worker can post a new heap payload after the drain while the HWND is being destroyed, leaving a queued payload with no receiver.

Required change:

- Call `update_session_.CancelAndJoin()` before `DiscardWorkerMessages()`
- Clear `update_check_running_` during shutdown
- Keep the existing queue drain after every producer has stopped

### 17. Theme preset validation breaks unknown-key compatibility

Status: not fixed as described

Evidence: `src/appearance/presets.cpp:564-608`

`ApplyField` tries to parse every non-`name` and non-`dark` value as a color before it knows whether the key is recognized. An unknown future key with a non-color value now fails the whole file, even though the annotation says unknown keys remain skipped. The `dark` field also accepts every invalid value as false instead of rejecting a malformed Boolean.

Required change:

- Identify recognized keys before parsing their values
- Skip unknown keys explicitly
- Strictly accept only the documented Boolean spellings for `dark`
- Parse colors only for recognized color fields

## Validation performed

### Successful

- `cmake --build --preset x64-release`
- `cmake --build --preset x86-release`
- `git diff --check`
- Manual source tracing for every annotated finding in all ten reports
- Focused registry-backed CLI checks using a temporary key below `HKCU\Software\RegKitAuditVerification_20260907`, removed immediately after each check

### Failed behavior checks

- `regkit compare <left> <right> /s` returned exit code 1 when only the left side contained a child. This should be a completed comparison with exit code 2
- `regkit compare /s <left> <right>` returned exit code 0 when only the left side contained a grandchild. This should return exit code 2

### Coverage limitation

`ctest --test-dir build -C Release` and the matching x86 command both report `No tests were found`. Privileged SYSTEM and TrustedInstaller launches, forced Offreg failures, security-editor failures, and update-shutdown timing therefore still need focused runtime validation after the corrections above.

## Working-tree state

The build and audit used the current working tree, including Claude's sixteen uncommitted second-pass files. Commit `65f10b0` does not contain those final second-pass edits by itself. Preserve or commit those files before switching branches.
