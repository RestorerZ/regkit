// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "registry/key_access.h"

#include "win32/handle_owner.h"
#include "win32/process_rights.h"
#include "win32/translation.h"

#include <authz.h>
#include <sddl.h>

#include <initializer_list>

namespace regkit::key_access
{
namespace
{

constexpr wchar_t kSystemSid[] = L"S-1-5-18";
constexpr wchar_t kTrustedInstallerSid[] = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";
constexpr std::initializer_list<const wchar_t*> kWellKnownGroups = {L"S-1-5-32-544", L"S-1-1-0", L"S-1-5-11"};

struct FreeContext
{
    void operator()(AUTHZ_CLIENT_CONTEXT_HANDLE context) const noexcept
    {
        AuthzFreeContext(context);
    }
};
using Context = util::UniqueResource<AUTHZ_CLIENT_CONTEXT_HANDLE, FreeContext>;

AUTHZ_RESOURCE_MANAGER_HANDLE ResourceManager()
{
    static const AUTHZ_RESOURCE_MANAGER_HANDLE manager = [] {
        AUTHZ_RESOURCE_MANAGER_HANDLE handle = nullptr;
        AuthzInitializeResourceManager(AUTHZ_RM_FLAG_NO_AUDIT, nullptr, nullptr, nullptr, L"RegKit", &handle);
        return handle;
    }();
    return manager;
}

ACCESS_MASK Check(AUTHZ_CLIENT_CONTEXT_HANDLE context, PSECURITY_DESCRIPTOR descriptor)
{
    AUTHZ_ACCESS_REQUEST request = {};
    request.DesiredAccess = MAXIMUM_ALLOWED;
    ACCESS_MASK granted = 0;
    DWORD error = ERROR_SUCCESS;
    AUTHZ_ACCESS_REPLY reply = {};
    reply.ResultListLength = 1;
    reply.GrantedAccessMask = &granted;
    reply.Error = &error;
    return context && AuthzAccessCheck(0, context, &request, nullptr, descriptor, nullptr, 0, &reply, nullptr) && error == ERROR_SUCCESS ? granted : 0;
}

// service sids have no account to expand, so their groups are listed explicitly
Context FromSid(PSID sid, std::initializer_list<const wchar_t*> groups)
{
    const LUID none = {};
    Context context;
    if (!AuthzInitializeContextFromSid(groups.size() ? AUTHZ_SKIP_TOKEN_GROUPS : 0, sid, ResourceManager(), nullptr, none, nullptr, context.put()))
    {
        return Context();
    }
    std::vector<util::UniqueLocal<PSID>> owned;
    std::vector<SID_AND_ATTRIBUTES> sids;
    for (const wchar_t* text : groups)
    {
        util::UniqueLocal<PSID> group;
        if (ConvertStringSidToSidW(text, group.put()))
        {
            sids.push_back({group.get(), SE_GROUP_ENABLED | SE_GROUP_MANDATORY});
            owned.push_back(std::move(group));
        }
    }
    Context extended;
    const bool added = sids.empty() || AuthzAddSidsToContext(context.get(), sids.data(), static_cast<DWORD>(sids.size()), nullptr, 0, extended.put());
    return !added ? Context() : sids.empty() ? std::move(context)
                                             : std::move(extended);
}

Context FromSidText(const wchar_t* text, std::initializer_list<const wchar_t*> groups)
{
    util::UniqueLocal<PSID> sid;
    return ConvertStringSidToSidW(text, sid.put()) ? FromSid(sid.get(), groups) : Context();
}

Context FromToken(HANDLE token)
{
    const LUID none = {};
    Context context;
    return token && AuthzInitializeContextFromToken(0, token, ResourceManager(), nullptr, none, nullptr, context.put()) ? std::move(context) : Context();
}

} // namespace

ACCESS_MASK ForSid(PSECURITY_DESCRIPTOR descriptor, PSID sid)
{
    Context context = FromSid(sid, {});
    if (!context)
    {
        context = FromSid(sid, kWellKnownGroups);
    }
    return Check(context.get(), descriptor);
}

std::vector<RunLevel> ForRunLevels(PSECURITY_DESCRIPTOR descriptor)
{
    std::vector<RunLevel> levels;
    util::UniqueHandle token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()))
    {
        levels.push_back({util::TrNoop(L"Current user"), Check(FromToken(token.get()).get(), descriptor)});
        TOKEN_LINKED_TOKEN linked = {};
        DWORD size = 0;
        if (!util::IsProcessElevated() && util::IsUacEnabled() && GetTokenInformation(token.get(), TokenLinkedToken, &linked, sizeof(linked), &size))
        {
            const util::UniqueHandle admin(linked.LinkedToken);
            levels.push_back({util::TrNoop(L"Administrator"), Check(FromToken(admin.get()).get(), descriptor)});
        }
    }
    levels.push_back({L"SYSTEM", Check(FromSidText(kSystemSid, kWellKnownGroups).get(), descriptor)});
    levels.push_back({L"TrustedInstaller", Check(FromSidText(kSystemSid, {L"S-1-5-32-544", L"S-1-1-0", L"S-1-5-11", kTrustedInstallerSid}).get(), descriptor)});
    return levels;
}

bool CanWrite(ACCESS_MASK granted) noexcept
{
    return (granted & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY)) == (KEY_SET_VALUE | KEY_CREATE_SUB_KEY);
}

std::wstring Describe(ACCESS_MASK granted)
{
    if ((granted & KEY_ALL_ACCESS) == KEY_ALL_ACCESS)
    {
        return util::Tr(L"Full control");
    }
    const bool read = (granted & KEY_QUERY_VALUE) != 0;
    std::wstring text = CanWrite(granted) ? (read ? util::Tr(L"Read and write") : util::Tr(L"Write")) : read ? util::Tr(L"Read")
                                                                                                             : util::Tr(L"No access");
    // an owner can always rewrite the dacl and so gain any access
    if (!CanWrite(granted) && (granted & (WRITE_DAC | WRITE_OWNER)))
    {
        text.append(L", ").append(util::Tr(L"can change permissions"));
    }
    return text;
}

} // namespace regkit::key_access
