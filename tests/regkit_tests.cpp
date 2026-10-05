// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "records/escaped_fields.h"
#include "records/json.h"
#include "regfile/reg_file.h"
#include "registry/registry_path.h"
#include "registry/value_format.h"
#include "search/compare.h"
#include "search/search.h"
#include "trace/trace_parser.h"

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
    CHECK(registry_path::Normalize(L"HKLM\\Software\\") == L"HKEY_LOCAL_MACHINE\\Software");
    CHECK(registry_path::Normalize(L"Computer\\HKEY_CURRENT_USER\\Console") == L"HKEY_CURRENT_USER\\Console");
    CHECK(registry_path::Normalize(L"\\REGISTRY\\MACHINE\\SYSTEM") == L"HKEY_LOCAL_MACHINE\\SYSTEM");
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
    const auto& key = document.keys[L"HKEY_CURRENT_USER\\Software\\Test"];
    CHECK(key.values.size() == 3 && key.removed_values.size() == 1);
    CHECK(key.values.at(L"Number").type == REG_DWORD && key.values.at(L"Number").data == std::vector<BYTE>({42, 0, 0, 0}));
    CHECK(key.values.at(L"Data").data == std::vector<BYTE>({1, 2, 3}));
    CHECK(document.keys[L"HKEY_CURRENT_USER\\Software\\Removed"].removed);

    std::vector<regfile::Operation> operations;
    CHECK(regfile::ParseOperations(content, &operations));
    std::vector<regfile::Operation> again;
    CHECK(regfile::ParseOperations(regfile::RenderReg(operations), &again) && again.size() == operations.size());
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
    CHECK(!whole.Find(L"foobar").matched && whole.Find(L"a foo b").matched && !whole.Find(L"FOO").matched);
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
}

} // namespace

int main()
{
    std::printf("RegistryPath
"); std::fflush(stdout); RegistryPath();
    std::printf("ValueFormat
"); std::fflush(stdout); ValueFormat();
    std::printf("RegFile
"); std::fflush(stdout); RegFile();
    std::printf("Records
"); std::fflush(stdout); Records();
    std::printf("Search
"); std::fflush(stdout); Search();
    std::printf("Compare
"); std::fflush(stdout); Compare();
    std::printf("Trace
"); std::fflush(stdout); Trace();
    std::printf(failures ? "%d failed
" : "all passed
", failures);
    return failures ? 1 : 0;
}
