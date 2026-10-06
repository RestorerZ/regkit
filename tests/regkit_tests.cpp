// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "records/escaped_fields.h"
#include "records/json.h"
#include "regfile/reg_file.h"
#include "registry/hive_files.h"
#include "registry/registry_path.h"
#include "registry/resource_list.h"
#include "registry/value_format.h"
#include "search/compare.h"
#include "search/search.h"
#include "trace/trace_parser.h"
#include "trace/trace_paths.h"

#include <algorithm>
#include <cstdio>

using namespace regkit;

namespace
{

int failures = 0;

void Check(bool ok, const char* expression, int line)
{
    if (!ok)
    {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

void RegistryPath()
{
    CHECK(registry_path::Normalize(L"[HKLM\\Software]") == L"HKEY_LOCAL_MACHINE\\Software");
    CHECK(registry_path::Normalize(L"Computer\\HKEY_CURRENT_USER\\Console") == L"HKEY_CURRENT_USER\\Console");
    CHECK(registry_path::Normalize(L"\\REGISTRY\\MACHINE\\SYSTEM") == L"HKEY_LOCAL_MACHINE\\SYSTEM");
    CHECK(registry_path::VirtualStorePath(L"\\REGISTRY\\MACHINE\\SOFTWARE\\WOW6432Node\\App") == L"HKEY_CURRENT_USER\\Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\WOW6432Node\\App");
    CHECK(registry_path::VirtualStorePath(L"\\REGISTRY\\MACHINE\\SOFTWAREX").empty() && registry_path::VirtualStorePath(L"\\REGISTRY\\MACHINE\\SYSTEM").empty());
    CHECK(registry_path::GlobalKeyPath(L"HKEY_CURRENT_USER\\Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\App") == L"HKEY_LOCAL_MACHINE\\SOFTWARE\\App");
    CHECK(registry_path::GlobalKeyPath(L"HKEY_CURRENT_USER\\Software").empty());
    CHECK(registry_path::Normalize(L"\\REGISTRY\\USER\\S-1-5-21-1\\Software", L"S-1-5-21-1") == L"HKEY_CURRENT_USER\\Software");
    CHECK(registry_path::Normalize(L"\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\.txt") == L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\.txt");
    CHECK(registry_path::Normalize(L"Registry::HKCU\\Console") == L"HKEY_CURRENT_USER\\Console");
    CHECK(registry_path::Normalize(L"HKCU:\\Console") == L"HKEY_CURRENT_USER\\Console");

    const std::wstring path = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft";
    CHECK(registry_path::Format(path, registry_path::Style::kAbbreviated) == L"HKLM\\SOFTWARE\\Microsoft");
    CHECK(registry_path::Format(path, registry_path::Style::kRegFileHeader) == L"[" + path + L"]");
    CHECK(registry_path::Format(path, registry_path::Style::kPowerShellDrive) == L"HKLM:\\SOFTWARE\\Microsoft");
    CHECK(registry_path::Format(path, registry_path::Style::kPowerShellProvider) == L"Registry::" + path);
    CHECK(registry_path::Format(path, registry_path::Style::kEscaped) == L"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Microsoft");

    const auto parts = registry_path::Split(path);
    CHECK(parts.size() == 3 && parts[2] == L"Microsoft");
    CHECK(registry_path::Join(parts) == path);
    CHECK(registry_path::Parent(path) == L"HKEY_LOCAL_MACHINE\\SOFTWARE");
    CHECK(registry_path::Leaf(path) == L"Microsoft");
    CHECK(registry_path::HasComponentPrefix(path, L"HKEY_LOCAL_MACHINE\\SOFTWARE"));
    CHECK(!registry_path::HasComponentPrefix(L"HKEY_LOCAL_MACHINE\\SOFTWAREX", L"HKEY_LOCAL_MACHINE\\SOFTWARE"));
    CHECK(registry_path::ClassesSourcePath(L"\\REGISTRY\\MACHINE\\SOFTWARE\\ClassesX\\a", L"S-1").empty());
}

void HiveFiles()
{
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring folder = std::wstring(temp) + L"regkit_hives";
    CreateDirectoryW(folder.c_str(), nullptr);
    CreateDirectoryW((folder + L"\\alice").c_str(), nullptr);
    const std::wstring files[] = {L"\\SYSTEM", L"\\extra.dat", L"\\notes.txt", L"\\alice\\NTUSER.DAT", L"\\alice\\USRCLASS.DAT"};
    for (const std::wstring& file : files)
    {
        CloseHandle(CreateFileW((folder + file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
    }
    std::vector<OfflineHiveCandidate> found;
    CollectOfflineHivesInFolder(folder, &found);
    std::vector<std::wstring> labels;
    for (const auto& candidate : found)
    {
        labels.push_back(candidate.label);
    }
    CHECK(labels == std::vector<std::wstring>({L"SYSTEM", L"extra", L"alice", L"alice_Classes"}));
    CHECK(ResolveOfflineRootName(folder + L"\\alice\\NTUSER.DAT", false, nullptr) == L"HKEY_USERS");
    CHECK(ResolveOfflineRootName(folder + L"\\SYSTEM", false, nullptr) == L"HKEY_LOCAL_MACHINE");
    for (const std::wstring& file : files)
    {
        DeleteFileW((folder + file).c_str());
    }
    RemoveDirectoryW((folder + L"\\alice").c_str());
    RemoveDirectoryW(folder.c_str());
}

void ValueFormat()
{
    CHECK(value_format::TypeName(REG_DWORD) == L"REG_DWORD");
    CHECK(value_format::TypeName(REG_DWORD_BIG_ENDIAN) == L"REG_DWORD_BIG_ENDIAN");
    std::vector<BYTE> bytes;
    CHECK(value_format::ParseHex(L"de,ad,BE,ef", &bytes) && bytes == std::vector<BYTE>({0xDE, 0xAD, 0xBE, 0xEF}));
    const auto multi = value_format::MultiStringData(std::vector<std::wstring>{L"a", L"bc"});
    CHECK(value_format::MultiStringItems(multi) == std::vector<std::wstring>({L"a", L"bc"}));
}

void RegFile()
{
    const std::wstring content = L"Windows Registry Editor Version 5.00\r\n\r\n"
                                 L"[HKEY_CURRENT_USER\\Software\\Test]\r\n"
                                 L"@=\"default\"\r\n"
                                 L"\"Number\"=dword:0000002a\r\n"
                                 L"\"Data\"=hex:01,02,\\\r\n  03\r\n"
                                 L"\"Gone\"=-\r\n\r\n"
                                 L"[-HKEY_CURRENT_USER\\Software\\Removed]\r\n";
    regfile::Document document;
    CHECK(regfile::Parse(content, &document));
    CHECK(document.key_order.size() == 2);
    auto& key = document.keys[L"hkey_current_user\\software\\test"];
    CHECK(key.path == L"HKEY_CURRENT_USER\\Software\\Test" && key.values.size() == 3 && key.removed_values.size() == 1);
    CHECK(key.values[L"number"].type == REG_DWORD && key.values[L"number"].data == std::vector<BYTE>({42, 0, 0, 0}));
    CHECK(key.values[L"data"].data == std::vector<BYTE>({1, 2, 3}));
    CHECK(document.keys[L"hkey_current_user\\software\\removed"].removed);

    std::vector<regfile::Operation> operations;
    CHECK(regfile::ParseOperations(content, &operations));
    std::vector<regfile::Operation> again;
    CHECK(regfile::ParseOperations(regfile::RenderReg(operations), &again) && again.size() == operations.size());

    wchar_t folder[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, folder);
    const std::wstring path = std::wstring(folder) + L"regkit_tests.reg";
    const std::wstring two_roots = L"\xFEFF" + content + L"\r\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\A\\B]\r\n\"x\"=\"1\"\r\n";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    WriteFile(file, two_roots.data(), static_cast<DWORD>(two_roots.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
    std::vector<regfile::VirtualRoot> roots;
    std::wstring error;
    CHECK(regfile::LoadVirtualRoots(path, &roots, &error, nullptr, nullptr) && roots.size() == 2);
    const VirtualRegistryKey* b = roots.size() == 2 ? roots[1].data->root.get() : nullptr;
    for (const wchar_t* part : {L"software", L"a", L"b"})
    {
        b = b && b->children.contains(part) ? b->children.at(part).get() : nullptr;
    }
    CHECK(b && b->name == L"B" && b->values.contains(L"x"));
    CHECK(roots.size() == 2 && roots[0].name == L"HKEY_CURRENT_USER");
    DeleteFileW(path.c_str());
}

void Records()
{
    const std::wstring field = L"a\tb\\c\nd";
    CHECK(record_fields::Unescape(record_fields::Escape(field)) == field);
    std::wstring line;
    record_fields::AppendRecord(&line, {L"one", field});
    line.pop_back();
    CHECK(record_fields::DecodeRecord(line) == std::vector<std::wstring>({L"one", field}));
    uint64_t number = 0;
    CHECK(record_fields::ParseUnsigned(L"42", 100, &number) && number == 42);
    CHECK(!record_fields::ParseUnsigned(L"101", 100, &number));

    std::wstring error;
    std::wstring name;
    uint64_t count = 0;
    json::Reader reader(L"{\"name\": \"x\\u0041\", \"count\": 7, \"skip\": [1, {\"a\": null}]}", &error);
    CHECK(reader.Object([&](const std::wstring& key) {
        return key == L"name" ? reader.String(&name) : key == L"count" ? reader.Unsigned(&count) : reader.Skip();
    }) && reader.End());
    CHECK(name == L"xA" && count == 7);
    json::Reader bad(L"{\"a\": }", &error);
    CHECK(!bad.Object([&](const std::wstring&) { return bad.Skip(); }));
    std::wstring written;
    json::AppendString(&written, L"q\"\\");
    CHECK(written == L"\"q\\\"\\\\\"");
}

void Search()
{
    search::Matcher plain({L"foo", false, false, false});
    CHECK(plain.Find(L"xFOOx").matched && plain.Find(L"xFOOx").start == 1);
    search::Matcher whole({L"foo", true, true, false});
    CHECK(!whole.Find(L"foobar").matched && whole.Find(L"foo").matched && !whole.Find(L"FOO").matched);
    search::Matcher regex({L"f(o+)", false, false, true});
    CHECK(regex.valid() && regex.Find(L"xfoooy").length == 4);
    std::wstring replaced;
    regex.Replace(L"foo", L"b$1", &replaced);
    CHECK(replaced == L"boo");
    CHECK(!search::Matcher({L"(", false, false, true}).valid());
}

void Compare()
{
    search::compare::Snapshot first;
    search::compare::Snapshot second;
    first.keys[L"a"].values[L"same"] = {L"same", REG_SZ, value_format::StringData(L"1")};
    first.keys[L"a"].values[L"diff"] = {L"diff", REG_SZ, value_format::StringData(L"1")};
    second.keys[L"a"] = first.keys[L"a"];
    second.keys[L"a"].values[L"diff"].data = value_format::StringData(L"2");
    second.keys[L"b"];
    const auto differences = search::compare::BuildRows(first, second);
    const auto all = search::compare::BuildRows(first, second, search::compare::RowFilter::kAll);
    CHECK(std::none_of(differences.begin(), differences.end(), [](const auto& row) { return row.matches; }));
    CHECK(std::any_of(differences.begin(), differences.end(), [](const auto& row) { return row.value_name == L"diff"; }));
    CHECK(std::any_of(differences.begin(), differences.end(), [](const auto& row) { return row.is_key; }));
    CHECK(all.size() > differences.size());
    std::vector<search::compare::Row> parsed;
    CHECK(search::compare::ParseRows(search::compare::SerializeRows(all), &parsed) && parsed.size() == all.size());
}

void Trace()
{
    trace::Normalizers normalizers;
    normalizers.key = [](const std::wstring& path) { return registry_path::Normalize(path); };
    normalizers.display = normalizers.key;
    std::vector<trace::Entry> entries;
    std::wstring error;
    const std::string buffer = "\xEF\xBB\xBFHKLM\\SOFTWARE\\A : Value\r\n\r\nHKCU\\B : (Default)\nno separator\n";
    CHECK(trace::ParseEntries(buffer, normalizers, [&](trace::Entry&& entry) { entries.push_back(std::move(entry)); return true; }, &error));
    CHECK(entries.size() == 2);
    CHECK(entries[0].key_path == L"HKEY_LOCAL_MACHINE\\SOFTWARE\\A" && entries[0].value_name == L"Value");
    CHECK(entries[1].value_name.empty() && entries[1].has_value);
    CHECK(!trace::ParseEntries("nothing here", normalizers, [](trace::Entry&&) { return true; }, &error));

    CHECK(trace::NormalizeSelectionPath(L"REGISTRY\\\\MACHINE") == L"REGISTRY\\MACHINE");
    CHECK(trace::NormalizeKeyPathBasic(L"\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\.txt") == L"HKEY_CLASSES_ROOT\\.txt");
    const std::wstring services = trace::NormalizeKeyPathBasic(L"HKLM\\SYSTEM\\CurrentControlSet\\Services");
    CHECK(services.starts_with(L"HKEY_LOCAL_MACHINE\\SYSTEM\\ControlSet") && services.ends_with(L"\\Services"));
    CHECK(trace::MapControlSetToCurrent(services).empty());
    const std::wstring other = trace::MapControlSetToCurrent(L"HKEY_LOCAL_MACHINE\\SYSTEM\\ControlSet999\\Services");
    CHECK(other == services);
}

void ResourceList()
{
    const std::string hex = "0100000000000000000000000000000008000000030100000010000000000000"
                            "00F009000000000003010000000010000000000000F0C1090000000003010000"
                            "0000000A0000000000002000000000000301000000E0200A000000000020DF00"
                            "00000000030100000000020B00000000008025B80000000003010000009027C3"
                            "0000000000302407000000000301000000F09FCB000000000010600100000000"
                            "07010002000000000100000000302F0700000000";
    std::vector<BYTE> data;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        data.push_back(static_cast<BYTE>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    const auto tables = resource_list::Decode(REG_RESOURCE_LIST, data.data(), data.size());
    CHECK(tables && tables->size() == 2);
    CHECK(tables && tables->front().rows.size() == 1 && tables->front().rows[0].back() == L"8");
    CHECK(tables && (*tables)[1].rows.size() == 8 && (*tables)[1].rows[0][0] == L"0x0000000000001000" && (*tables)[1].rows[0][1] == L"0x0009F000");
    CHECK(tables && (*tables)[1].rows[7][0] == L"0x0000000100000000" && (*tables)[1].rows[7][1] == L"0x000000072F300000");
    CHECK(!resource_list::Decode(REG_RESOURCE_LIST, data.data(), 40));
}

} // namespace

int main()
{
    RegistryPath();
    HiveFiles();
    ValueFormat();
    RegFile();
    Records();
    Search();
    Compare();
    Trace();
    ResourceList();
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
