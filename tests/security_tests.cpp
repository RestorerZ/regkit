// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "editors/bitfield_definition.h"
#include "registry/registry_store.h"
#include "registry/value_format.h"
#include "win32/file_text.h"
#include "win32/handle_owner.h"
#include "win32/process_rights.h"
#include "win32/registry_native.h"

#include <sddl.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(
    bool condition,
    const char* name
) {
  wprintf(L"%hs %hs\n", condition ? "PASS" : "FAIL", name);
  failures += condition ? 0 : 1;
}

bool ParsesDefinition(
    const std::string& json
) {
  const std::vector<BYTE> bytes(json.begin(), json.end());
  regkit::editors::bitfield::DefinitionFile file;
  std::wstring error;
  return regkit::editors::bitfield::Parse(bytes, &file, &error);
}

void BitfieldParserRejectsTruncatedEscapes() {
  const std::string prefix = "{\"format\":\"regkit-bitfield\",\"name\":\"";
  const char* tails[] = {"a\\", "a\\u", "a\\uD", "a\\uD8", "a\\uD80", "a\\uD800", "a\\uD800\\", "a\\uD800\\u", "a\\uD800\\uDC0", "a\\uDC00\""};
  bool all_rejected = true;
  for (const char* tail : tails) {
    all_rejected = all_rejected && !ParsesDefinition(prefix + tail);
  }
  Check(all_rejected, "bitfield parser rejects truncated escapes");
  Check(ParsesDefinition("{\"format\":\"regkit-bitfield\",\"name\":\"Demo\",\"definitions\":[{\"name\":\"Demo\",\"value_name\":\"Flags\",\"bit_width\":32,"
                         "\"fields\":[{\"name\":\"Bit0\",\"bits\":[0],\"meaning\":\"first\"}]}]}"),
        "bitfield parser accepts a valid definition");
}

void RandomFileSuffixIsUnpredictable() {
  std::set<std::wstring> seen;
  bool well_formed = true;
  for (int i = 0; i < 1000; ++i) {
    const std::wstring suffix = util::RandomFileSuffix(L".part");
    well_formed = well_formed && suffix.size() == 22 && suffix.front() == L'.' && suffix.ends_with(L".part");
    seen.insert(suffix);
  }
  Check(well_formed && seen.size() == 1000, "random file suffixes are unique and well formed");
}

void IndirectStringsResolveOnlyLocally() {
  auto display = [](const std::wstring& text, bool resolve) {
    return regkit::value_format::DisplayData(REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()), static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)), resolve);
  };
  const std::wstring local = L"@%SystemRoot%\\system32\\shell32.dll,-21787";
  const std::wstring unc = L"@\\\\127.0.0.1\\share\\missing.dll,-1";
  const std::wstring url = L"@https://example.invalid/x.dll,-1";
  Check(display(local, true) != local, "local indirect string resolves");
  Check(display(local, false) == local, "search and compare keep indirect strings literal");
  Check(display(unc, true) == unc, "UNC indirect string stays literal");
  Check(display(url, true) == url, "URL indirect string stays literal");
}

bool DaclGrantsGuests(
    HKEY key
) {
  DWORD size = 0;
  if (RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, nullptr, &size) != ERROR_INSUFFICIENT_BUFFER) {
    return false;
  }
  std::vector<BYTE> descriptor(size);
  if (RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, descriptor.data(), &size) != ERROR_SUCCESS) {
    return false;
  }
  LPWSTR text = nullptr;
  if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor.data(), SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr)) {
    return false;
  }
  const bool guests = wcsstr(text, L";BG)") != nullptr;
  LocalFree(text);
  return guests;
}

void SymbolicLinkSecurityIsReadFromTheLink() {
  constexpr wchar_t kRoot[] = L"Software\\RegKitSecurityTest";
  RegDeleteTreeW(HKEY_CURRENT_USER, kRoot);
  RegDeleteKeyW(HKEY_CURRENT_USER, kRoot);
  util::UniqueHKey root;
  util::UniqueHKey target;
  util::UniqueHKey link;
  DWORD disposition = 0;
  bool ready = util::CreateRegistryKey(HKEY_CURRENT_USER, kRoot, KEY_ALL_ACCESS, REG_OPTION_NON_VOLATILE, &root, &disposition) == ERROR_SUCCESS &&
               util::CreateRegistryKey(root.get(), L"Target", KEY_ALL_ACCESS, REG_OPTION_NON_VOLATILE, &target, &disposition) == ERROR_SUCCESS;
  const std::wstring sid = util::GetCurrentUserSidString();
  const std::wstring sddl = L"D:(A;CI;KA;;;" + sid + L")(A;CI;KA;;;SY)(A;CI;KA;;;BA)(A;;KR;;;BG)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  ready = ready && ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) &&
          RegSetKeySecurity(target.get(), DACL_SECURITY_INFORMATION, descriptor) == ERROR_SUCCESS;
  LocalFree(descriptor);
  const std::wstring native_target = L"\\Registry\\User\\" + sid + L"\\" + kRoot + L"\\Target";
  ready = ready &&
          util::CreateRegistryKey(root.get(), L"Link", KEY_SET_VALUE | KEY_CREATE_LINK | DELETE, REG_OPTION_NON_VOLATILE | REG_OPTION_CREATE_LINK, &link, &disposition) == ERROR_SUCCESS &&
          RegSetValueExW(link.get(), L"SymbolicLinkValue", 0, REG_LINK, reinterpret_cast<const BYTE*>(native_target.c_str()), static_cast<DWORD>(native_target.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
  Check(ready, "symbolic link fixture created");
  if (ready) {
    util::UniqueHKey followed;
    util::UniqueHKey opened_link;
    const std::wstring link_path = std::wstring(kRoot) + L"\\Link";
    const bool opened = util::OpenRegistryPath(HKEY_CURRENT_USER, link_path, READ_CONTROL, false, &followed) == ERROR_SUCCESS &&
                        util::OpenRegistryPath(HKEY_CURRENT_USER, link_path, READ_CONTROL, true, &opened_link) == ERROR_SUCCESS;
    Check(opened && DaclGrantsGuests(followed.get()), "following the link reaches the target DACL");
    Check(opened && !DaclGrantsGuests(opened_link.get()), "opening the link reads the link's own DACL");
    regkit::RegistryNode node;
    node.root = HKEY_CURRENT_USER;
    node.root_name = L"HKEY_CURRENT_USER";
    node.subkey = link_path;
    std::vector<BYTE> link_descriptor;
    LPWSTR text = nullptr;
    const bool backend = regkit::RegistryStore::ReadKeySecurity(node, &link_descriptor) && !link_descriptor.empty() &&
                         ConvertSecurityDescriptorToStringSecurityDescriptorW(link_descriptor.data(), SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr);
    Check(backend && wcsstr(text, L";BG)") == nullptr, "registry backend reads the link's own DACL");
    LocalFree(text);
  }
  if (link) {
    util::DeleteNativeRegistryKey(link.get());
  }
  link.reset();
  target.reset();
  root.reset();
  RegDeleteTreeW(HKEY_CURRENT_USER, kRoot);
  RegDeleteKeyW(HKEY_CURRENT_USER, kRoot);
}

} // namespace

int wmain() {
  BitfieldParserRejectsTruncatedEscapes();
  RandomFileSuffixIsUnpredictable();
  IndirectStringsResolveOnlyLocally();
  SymbolicLinkSecurityIsReadFromTheLink();
  wprintf(L"%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
