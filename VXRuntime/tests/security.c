// advapi32's security objects (src/security.c): SIDs and their strings, ACLs and ACE walking, security descriptors in
// both layouts and their conversions, SDDL round trips, tokens (restricted, duplicated, impersonating, also through
// NtSetInformationThread of src/extras.c), privileges, AccessCheck, CheckTokenMembership, object and named security,
// SetEntriesInAcl, and the errors each sets. Made-up data only (the domain S-1-5-21-111-222-333 below is no machine's);
// the files are the test's own, and the Mac's root folder is only read (and a change to it refused, as it is root's).
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s (last error %lu)\n", __LINE__, #c, GetLastError()); failures++; } } while (0)

static const WCHAR *USER = L"S-1-5-21-1000-1000-1000-1001";   // the runtime's user (src/security.c)
static PSID sid_of(const WCHAR *text) { PSID s = NULL; if (!ConvertStringSidToSidW(text, &s)) printf("no SID for %ls\n", text); return s; }
static int sid_is(PSID sid, const WCHAR *text) {
    LPWSTR s = NULL;
    int ok = sid && ConvertSidToStringSidW(sid, &s) && !wcscmp(s, text);
    if (sid && s && !ok) printf("  SID %ls, expected %ls\n", s, text);
    LocalFree(s);
    return ok;
}
static int sddl_round_trip(const WCHAR *in, const WCHAR *out, DWORD info) {
    PSECURITY_DESCRIPTOR sd = NULL; LPWSTR s = NULL; ULONG n = 0, len = 0;
    int ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(in, SDDL_REVISION_1, &sd, &n) && IsValidSecurityDescriptor(sd) &&
             n == GetSecurityDescriptorLength(sd) && ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, info, &s, &len) &&
             !wcscmp(s, out) && len == wcslen(s) + 1;
    if (!ok) printf("  SDDL %ls -> %ls (expected %ls), error %lu\n", in, s ? s : L"(none)", out, GetLastError());
    LocalFree(sd); LocalFree(s);
    return ok;
}
static PSECURITY_DESCRIPTOR sddl(const WCHAR *s) {
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(s, SDDL_REVISION_1, &sd, NULL)) printf("  SDDL %ls refused: %lu\n", s, GetLastError());
    return sd;
}
static ACE_HEADER *ace_at(PACL acl, DWORD i) { void *a = NULL; return GetAce(acl, i, &a) ? a : NULL; }
static PSID ace_sid(ACE_HEADER *a) { return &((ACCESS_ALLOWED_ACE *)a)->SidStart; }
static DWORD ace_mask(ACE_HEADER *a) { return ((ACCESS_ALLOWED_ACE *)a)->Mask; }
static void *token_info(HANDLE t, TOKEN_INFORMATION_CLASS c) {
    DWORD n = 0;
    GetTokenInformation(t, c, NULL, 0, &n);
    void *p = n ? malloc(n) : NULL;
    if (p && !GetTokenInformation(t, c, p, n, &n)) { free(p); p = NULL; }
    return p;
}
static DWORD integrity_of(HANDLE t) {
    TOKEN_MANDATORY_LABEL *l = token_info(t, TokenIntegrityLevel);
    DWORD rid = l ? *GetSidSubAuthority(l->Label.Sid, 0) : 0xffffffff;
    free(l);
    return rid;
}
static int privilege_state(HANDLE t, DWORD luid) {    // a privilege's attributes, -1 if the token lacks it
    TOKEN_PRIVILEGES *tp = token_info(t, TokenPrivileges);
    int r = -1;
    for (DWORD i = 0; tp && i < tp->PrivilegeCount; i++) if (tp->Privileges[i].Luid.LowPart == luid && !tp->Privileges[i].Luid.HighPart) r = (int)tp->Privileges[i].Attributes;
    free(tp);
    return r;
}
static int privilege_on(HANDLE t, DWORD luid) { int a = privilege_state(t, luid); return a >= 0 && (a & SE_PRIVILEGE_ENABLED); }
static DWORD privilege_count(HANDLE t) { TOKEN_PRIVILEGES *tp = token_info(t, TokenPrivileges); DWORD n = tp ? tp->PrivilegeCount : 0xffffffff; free(tp); return n; }
// The rights an ACL's allow ACEs give SIDs other than `sid`, and the first allow ACE's for `sid`.
static DWORD others_get(PACL acl, PSID sid) {
    DWORD r = 0;
    for (DWORD i = 0; acl && i < acl->AceCount; i++) { ACE_HEADER *a = ace_at(acl, i); if (a->AceType == ACCESS_ALLOWED_ACE_TYPE && !EqualSid(ace_sid(a), sid)) r |= ace_mask(a); }
    return r;
}
static DWORD sid_gets(PACL acl, PSID sid) {
    for (DWORD i = 0; acl && i < acl->AceCount; i++) { ACE_HEADER *a = ace_at(acl, i); if (a->AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid(ace_sid(a), sid)) return ace_mask(a); }
    return 0;
}
static GENERIC_MAPPING map = { 0x1 | READ_CONTROL, 0x2, 0x4, 0xf | STANDARD_RIGHTS_REQUIRED };
// AccessCheck of `desired` against an SDDL descriptor: the access granted (0 when denied), *err its last error.
static DWORD check(HANDLE token, const WCHAR *text, DWORD desired, DWORD *err) {
    PSECURITY_DESCRIPTOR sd = sddl(text);
    BYTE ps[64]; DWORD pslen = sizeof ps, granted = 0xdead; BOOL status = 2;
    SetLastError(0);
    BOOL ok = AccessCheck(sd, token, desired, &map, (PPRIVILEGE_SET)ps, &pslen, &granted, &status);
    if (err) *err = GetLastError();
    LocalFree(sd);
    if (!ok) return 0xffffffff;
    return status ? granted : (granted == 0 ? 0 : 0xbad);
}

typedef LONG (NTAPI *nt_set_thread_fn)(HANDLE, int, const void *, ULONG);
static nt_set_thread_fn nt_set_thread;                    // ntdll's NtSetInformationThread (looked up)

static HANDLE g_ready, g_go, g_done;
static int g_thread_had_token, g_thread_token_before;
static DWORD WINAPI other_thread(void *arg) {
    (void)arg;
    HANDLE t;
    g_thread_token_before = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t);
    if (g_thread_token_before) CloseHandle(t);
    SetEvent(g_ready);
    WaitForSingleObject(g_go, INFINITE);
    g_thread_had_token = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t);
    if (g_thread_had_token) { g_thread_had_token = integrity_of(t) == SECURITY_MANDATORY_LOW_RID; CloseHandle(t); }
    RevertToSelf();
    SetEvent(g_done);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return 42;    // CreateProcessAsUserW's child

    // ---- SIDs ----
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    PSID users = NULL, bad = NULL;
    CHECK(AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0, &users));
    CHECK(IsValidSid(users) && GetLengthSid(users) == 16 && *GetSidSubAuthorityCount(users) == 2 && *GetSidSubAuthority(users, 1) == 545);
    CHECK(GetSidIdentifierAuthority(users)->Value[5] == 5 && sid_is(users, L"S-1-5-32-545"));
    CHECK(!AllocateAndInitializeSid(&nt, 9, 1, 2, 3, 4, 5, 6, 7, 8, &bad) && GetLastError() == ERROR_INVALID_SID);
    BYTE copy[SECURITY_MAX_SID_SIZE];
    CHECK(!CopySid(8, copy, users) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    CHECK(CopySid(sizeof copy, copy, users) && EqualSid(copy, users));
    PSID admins = sid_of(L"BA"), world = sid_of(L"WD"), user = sid_of(USER), low = sid_of(L"LW"), auth_users = sid_of(L"AU");
    PSID restricted_code = sid_of(L"RC"), write_restricted = sid_of(L"WR");
    CHECK(sid_is(admins, L"S-1-5-32-544") && sid_is(world, L"S-1-1-0") && sid_is(low, L"S-1-16-4096"));
    CHECK(!EqualSid(admins, users) && EqualPrefixSid(admins, users) && !EqualPrefixSid(admins, world));
    CHECK(GetSidLengthRequired(3) == 20);
    BYTE built[20];
    CHECK(InitializeSid(built, &nt, 3));
    *GetSidSubAuthority(built, 0) = 21; *GetSidSubAuthority(built, 1) = 111; *GetSidSubAuthority(built, 2) = 222;
    CHECK(sid_is(built, L"S-1-5-21-111-222"));
    built[0] = 2;
    CHECK(!IsValidSid(built) && !EqualSid(built, built));
    // strings: numbers in both forms, abbreviations, and what is refused
    static const struct { const WCHAR *in, *out; } good[] = {
        { L"S-1-5-18", L"S-1-5-18" }, { L"SY", L"S-1-5-18" }, { L"ba", L"S-1-5-32-544" }, { L"S-1-5", L"S-1-5" },
        { L"S-1-0x123456789ABC-7", L"S-1-0x123456789ABC-7" }, { L"S-1-5-0x20-0x221", L"S-1-5-32-545" },
        { L"S-1-5-21-111-222-333-1004", L"S-1-5-21-111-222-333-1004" }, { L"LA", L"S-1-5-21-1000-1000-1000-500" },
        { L"S-1-15-2-1", L"S-1-15-2-1" }, { L"AC", L"S-1-15-2-1" }, { L"ME", L"S-1-16-8192" }, { L"OW", L"S-1-3-4" },
    };
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) { PSID s = sid_of(good[i].in); CHECK(sid_is(s, good[i].out)); LocalFree(s); }
    static const WCHAR *refused[] = { L"", L"S", L"S-1", L"S-2-5-18", L"X-1-5-18", L"S-1-5-18-", L"S-1-5-18x", L"S-1-5-4294967296",
                                      L"S-1-0x1000000000000-1", L"S-1-5-1-2-3-4-5-6-7-8-9-10-11-12-13-14-15-16", L"ZZ", L"DA", L"S-1-5- 18" };
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++) {
        PSID s = (PSID)1;
        CHECK(!ConvertStringSidToSidW(refused[i], &s) && GetLastError() == ERROR_INVALID_SID);
    }
    PSID s0;
    CHECK(!ConvertStringSidToSidW(NULL, &s0) && GetLastError() == ERROR_INVALID_PARAMETER);
    LPSTR narrow = NULL; PSID na = NULL;
    CHECK(ConvertStringSidToSidA("S-1-5-11", &na) && ConvertSidToStringSidA(na, &narrow) && !strcmp(narrow, "S-1-5-11"));
    LocalFree(na); LocalFree(narrow);
    LPWSTR wide_text = NULL;
    CHECK(!ConvertSidToStringSidW(built, &wide_text) && GetLastError() == ERROR_INVALID_SID);
    // well-known SIDs
    static const struct { WELL_KNOWN_SID_TYPE t; const WCHAR *s; } wk[] = {
        { WinNullSid, L"S-1-0-0" }, { WinWorldSid, L"S-1-1-0" }, { WinNtAuthoritySid, L"S-1-5" }, { WinInteractiveSid, L"S-1-5-4" },
        { WinAuthenticatedUserSid, L"S-1-5-11" }, { WinRestrictedCodeSid, L"S-1-5-12" }, { WinLocalSystemSid, L"S-1-5-18" },
        { WinBuiltinAdministratorsSid, L"S-1-5-32-544" }, { WinBuiltinUsersSid, L"S-1-5-32-545" }, { WinUntrustedLabelSid, L"S-1-16-0" },
        { WinLowLabelSid, L"S-1-16-4096" }, { WinHighLabelSid, L"S-1-16-12288" }, { WinWriteRestrictedCodeSid, L"S-1-5-33" },
        { WinCreatorOwnerRightsSid, L"S-1-3-4" }, { WinBuiltinAnyPackageSid, L"S-1-15-2-1" }, { WinConsoleLogonSid, L"S-1-2-1" },
    };
    for (size_t i = 0; i < sizeof wk / sizeof *wk; i++) {
        BYTE s[SECURITY_MAX_SID_SIZE]; DWORD n = sizeof s;
        CHECK(CreateWellKnownSid(wk[i].t, NULL, s, &n) && n == GetLengthSid(s) && sid_is(s, wk[i].s) && IsWellKnownSid(s, wk[i].t));
    }
    DWORD need = 0;
    CHECK(!CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, NULL, &need) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && need == 16);
    PSID domain = sid_of(L"S-1-5-21-111-222-333");
    BYTE da[SECURITY_MAX_SID_SIZE]; DWORD dan = sizeof da;
    CHECK(CreateWellKnownSid(WinAccountAdministratorSid, domain, da, &dan) && sid_is(da, L"S-1-5-21-111-222-333-500"));
    dan = sizeof da;
    CHECK(!CreateWellKnownSid(WinAccountAdministratorSid, NULL, da, &dan) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(IsWellKnownSid(users, WinBuiltinUsersSid) && !IsWellKnownSid(users, WinBuiltinAdministratorsSid));
    FreeSid(users);
    users = sid_of(L"BU");

    // ---- ACLs ----
    BYTE aclbuf[256]; PACL acl = (PACL)aclbuf;
    CHECK(!InitializeAcl(acl, 4, ACL_REVISION) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    CHECK(!InitializeAcl(acl, sizeof aclbuf, 7) && GetLastError() == ERROR_REVISION_MISMATCH);
    CHECK(InitializeAcl(acl, sizeof aclbuf, ACL_REVISION));
    CHECK(acl->AclRevision == ACL_REVISION && acl->AclSize == 256 && acl->AceCount == 0 && IsValidAcl(acl));
    CHECK(AddAccessAllowedAce(acl, ACL_REVISION, FILE_GENERIC_READ, users));
    CHECK(AddAccessDeniedAceEx(acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, DELETE, world));
    CHECK(!AddAccessAllowedAceEx(acl, ACL_REVISION, 0x20, GENERIC_ALL, world) && GetLastError() == ERROR_INVALID_FLAGS);
    CHECK(!AddAccessAllowedAce(acl, 9, GENERIC_ALL, world) && GetLastError() == ERROR_REVISION_MISMATCH);
    CHECK(!AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, built) && GetLastError() == ERROR_INVALID_SID);
    CHECK(AddMandatoryAce(acl, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, low));
    CHECK(!AddMandatoryAce(acl, ACL_REVISION, 0, 8, low) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(!AddMandatoryAce(acl, ACL_REVISION, 0, 1, world) && GetLastError() == ERROR_INVALID_LABEL);
    CHECK(AddAuditAccessAce(acl, ACL_REVISION, GENERIC_WRITE, world, TRUE, FALSE));
    CHECK(acl->AceCount == 4 && IsValidAcl(acl));
    ACE_HEADER *a = ace_at(acl, 0);
    CHECK(a && a->AceType == ACCESS_ALLOWED_ACE_TYPE && a->AceFlags == 0 && a->AceSize == 24 && ace_mask(a) == FILE_GENERIC_READ && EqualSid(ace_sid(a), users));
    CHECK(a && (BYTE *)a == aclbuf + 8);
    a = ace_at(acl, 1);
    CHECK(a && a->AceType == ACCESS_DENIED_ACE_TYPE && a->AceFlags == 3 && a->AceSize == 20 && ace_mask(a) == DELETE && EqualSid(ace_sid(a), world));
    a = ace_at(acl, 2);
    CHECK(a && a->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE && a->AceSize == 20 && ace_mask(a) == SYSTEM_MANDATORY_LABEL_NO_WRITE_UP && EqualSid(ace_sid(a), low));
    a = ace_at(acl, 3);
    CHECK(a && a->AceType == SYSTEM_AUDIT_ACE_TYPE && a->AceFlags == SUCCESSFUL_ACCESS_ACE_FLAG && ace_mask(a) == GENERIC_WRITE);
    CHECK(!ace_at(acl, 4) && GetLastError() == ERROR_INVALID_PARAMETER);
    ACL_SIZE_INFORMATION si; ACL_REVISION_INFORMATION ri;
    CHECK(GetAclInformation(acl, &si, sizeof si, AclSizeInformation) && si.AceCount == 4 && si.AclBytesInUse == 92 && si.AclBytesFree == 164);
    CHECK(GetAclInformation(acl, &ri, sizeof ri, AclRevisionInformation) && ri.AclRevision == ACL_REVISION);
    CHECK(!GetAclInformation(acl, &si, 4, AclSizeInformation) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    void *free_ace = NULL;
    CHECK(FindFirstFreeAce(acl, &free_ace) && free_ace == aclbuf + 92);
    BYTE small_buf[16]; PACL small_acl = (PACL)small_buf;
    CHECK(InitializeAcl(small_acl, sizeof small_buf, ACL_REVISION));
    CHECK(!AddAccessAllowedAce(small_acl, ACL_REVISION, 1, world) && GetLastError() == ERROR_ALLOTTED_SPACE_EXCEEDED);
    BYTE audit[32]; memcpy(audit, ace_at(acl, 3), ace_at(acl, 3)->AceSize);
    CHECK(DeleteAce(acl, 1) && acl->AceCount == 3 && ace_at(acl, 1)->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE);
    CHECK(!DeleteAce(acl, 3) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(AddAce(acl, ACL_REVISION, 0, audit, ((ACE_HEADER *)audit)->AceSize) && acl->AceCount == 4 && ace_at(acl, 0)->AceType == SYSTEM_AUDIT_ACE_TYPE &&
          ace_at(acl, 1)->AceType == ACCESS_ALLOWED_ACE_TYPE && IsValidAcl(acl));
    // object ACEs need ACL_REVISION_DS and carry their GUIDs between mask and SID
    static const GUID prop = { 0xbf967aba, 0x0de6, 0x11d0, { 0xa2, 0x85, 0x00, 0xaa, 0x00, 0x30, 0x49, 0xe2 } };
    BYTE objbuf[128]; PACL objacl = (PACL)objbuf;
    CHECK(InitializeAcl(objacl, sizeof objbuf, ACL_REVISION));
    CHECK(!AddAccessAllowedObjectAce(objacl, ACL_REVISION, 0, 0x10, (GUID *)&prop, NULL, world) && GetLastError() == ERROR_REVISION_MISMATCH);
    CHECK(AddAccessAllowedObjectAce(objacl, ACL_REVISION_DS, 0, 0x10, (GUID *)&prop, NULL, world) && objacl->AclRevision == ACL_REVISION_DS);
    ACCESS_ALLOWED_OBJECT_ACE *oa = (ACCESS_ALLOWED_OBJECT_ACE *)ace_at(objacl, 0);
    CHECK(oa && oa->Header.AceSize == 40 && oa->Flags == ACE_OBJECT_TYPE_PRESENT && !memcmp(&oa->ObjectType, &prop, 16) &&
          EqualSid((BYTE *)oa + 28, world) && IsValidAcl(objacl));
    // ACLs that are not
    BYTE badacl[64];
    memcpy(badacl, aclbuf, 64); badacl[0] = 1; CHECK(!IsValidAcl((PACL)badacl));
    memcpy(badacl, aclbuf, 8); ((PACL)badacl)->AclSize = 8; ((PACL)badacl)->AceCount = 1; CHECK(!IsValidAcl((PACL)badacl));
    memcpy(badacl, aclbuf, 64); ((PACL)badacl)->AclSize = 64; ((ACE_HEADER *)(badacl + 8))->AceSize = 0; CHECK(!IsValidAcl((PACL)badacl));
    memcpy(badacl, aclbuf, 64); ((PACL)badacl)->AclSize = 64; ((PACL)badacl)->AceCount = 1; badacl[8 + 8 + 1] = 15; CHECK(!IsValidAcl((PACL)badacl));
    memcpy(badacl, objbuf, 64); badacl[0] = ACL_REVISION; CHECK(!IsValidAcl((PACL)badacl));
    DWORD mask = GENERIC_READ | DELETE;
    GENERIC_MAPPING fm = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
    MapGenericMask(&mask, &fm);
    CHECK(mask == (FILE_GENERIC_READ | DELETE));

    // ---- security descriptors ----
    SECURITY_DESCRIPTOR sd;
    CHECK(!InitializeSecurityDescriptor(&sd, 2) && GetLastError() == ERROR_UNKNOWN_REVISION);
    CHECK(InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) && IsValidSecurityDescriptor(&sd));
    SECURITY_DESCRIPTOR_CONTROL control; DWORD rev;
    CHECK(GetSecurityDescriptorControl(&sd, &control, &rev) && control == 0 && rev == 1);
    BYTE daclbuf[64], saclbuf[64]; PACL dacl = (PACL)daclbuf, sacl = (PACL)saclbuf;
    CHECK(InitializeAcl(dacl, sizeof daclbuf, ACL_REVISION) && AddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_ALL, world));
    CHECK(InitializeAcl(sacl, 28, ACL_REVISION) && AddMandatoryAce(sacl, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, low));
    CHECK(SetSecurityDescriptorOwner(&sd, users, FALSE) && SetSecurityDescriptorGroup(&sd, world, TRUE));
    CHECK(SetSecurityDescriptorDacl(&sd, TRUE, dacl, FALSE) && SetSecurityDescriptorSacl(&sd, TRUE, sacl, FALSE));
    CHECK(GetSecurityDescriptorControl(&sd, &control, &rev) && control == (SE_GROUP_DEFAULTED | SE_DACL_PRESENT | SE_SACL_PRESENT));
    PSID got_sid; BOOL defaulted, present; PACL got_acl;
    CHECK(GetSecurityDescriptorOwner(&sd, &got_sid, &defaulted) && got_sid == users && !defaulted);
    CHECK(GetSecurityDescriptorGroup(&sd, &got_sid, &defaulted) && got_sid == world && defaulted);
    CHECK(GetSecurityDescriptorDacl(&sd, &present, &got_acl, &defaulted) && present && got_acl == dacl && !defaulted);
    CHECK(GetSecurityDescriptorSacl(&sd, &present, &got_acl, &defaulted) && present && got_acl == sacl);
    CHECK(IsValidSecurityDescriptor(&sd) && GetSecurityDescriptorLength(&sd) == 40 + 16 + 12 + 64 + 28);
    DWORD len = 0;
    CHECK(!MakeSelfRelativeSD(&sd, NULL, &len) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && len == 20 + 16 + 12 + 28 + 64);
    BYTE rel[256];
    len = sizeof rel;
    CHECK(MakeSelfRelativeSD(&sd, rel, &len) && GetSecurityDescriptorLength(rel) == 140);
    len = 140;
    SECURITY_DESCRIPTOR_RELATIVE *r = (SECURITY_DESCRIPTOR_RELATIVE *)rel;
    CHECK(r->Revision == 1 && r->Control == (SE_SELF_RELATIVE | SE_GROUP_DEFAULTED | SE_DACL_PRESENT | SE_SACL_PRESENT));
    CHECK(r->Owner == 20 && r->Group == 36 && r->Sacl == 48 && r->Dacl == 76 && EqualSid(rel + 20, users) && !memcmp(rel + 76, daclbuf, 64));
    CHECK(GetSecurityDescriptorOwner(rel, &got_sid, &defaulted) && got_sid == rel + 20 && IsValidSecurityDescriptor(rel));
    CHECK(GetSecurityDescriptorLength(rel) == 140);
    CHECK(!SetSecurityDescriptorDacl(rel, TRUE, dacl, FALSE) && GetLastError() == ERROR_INVALID_SECURITY_DESCR);
    CHECK(!MakeSelfRelativeSD(rel, rel, &len) && GetLastError() == ERROR_BAD_DESCRIPTOR_FORMAT);
    SECURITY_DESCRIPTOR abs; BYTE ad[64], as[64], ao[68], ag[68];
    DWORD absn = 0, adn = 0, asn = 0, aon = 0, agn = 0;
    CHECK(!MakeAbsoluteSD(rel, NULL, &absn, NULL, &adn, NULL, &asn, NULL, &aon, NULL, &agn) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    CHECK(absn == sizeof abs && adn == 64 && asn == 28 && aon == 16 && agn == 12);
    CHECK(MakeAbsoluteSD(rel, &abs, &absn, (PACL)ad, &adn, (PACL)as, &asn, ao, &aon, ag, &agn));
    CHECK(abs.Owner == ao && abs.Group == ag && abs.Dacl == (PACL)ad && abs.Sacl == (PACL)as && EqualSid(ao, users) && !memcmp(ad, daclbuf, 64));
    BYTE rel2[256]; DWORD len2 = sizeof rel2;
    CHECK(MakeSelfRelativeSD(&abs, rel2, &len2) && GetSecurityDescriptorLength(rel2) == 140 && !memcmp(rel, rel2, 140));   // (the same bytes again)
    CHECK(!MakeAbsoluteSD(&abs, &abs, &absn, (PACL)ad, &adn, (PACL)as, &asn, ao, &aon, ag, &agn) && GetLastError() == ERROR_BAD_DESCRIPTOR_FORMAT);
    CHECK(SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED) && GetSecurityDescriptorControl(&sd, &control, &rev) && (control & SE_DACL_PROTECTED));
    CHECK(!SetSecurityDescriptorControl(&sd, SE_DACL_PRESENT, 0) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE) && GetSecurityDescriptorDacl(&sd, &present, &got_acl, &defaulted) && present && !got_acl);
    CHECK(SetSecurityDescriptorDacl(&sd, FALSE, dacl, FALSE) && GetSecurityDescriptorDacl(&sd, &present, &got_acl, &defaulted) && !present);
    BYTE badsd[140];
    memcpy(badsd, rel, 140); badsd[0] = 0; CHECK(!IsValidSecurityDescriptor(badsd));
    memcpy(badsd, rel, 140); ((SECURITY_DESCRIPTOR_RELATIVE *)badsd)->Owner = 4; CHECK(!IsValidSecurityDescriptor(badsd));
    memcpy(badsd, rel, 140); badsd[76] = 9; CHECK(!IsValidSecurityDescriptor(badsd));

    // ---- SDDL ----
    const DWORD ALL = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION;
    static const struct { const WCHAR *in, *out; } trips[] = {
        { L"O:BAG:SYD:(A;;GA;;;SY)(A;OICI;FA;;;BA)", NULL },
        { L"D:P(D;;GA;;;AN)(A;;0x1200a9;;;BU)", NULL },
        { L"D:NO_ACCESS_CONTROL", NULL }, { L"D:", NULL }, { L"", NULL },
        { L"S:(ML;;NW;;;LW)", NULL }, { L"S:(AU;SAFA;FA;;;WD)", NULL },
        { L"O:S-1-5-21-111-222-333-1004G:S-1-5-21-111-222-333-513D:(A;ID;0x1f0003;;;S-1-5-21-111-222-333-1004)", NULL },
        { L"D:(OA;CI;RPWP;bf967aba-0de6-11d0-a285-00aa003049e2;;AU)", NULL },
        { L"D:(OD;;CR;;{BF967ABA-0DE6-11D0-A285-00AA003049E2};WD)", L"D:(OD;;CR;;bf967aba-0de6-11d0-a285-00aa003049e2;WD)" },
        { L"D:(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;WD)", NULL }, { L"D:(A;;KA;;;SY)(A;;KX;;;BU)", L"D:(A;;KA;;;SY)(A;;KR;;;BU)" },
        { L"D:(A;;GXGR;;;WD)", L"D:(A;;GRGX;;;WD)" }, { L"D:PAI(A;;FA;;;SY)", NULL }, { L"D:ARAI(A;;FA;;;SY)", NULL },
        { L"D:(A;;0x10;;;WD)(A;;268435456;;;S-1-5-21-1000-1000-1000-500)", L"D:(A;;RP;;;WD)(A;;GA;;;LA)" },
        { L"O:LAG:LGD:(A;OICIIONPID;;;;CO)", L"O:LAG:LGD:(A;OICINPIOID;;;;CO)" },
        { L"S:(ML;;NWNRNX;;;HI)(TL;;CC;;;S-1-19-512-1024)", NULL },
        { L"O:baD:(a;;ga;;;wd)", L"O:BAD:(A;;GA;;;WD)" },
    };
    for (size_t i = 0; i < sizeof trips / sizeof *trips; i++) CHECK(sddl_round_trip(trips[i].in, trips[i].out ? trips[i].out : trips[i].in, ALL));
    // the bytes, as documented: SECURITY_DESCRIPTOR_RELATIVE, then the ACL and its ACE
    static const BYTE everyone_all[48] = {
        1, 0, 0x04, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0,
        2, 0, 28, 0, 1, 0, 0, 0,  0, 0, 20, 0, 0, 0, 0, 0x10,  1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0 };
    PSECURITY_DESCRIPTOR p = NULL; ULONG pn = 0;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &p, &pn) && pn == 48 && !memcmp(p, everyone_all, 48));
    LocalFree(p);
    static const BYTE low_label[48] = {
        1, 0, 0x10, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 0,
        2, 0, 28, 0, 1, 0, 0, 0,  0x11, 0, 20, 0, 1, 0, 0, 0,  1, 1, 0, 0, 0, 0, 0, 16, 0, 0x10, 0, 0 };
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;;NW;;;LW)", SDDL_REVISION_1, &p, &pn) && pn == 48 && !memcmp(p, low_label, 48));
    LocalFree(p);
    static const WCHAR *bad_sddl[] = { L"D:(A;;GA;;;XX)", L"D:(Z;;GA;;;WD)", L"Q:BA", L"D:(A;;GA;;;WD", L"O:BAO:SY", L"D:(A;;GA;;;WD;x)",
                                       L"D:(A;;GZ;;;WD)", L"D:(A;QQ;GA;;;WD)", L"D:(A;;GA;bf967aba-0de6-11d0-a285-00aa003049e2;;WD)",
                                       L"D:(OA;;GA;bf967aba-0de6;;WD)", L"D:NO_ACCESS_CONTROL(A;;GA;;;WD)", L"O:", L"D:(A;;0x100000000;;;WD)" };
    for (size_t i = 0; i < sizeof bad_sddl / sizeof *bad_sddl; i++) {
        p = NULL;
        BOOL ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(bad_sddl[i], SDDL_REVISION_1, &p, NULL);
        CHECK(!ok && (GetLastError() == ERROR_INVALID_PARAMETER || GetLastError() == ERROR_INVALID_SID));
        if (ok) { printf("  accepted: %ls\n", bad_sddl[i]); LocalFree(p); }
    }
    CHECK(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:", 2, &p, NULL) && GetLastError() == ERROR_UNKNOWN_REVISION);
    CHECK(!ConvertStringSecurityDescriptorToSecurityDescriptorW(NULL, SDDL_REVISION_1, &p, NULL) && GetLastError() == ERROR_INVALID_PARAMETER);
    PSECURITY_DESCRIPTOR mixed = sddl(L"O:BAG:SYD:(A;;GA;;;WD)S:(AU;FA;GA;;;WD)(ML;;NW;;;LW)");
    LPWSTR text = NULL; ULONG tn = 0;
    CHECK(ConvertSecurityDescriptorToStringSecurityDescriptorW(mixed, 1, OWNER_SECURITY_INFORMATION, &text, &tn) && !wcscmp(text, L"O:BA") && tn == 5);
    LocalFree(text);
    CHECK(ConvertSecurityDescriptorToStringSecurityDescriptorW(mixed, 1, DACL_SECURITY_INFORMATION, &text, NULL) && !wcscmp(text, L"D:(A;;GA;;;WD)"));
    LocalFree(text);
    CHECK(ConvertSecurityDescriptorToStringSecurityDescriptorW(mixed, 1, LABEL_SECURITY_INFORMATION, &text, NULL) && !wcscmp(text, L"S:(ML;;NW;;;LW)"));
    LocalFree(text);
    CHECK(ConvertSecurityDescriptorToStringSecurityDescriptorW(mixed, 1, SACL_SECURITY_INFORMATION, &text, NULL) && !wcscmp(text, L"S:(AU;FA;GA;;;WD)(ML;;NW;;;LW)"));
    LocalFree(text);
    CHECK(!ConvertSecurityDescriptorToStringSecurityDescriptorW(mixed, 2, ALL, &text, NULL) && GetLastError() == ERROR_UNKNOWN_REVISION);
    CHECK(!ConvertSecurityDescriptorToStringSecurityDescriptorW(badsd, 1, ALL, &text, NULL) && GetLastError() == ERROR_INVALID_SECURITY_DESCR);
    CHECK(ConvertSecurityDescriptorToStringSecurityDescriptorW(&abs, 1, ALL, &text, NULL) && !wcscmp(text, L"O:BUG:WDD:(A;;GA;;;WD)S:(ML;;NW;;;LW)"));
    LocalFree(text);
    LPSTR atext = NULL; PSECURITY_DESCRIPTOR asd = NULL;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorA("D:(A;;FR;;;IU)", 1, &asd, NULL) &&
          ConvertSecurityDescriptorToStringSecurityDescriptorA(asd, 1, DACL_SECURITY_INFORMATION, &atext, NULL) && !strcmp(atext, "D:(A;;FR;;;IU)"));
    LocalFree(asd); LocalFree(atext); LocalFree(mixed);

    // ---- tokens ----
    HANDLE tok = NULL;
    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &tok));
    DWORD n = 0;
    CHECK(!GetTokenInformation(tok, TokenUser, NULL, 0, &n) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && n >= sizeof(TOKEN_USER) + 28);
    TOKEN_USER *tu = token_info(tok, TokenUser);
    CHECK(tu && sid_is(tu->User.Sid, USER) && (BYTE *)tu->User.Sid > (BYTE *)tu && (BYTE *)tu->User.Sid < (BYTE *)tu + n && tu->User.Attributes == 0);
    free(tu);
    TOKEN_GROUPS *tg = token_info(tok, TokenGroups);
    int has_users = 0, has_admins = 0, has_logon = 0, has_label = 0;
    for (DWORD i = 0; tg && i < tg->GroupCount; i++) {
        PSID g = tg->Groups[i].Sid; DWORD at = tg->Groups[i].Attributes;
        if (EqualSid(g, users) && (at & SE_GROUP_ENABLED)) has_users = 1;
        if (EqualSid(g, admins)) has_admins = 1;
        if ((at & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID && *GetSidSubAuthorityCount(g) == 3 && *GetSidSubAuthority(g, 0) == 5) has_logon = 1;
        if ((at & SE_GROUP_INTEGRITY) && sid_is(g, L"S-1-16-8192")) has_label = 1;
    }
    CHECK(tg && tg->GroupCount == 12 && has_users && !has_admins && has_logon && has_label);
    free(tg);
    CHECK(integrity_of(tok) == SECURITY_MANDATORY_MEDIUM_RID);
    TOKEN_TYPE type; DWORD dw;
    CHECK(GetTokenInformation(tok, TokenType, &type, sizeof type, &n) && type == TokenPrimary && n == 4);
    CHECK(!GetTokenInformation(tok, TokenImpersonationLevel, &dw, sizeof dw, &n) && GetLastError() == ERROR_INVALID_PARAMETER);
    TOKEN_ELEVATION el;
    CHECK(!GetTokenInformation(tok, TokenElevation, &el, 2, &n) && GetLastError() == ERROR_BAD_LENGTH && n == 4);
    CHECK(GetTokenInformation(tok, TokenElevation, &el, sizeof el, &n) && !el.TokenIsElevated);
    TOKEN_PRIVILEGES *tp = token_info(tok, TokenPrivileges);
    int change_notify = 0;
    for (DWORD i = 0; tp && i < tp->PrivilegeCount; i++)
        if (tp->Privileges[i].Luid.LowPart == 23 && (tp->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED)) change_notify = 1;
    CHECK(tp && tp->PrivilegeCount == 5 && change_notify);
    free(tp);
    TOKEN_STATISTICS st;
    CHECK(GetTokenInformation(tok, TokenStatistics, &st, sizeof st, &n) && st.TokenType == TokenPrimary && st.GroupCount == 12 && st.PrivilegeCount == 5);
    TOKEN_OWNER *to = token_info(tok, TokenOwner); TOKEN_PRIMARY_GROUP *tpg = token_info(tok, TokenPrimaryGroup);
    CHECK(to && sid_is(to->Owner, USER) && tpg && sid_is(tpg->PrimaryGroup, L"S-1-5-21-1000-1000-1000-513"));
    free(to); free(tpg);
    TOKEN_DEFAULT_DACL *dd = token_info(tok, TokenDefaultDacl);
    CHECK(dd && dd->DefaultDacl && IsValidAcl(dd->DefaultDacl) && dd->DefaultDacl->AceCount == 3 && EqualSid(ace_sid(ace_at(dd->DefaultDacl, 0)), user) &&
          ace_mask(ace_at(dd->DefaultDacl, 0)) == GENERIC_ALL);
    free(dd);
    TOKEN_GROUPS *ls = token_info(tok, TokenLogonSid);
    CHECK(ls && ls->GroupCount == 1 && *GetSidSubAuthority(ls->Groups[0].Sid, 0) == 5);
    free(ls);
    TOKEN_GROUPS_AND_PRIVILEGES *gp = token_info(tok, TokenGroupsAndPrivileges);
    CHECK(gp && gp->SidCount == 13 && sid_is(gp->Sids[0].Sid, USER) && gp->RestrictedSidCount == 0 && gp->PrivilegeCount == 5 && gp->Privileges);
    free(gp);
    TOKEN_GROUPS *rs = token_info(tok, TokenRestrictedSids);
    CHECK(rs && rs->GroupCount == 0 && !IsTokenRestricted(tok));
    free(rs);
    TOKEN_USER *ptu = token_info(GetCurrentProcessToken(), TokenUser);
    CHECK(ptu && sid_is(ptu->User.Sid, USER));
    free(ptu);
    CHECK(!GetTokenInformation(GetCurrentThreadToken(), TokenType, &type, sizeof type, &n) && GetLastError() == ERROR_NO_TOKEN);
    CHECK(GetTokenInformation(GetCurrentThreadEffectiveToken(), TokenType, &type, sizeof type, &n) && type == TokenPrimary);
    CHECK(!GetTokenInformation((HANDLE)0x1234, TokenType, &type, sizeof type, &n) && GetLastError() == ERROR_INVALID_HANDLE);
    CHECK(!GetTokenInformation(tok, (TOKEN_INFORMATION_CLASS)999, &dw, sizeof dw, &n) && GetLastError() == ERROR_INVALID_PARAMETER);

    // duplicates: a token of their own, levels no higher than their source
    HANDLE imp = NULL, ident = NULL, x = NULL;
    CHECK(DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenImpersonation, &imp));
    SECURITY_IMPERSONATION_LEVEL level = 0;
    CHECK(GetTokenInformation(imp, TokenType, &type, sizeof type, &n) && type == TokenImpersonation);
    CHECK(GetTokenInformation(imp, TokenImpersonationLevel, &level, sizeof level, &n) && level == SecurityImpersonation);
    CHECK(!DuplicateTokenEx(imp, MAXIMUM_ALLOWED, NULL, SecurityDelegation, TokenImpersonation, &x) && GetLastError() == ERROR_BAD_IMPERSONATION_LEVEL);
    CHECK(DuplicateToken(imp, SecurityIdentification, &ident));
    CHECK(!DuplicateTokenEx(ident, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &x) && GetLastError() == ERROR_BAD_IMPERSONATION_LEVEL);
    CHECK(!DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, (TOKEN_TYPE)7, &x) && GetLastError() == ERROR_INVALID_PARAMETER);
    // a descriptor for the new token: kept (its generic rights mapped) when it leaves the user every right to it
    SECURITY_ATTRIBUTES sa = { sizeof sa, sddl(L"O:S-1-5-21-1000-1000-1000-1001D:(A;;GA;;;S-1-5-21-1000-1000-1000-1001)(A;;GR;;;SY)"), FALSE };
    HANDLE dup_sd = NULL;
    CHECK(DuplicateTokenEx(tok, MAXIMUM_ALLOWED, &sa, SecurityAnonymous, TokenPrimary, &dup_sd));
    PSID o = NULL; PSECURITY_DESCRIPTOR osd = NULL; PACL od = NULL;
    CHECK(GetSecurityInfo(dup_sd, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &o, NULL, &od, NULL, &osd) == ERROR_SUCCESS &&
          sid_is(o, USER) && od && od->AceCount == 2 && ace_mask(ace_at(od, 0)) == TOKEN_ALL_ACCESS);
    LocalFree(osd); LocalFree(sa.lpSecurityDescriptor); CloseHandle(dup_sd);
    // one that would keep the user out (nothing would hold anyone to it) and an owner the caller cannot give: refused
    sa.lpSecurityDescriptor = sddl(L"D:(A;;GA;;;SY)");
    CHECK(!DuplicateTokenEx(tok, MAXIMUM_ALLOWED, &sa, SecurityAnonymous, TokenPrimary, &x) && GetLastError() == ERROR_NOT_SUPPORTED);
    LocalFree(sa.lpSecurityDescriptor);
    sa.lpSecurityDescriptor = sddl(L"O:BAD:(A;;GA;;;WD)");
    CHECK(!DuplicateTokenEx(tok, MAXIMUM_ALLOWED, &sa, SecurityAnonymous, TokenPrimary, &x) && GetLastError() == ERROR_INVALID_OWNER);
    LocalFree(sa.lpSecurityDescriptor);

    // SetTokenInformation: integrity only lowered, owner and group only the token's own SIDs
    TOKEN_MANDATORY_LABEL label = { { low, SE_GROUP_INTEGRITY } };
    CHECK(SetTokenInformation(imp, TokenIntegrityLevel, &label, sizeof label) && integrity_of(imp) == SECURITY_MANDATORY_LOW_RID);
    CHECK(integrity_of(tok) == SECURITY_MANDATORY_MEDIUM_RID);
    label.Label.Sid = sid_of(L"ME");
    CHECK(!SetTokenInformation(imp, TokenIntegrityLevel, &label, sizeof label) && GetLastError() == ERROR_PRIVILEGE_NOT_HELD);
    LocalFree(label.Label.Sid);
    label.Label.Sid = world;
    CHECK(!SetTokenInformation(imp, TokenIntegrityLevel, &label, sizeof label) && GetLastError() == ERROR_INVALID_PARAMETER);
    TOKEN_OWNER new_owner = { world };
    CHECK(!SetTokenInformation(imp, TokenOwner, &new_owner, sizeof new_owner) && GetLastError() == ERROR_INVALID_OWNER);
    new_owner.Owner = user;
    CHECK(SetTokenInformation(imp, TokenOwner, &new_owner, sizeof new_owner));
    TOKEN_PRIMARY_GROUP new_group = { users };
    CHECK(SetTokenInformation(imp, TokenPrimaryGroup, &new_group, sizeof new_group));
    new_group.PrimaryGroup = admins;
    CHECK(!SetTokenInformation(imp, TokenPrimaryGroup, &new_group, sizeof new_group) && GetLastError() == ERROR_INVALID_PRIMARY_GROUP);
    TOKEN_DEFAULT_DACL new_dacl = { dacl };
    CHECK(InitializeAcl(dacl, sizeof daclbuf, ACL_REVISION) && AddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_ALL, world));
    CHECK(SetTokenInformation(imp, TokenDefaultDacl, &new_dacl, sizeof new_dacl));
    dd = token_info(imp, TokenDefaultDacl);
    CHECK(dd && dd->DefaultDacl && dd->DefaultDacl->AceCount == 1);
    free(dd);
    new_dacl.DefaultDacl = NULL;
    CHECK(SetTokenInformation(imp, TokenDefaultDacl, &new_dacl, sizeof new_dacl));
    dd = token_info(imp, TokenDefaultDacl);
    CHECK(dd && !dd->DefaultDacl);
    free(dd);
    dw = 3;
    CHECK(!SetTokenInformation(imp, TokenSessionId, &dw, sizeof dw) && GetLastError() == ERROR_PRIVILEGE_NOT_HELD);
    CHECK(!SetTokenInformation(imp, (TOKEN_INFORMATION_CLASS)999, &dw, sizeof dw) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(!SetTokenInformation(imp, TokenOwner, &new_owner, 4) && GetLastError() == ERROR_BAD_LENGTH);

    // restricted tokens: privileges dropped, Users deny-only, two restricting SIDs (then only those both lists have)
    HANDLE rt = NULL, rt2 = NULL, rimp = NULL;
    SID_AND_ATTRIBUTES disable[1] = { { users, 0 } }, restrict_to[2] = { { world, 0 }, { restricted_code, 0 } };
    CHECK(CreateRestrictedToken(tok, DISABLE_MAX_PRIVILEGE, 1, disable, 0, NULL, 2, restrict_to, &rt));
    tp = token_info(rt, TokenPrivileges);
    CHECK(tp && tp->PrivilegeCount == 1 && tp->Privileges[0].Luid.LowPart == 23);
    free(tp);
    tg = token_info(rt, TokenGroups);
    DWORD users_attr = 0;
    for (DWORD i = 0; tg && i < tg->GroupCount; i++) if (EqualSid(tg->Groups[i].Sid, users)) users_attr = tg->Groups[i].Attributes;
    CHECK(users_attr == (SE_GROUP_MANDATORY | SE_GROUP_USE_FOR_DENY_ONLY));
    free(tg);
    rs = token_info(rt, TokenRestrictedSids);
    CHECK(rs && rs->GroupCount == 2 && EqualSid(rs->Groups[0].Sid, world) && rs->Groups[0].Attributes == (SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED));
    free(rs);
    CHECK(IsTokenRestricted(rt) && GetTokenInformation(rt, TokenHasRestrictions, &dw, sizeof dw, &n) && dw == 1);
    CHECK(GetTokenInformation(rt, TokenType, &type, sizeof type, &n) && type == TokenPrimary);
    SID_AND_ATTRIBUTES restrict2[2] = { { world, 0 }, { users, 0 } };
    CHECK(CreateRestrictedToken(rt, 0, 0, NULL, 0, NULL, 2, restrict2, &rt2));
    rs = token_info(rt2, TokenRestrictedSids);
    CHECK(rs && rs->GroupCount == 1 && EqualSid(rs->Groups[0].Sid, world));
    free(rs);
    CHECK(!CreateRestrictedToken(tok, 0x10, 0, NULL, 0, NULL, 0, NULL, &x) && GetLastError() == ERROR_INVALID_PARAMETER);
    SID_AND_ATTRIBUTES bad_disable[1] = { { built, 0 } };
    CHECK(!CreateRestrictedToken(tok, 0, 1, bad_disable, 0, NULL, 0, NULL, &x) && GetLastError() == ERROR_INVALID_SID);
    CHECK(DuplicateTokenEx(rt, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenImpersonation, &rimp));
    // a restricted token made from a restricted one gives no more than it: no restricting SID in common leaves none,
    // and the token stays restricted (nothing passes the second check); WRITE_RESTRICTED does not make a fully
    // restricted token's reads unchecked, and stays only where both were write-restricted
    HANDLE rt3 = NULL, rimp3 = NULL, rt4 = NULL, rimp4 = NULL;
    SID_AND_ATTRIBUTES only_users[1] = { { users, 0 } };
    CHECK(CreateRestrictedToken(rt, 0, 0, NULL, 0, NULL, 1, only_users, &rt3) && IsTokenRestricted(rt3));
    rs = token_info(rt3, TokenRestrictedSids);
    CHECK(rs && rs->GroupCount == 0);
    free(rs);
    CHECK(GetTokenInformation(rt3, (TOKEN_INFORMATION_CLASS)40 /* TokenIsRestricted */, &dw, sizeof dw, &n) && dw == 1);
    CHECK(DuplicateTokenEx(rt3, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenImpersonation, &rimp3));
    CHECK(CreateRestrictedToken(rt, WRITE_RESTRICTED, 0, NULL, 0, NULL, 0, NULL, &rt4) && IsTokenRestricted(rt4) &&
          DuplicateTokenEx(rt4, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenImpersonation, &rimp4));

    // impersonation, per thread
    CHECK(!SetThreadToken(NULL, tok) && GetLastError() == ERROR_BAD_TOKEN_TYPE);
    CHECK(!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &x) && GetLastError() == ERROR_NO_TOKEN && !x);
    g_ready = CreateEventW(NULL, FALSE, FALSE, NULL); g_go = CreateEventW(NULL, FALSE, FALSE, NULL); g_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
    WaitForSingleObject(g_ready, INFINITE);
    CHECK(SetThreadToken(NULL, imp));
    HANDLE mine = NULL;
    CHECK(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &mine) && integrity_of(mine) == SECURITY_MANDATORY_LOW_RID);
    CHECK(integrity_of(GetCurrentThreadEffectiveToken()) == SECURITY_MANDATORY_LOW_RID && integrity_of(GetCurrentProcessToken()) == SECURITY_MANDATORY_MEDIUM_RID);
    CHECK(SetThreadToken(&th, imp));                      // the other thread's own token
    SetEvent(g_go);
    WaitForSingleObject(g_done, INFINITE);
    CHECK(!g_thread_token_before && g_thread_had_token);
    BOOL member = 2;
    CHECK(CheckTokenMembership(NULL, users, &member) && member);
    CHECK(RevertToSelf() && !OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &x) && GetLastError() == ERROR_NO_TOKEN);
    CHECK(ImpersonateSelf(SecurityIdentification) && OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &x) &&
          GetTokenInformation(x, TokenImpersonationLevel, &level, sizeof level, &n) && level == SecurityIdentification);
    CloseHandle(x); RevertToSelf();
    CHECK(ImpersonateLoggedOnUser(tok) && OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &x) &&
          GetTokenInformation(x, TokenImpersonationLevel, &level, sizeof level, &n) && level == SecurityImpersonation);
    CloseHandle(x); RevertToSelf();
    // NtSetInformationThread(ThreadImpersonationToken) is SetThreadToken by another name: the token is in force
    // afterwards (OpenThreadToken and the effective token see it), a primary token is refused and changes nothing, NULL
    // reverts, and the data is exactly one handle. ThreadHideFromDebugger takes no data. A class the runtime does not
    // carry out is refused (STATUS_NOT_SUPPORTED), never reported done. No Nt call touches the last error.
    nt_set_thread = (nt_set_thread_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationThread");
    CHECK(nt_set_thread != NULL);
    if (nt_set_thread) {
        HANDLE none = NULL, bogus = (HANDLE)(intptr_t)0x1234;
        LONG priority = THREAD_PRIORITY_LOWEST;
        DWORD one = 1;
        SetLastError(4321);
        CHECK(nt_set_thread(GetCurrentThread(), 5 /* ThreadImpersonationToken */, &imp, sizeof imp) == 0 && GetLastError() == 4321);
        CHECK(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &x) && integrity_of(x) == SECURITY_MANDATORY_LOW_RID);
        CloseHandle(x);
        CHECK(integrity_of(GetCurrentThreadEffectiveToken()) == SECURITY_MANDATORY_LOW_RID);
        SetLastError(4321);
        CHECK(nt_set_thread(GetCurrentThread(), 5, &tok, sizeof tok) == (LONG)0xC00000A8 && GetLastError() == 4321);   // STATUS_BAD_TOKEN_TYPE
        CHECK(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &x) && integrity_of(x) == SECURITY_MANDATORY_LOW_RID);
        CloseHandle(x);
        CHECK(nt_set_thread(GetCurrentThread(), 5, &none, sizeof none) == 0);
        CHECK(!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &x) && GetLastError() == ERROR_NO_TOKEN);
        SetLastError(4321);
        CHECK(nt_set_thread(GetCurrentThread(), 5, &imp, sizeof(DWORD)) == (LONG)0xC0000004);   // STATUS_INFO_LENGTH_MISMATCH
        CHECK(nt_set_thread(GetCurrentThread(), 5, NULL, sizeof imp) == (LONG)0xC0000005);      // STATUS_ACCESS_VIOLATION
        CHECK(nt_set_thread(bogus, 5, &imp, sizeof imp) == (LONG)0xC0000008 && GetLastError() == 4321);   // STATUS_INVALID_HANDLE
        CHECK(!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &x) && GetLastError() == ERROR_NO_TOKEN);   // none applied
        CHECK(nt_set_thread(GetCurrentThread(), 0x11 /* ThreadHideFromDebugger */, NULL, 0) == 0);
        CHECK(nt_set_thread(GetCurrentThread(), 0x11, &one, sizeof one) == (LONG)0xC0000004);
        CHECK(nt_set_thread(GetCurrentThread(), 2 /* ThreadPriority */, &priority, sizeof priority) == (LONG)0xC00000BB);
        CHECK(nt_set_thread(GetCurrentThread(), 1000, &priority, sizeof priority) == (LONG)0xC00000BB);
    }
    // the anonymous logon is not in Everyone unless "Let Everyone permissions apply to anonymous users" is on (off by
    // default; the test's own registry, the value removed again)
    HKEY lsa = NULL;
    CHECK(RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Lsa", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &lsa, NULL) == ERROR_SUCCESS);
    RegDeleteValueW(lsa, L"EveryoneIncludesAnonymous");
    PSID anonymous = sid_of(L"AN"), network = sid_of(L"NU");
    CHECK(ImpersonateAnonymousToken(GetCurrentThread()));
    tu = token_info(GetCurrentThreadToken(), TokenUser);
    CHECK(tu && sid_is(tu->User.Sid, L"S-1-5-7"));
    member = 2; CHECK(CheckTokenMembership(NULL, anonymous, &member) && member);
    member = 2; CHECK(CheckTokenMembership(NULL, network, &member) && member);
    member = 2; CHECK(CheckTokenMembership(NULL, world, &member) && !member);
    member = 2; CHECK(CheckTokenMembership(NULL, users, &member) && !member);
    free(tu); RevertToSelf();
    DWORD policy_on = 1, policy_off = 0;
    CHECK(RegSetValueExW(lsa, L"EveryoneIncludesAnonymous", 0, REG_DWORD, (BYTE *)&policy_on, sizeof policy_on) == ERROR_SUCCESS);
    CHECK(ImpersonateAnonymousToken(GetCurrentThread()));
    member = 2; CHECK(CheckTokenMembership(NULL, world, &member) && member);
    RevertToSelf();
    CHECK(RegSetValueExW(lsa, L"EveryoneIncludesAnonymous", 0, REG_DWORD, (BYTE *)&policy_off, sizeof policy_off) == ERROR_SUCCESS);
    CHECK(ImpersonateAnonymousToken(GetCurrentThread()));
    member = 2; CHECK(CheckTokenMembership(NULL, world, &member) && !member);
    RevertToSelf();
    RegDeleteValueW(lsa, L"EveryoneIncludesAnonymous");
    RegCloseKey(lsa);
    LocalFree(anonymous); LocalFree(network);
    WaitForSingleObject(th, INFINITE); CloseHandle(th); CloseHandle(mine);

    // membership: enabled groups only, and on a restricted token its restricting SIDs too
    CHECK(CheckTokenMembership(NULL, users, &member) && member);
    CHECK(CheckTokenMembership(NULL, admins, &member) && !member);
    CHECK(CheckTokenMembership(NULL, world, &member) && member);
    CHECK(!CheckTokenMembership(tok, users, &member) && GetLastError() == ERROR_NO_IMPERSONATION_TOKEN);
    CHECK(CheckTokenMembership(rimp, world, &member) && member);
    CHECK(CheckTokenMembership(rimp, users, &member) && !member);
    CHECK(CheckTokenMembership(rimp, auth_users, &member) && !member);
    CHECK(CheckTokenMembership(rimp, restricted_code, &member) && !member);
    CHECK(CheckTokenMembership(rimp3, world, &member) && !member);    // (rt's restricting SIDs and the new ones: none in common)

    // ---- privileges ----
    LUID luid;
    CHECK(LookupPrivilegeValueW(NULL, L"SeChangeNotifyPrivilege", &luid) && luid.LowPart == 23 && luid.HighPart == 0);
    CHECK(LookupPrivilegeValueA(NULL, "sedebugprivilege", &luid) && luid.LowPart == 20 && luid.HighPart == 0);
    CHECK(LookupPrivilegeValueW(NULL, L"SeSecurityPrivilege", &luid) && luid.LowPart == 8);
    CHECK(!LookupPrivilegeValueW(NULL, L"SeNoSuchPrivilege", &luid) && GetLastError() == ERROR_NO_SUCH_PRIVILEGE);
    WCHAR pname[64]; char aname[64]; DWORD pnl = 64, anl = 64;
    luid.LowPart = 19; luid.HighPart = 0;
    CHECK(LookupPrivilegeNameW(NULL, &luid, pname, &pnl) && !wcscmp(pname, L"SeShutdownPrivilege") && pnl == 19);
    pnl = 4;
    CHECK(!LookupPrivilegeNameW(NULL, &luid, pname, &pnl) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && pnl == 20);
    luid.LowPart = 23;
    CHECK(LookupPrivilegeNameA(NULL, &luid, aname, &anl) && !strcmp(aname, "SeChangeNotifyPrivilege"));
    luid.LowPart = 99; pnl = 64;
    CHECK(!LookupPrivilegeNameW(NULL, &luid, pname, &pnl) && GetLastError() == ERROR_NO_SUCH_PRIVILEGE);
    // AdjustTokenPrivileges changes the token's own (here a duplicate's), and says when it lacks one
    HANDLE adj = NULL;
    CHECK(DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenImpersonation, &adj));
    struct { DWORD count; LUID_AND_ATTRIBUTES p[2]; } turn_on = { 2, { { { 19, 0 }, SE_PRIVILEGE_ENABLED }, { { 33, 0 }, SE_PRIVILEGE_ENABLED } } };
    struct { DWORD count; LUID_AND_ATTRIBUTES p[5]; } before; DWORD blen = 0;
    CHECK(!AdjustTokenPrivileges(adj, FALSE, (TOKEN_PRIVILEGES *)&turn_on, 8, (TOKEN_PRIVILEGES *)&before, &blen) &&
          GetLastError() == ERROR_INSUFFICIENT_BUFFER && blen == 4 + 2 * 12 && !privilege_on(adj, 19));   // (too small: nothing changed)
    SetLastError(0xdead);
    CHECK(AdjustTokenPrivileges(adj, FALSE, (TOKEN_PRIVILEGES *)&turn_on, sizeof before, (TOKEN_PRIVILEGES *)&before, &blen) && GetLastError() == ERROR_SUCCESS);
    CHECK(before.count == 2 && before.p[0].Luid.LowPart == 19 && before.p[0].Attributes == 0 && before.p[1].Luid.LowPart == 33);
    CHECK(privilege_on(adj, 19) && privilege_on(adj, 33) && !privilege_on(tok, 19));
    TOKEN_PRIVILEGES debug = { 1, { { { 20, 0 }, SE_PRIVILEGE_ENABLED } } }, backup = { 1, { { { 17, 0 }, SE_PRIVILEGE_ENABLED } } };
    CHECK(AdjustTokenPrivileges(adj, FALSE, &debug, 0, NULL, NULL) && GetLastError() == ERROR_NOT_ALL_ASSIGNED && privilege_state(adj, 20) == -1);
    CHECK(AdjustTokenPrivileges(adj, FALSE, &backup, 0, NULL, NULL) && GetLastError() == ERROR_NOT_ALL_ASSIGNED && privilege_state(adj, 17) == -1);
    CHECK(AdjustTokenPrivileges(adj, TRUE, NULL, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS && !privilege_on(adj, 19) && !privilege_on(adj, 23));
    CHECK(!AdjustTokenPrivileges(adj, FALSE, NULL, 0, NULL, NULL) && GetLastError() == ERROR_INVALID_PARAMETER);
    TOKEN_PRIVILEGES gone = { 1, { { { 25, 0 }, SE_PRIVILEGE_REMOVED } } };
    CHECK(AdjustTokenPrivileges(adj, FALSE, &gone, 0, NULL, NULL) && privilege_count(adj) == 4 && privilege_state(adj, 25) == -1);
    gone.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    CHECK(AdjustTokenPrivileges(adj, FALSE, &gone, 0, NULL, NULL) && GetLastError() == ERROR_NOT_ALL_ASSIGNED);   // (removed for good)
    CHECK(privilege_count(tok) == 5 && privilege_on(tok, 23));
    // PrivilegeCheck: all of a set, or any one, enabled in a client's token; the enabled ones marked used
    TOKEN_PRIVILEGES notify = { 1, { { { 23, 0 }, SE_PRIVILEGE_ENABLED } } };
    CHECK(AdjustTokenPrivileges(adj, FALSE, &notify, 0, NULL, NULL));
    struct { DWORD count, control; LUID_AND_ATTRIBUTES p[2]; } wanted = { 2, PRIVILEGE_SET_ALL_NECESSARY, { { { 23, 0 }, 0 }, { { 19, 0 }, 0 } } };
    BOOL has = 2;
    CHECK(PrivilegeCheck(adj, (PPRIVILEGE_SET)&wanted, &has) && !has && wanted.p[0].Attributes == SE_PRIVILEGE_USED_FOR_ACCESS && wanted.p[1].Attributes == 0);
    wanted.control = 0;
    CHECK(PrivilegeCheck(adj, (PPRIVILEGE_SET)&wanted, &has) && has);
    CHECK(!PrivilegeCheck(tok, (PPRIVILEGE_SET)&wanted, &has) && GetLastError() == ERROR_NO_IMPERSONATION_TOKEN);
    CloseHandle(adj);

    // ---- AccessCheck ----
    HANDLE med = NULL;
    CHECK(DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenImpersonation, &med));
    const WCHAR *obj = L"O:S-1-5-21-1000-1000-1000-1001G:S-1-5-21-1000-1000-1000-513D:(D;;0x2;;;BU)(A;;0x7;;;BU)(A;;0x8;;;S-1-5-21-1000-1000-1000-1001)";
    DWORD err;
    CHECK(check(med, obj, 0x1, NULL) == 0x1);
    CHECK(check(med, obj, 0x2, &err) == 0 && err == ERROR_ACCESS_DENIED);
    CHECK(check(med, obj, 0x8, NULL) == 0x8 && check(med, obj, 0xd, NULL) == 0xd);
    CHECK(check(med, obj, READ_CONTROL | WRITE_DAC, NULL) == (READ_CONTROL | WRITE_DAC));        // the owner's
    CHECK(check(med, obj, WRITE_OWNER, NULL) == 0);
    CHECK(check(med, obj, MAXIMUM_ALLOWED, NULL) == (0xd | READ_CONTROL | WRITE_DAC));
    CHECK(check(med, obj, MAXIMUM_ALLOWED | 0x2, NULL) == 0);
    CHECK(check(med, obj, 0, NULL) == 0);
    CHECK(check(med, obj, GENERIC_READ, &err) == 0xffffffff && err == ERROR_GENERIC_NOT_MAPPED);
    CHECK(check(tok, obj, 0x1, &err) == 0xffffffff && err == ERROR_NO_IMPERSONATION_TOKEN);
    CHECK(check(med, obj, ACCESS_SYSTEM_SECURITY, &err) == 0 && err == ERROR_PRIVILEGE_NOT_HELD);
    CHECK(check(med, L"D:(A;;0x7;;;WD)", 0x1, &err) == 0xffffffff && err == ERROR_INVALID_SECURITY_DESCR);   // no owner and group
    const WCHAR *null_dacl = L"O:BAG:BAD:NO_ACCESS_CONTROL", *empty = L"O:BAG:BAD:", *empty_mine = L"O:S-1-5-21-1000-1000-1000-1001G:BAD:";
    CHECK(check(med, null_dacl, 0x7, NULL) == 0x7 && check(med, null_dacl, MAXIMUM_ALLOWED, NULL) == map.GenericAll);
    CHECK(check(med, empty, 0x1, NULL) == 0 && check(med, empty_mine, 0x1, NULL) == 0 && check(med, empty_mine, READ_CONTROL, NULL) == READ_CONTROL);
    CHECK(check(med, L"O:BAG:BAD:(A;IO;0x1;;;WD)", 0x1, NULL) == 0);
    CHECK(check(med, L"O:S-1-5-21-1000-1000-1000-1001G:BAD:(A;;0x1;;;OW)", READ_CONTROL, NULL) == 0);
    CHECK(check(med, L"O:S-1-5-21-1000-1000-1000-1001G:BAD:(A;;0x1;;;OW)", 0x1, NULL) == 0x1);
    // the integrity label: a low token reads but does not write a medium (unlabelled) object
    CHECK(check(imp, L"O:BAG:BAD:(A;;0x7;;;WD)", 0x1, NULL) == 0x1);
    CHECK(check(imp, L"O:BAG:BAD:(A;;0x7;;;WD)", 0x2, NULL) == 0);
    CHECK(check(imp, L"O:BAG:BAD:(A;;0x7;;;WD)S:(ML;;NW;;;LW)", 0x2, NULL) == 0x2);
    CHECK(check(imp, L"O:BAG:BAD:(A;;0x7;;;WD)S:(ML;;NWNR;;;ME)", 0x1, NULL) == 0);
    CHECK(check(imp, L"O:BAG:BAD:(A;;0x7;;;WD)", MAXIMUM_ALLOWED, NULL) == 0x5);
    // a restricted token: what both its SIDs and its restricting SIDs are granted
    CHECK(check(rimp, L"O:BAG:BAD:(A;;0x7;;;AU)(A;;0x1;;;WD)", 0x1, NULL) == 0x1);
    CHECK(check(rimp, L"O:BAG:BAD:(A;;0x7;;;AU)(A;;0x1;;;WD)", 0x2, NULL) == 0);
    CHECK(check(rimp, L"O:BAG:BAD:(A;;0x7;;;AU)(A;;0x1;;;WD)", MAXIMUM_ALLOWED, NULL) == 0x1);
    CHECK(check(rimp, L"O:BAG:BAD:(A;;0x7;;;BU)", 0x1, NULL) == 0);   // (Users: deny-only)
    HANDLE wr = NULL, wimp = NULL;
    SID_AND_ATTRIBUTES wr_sids[1] = { { write_restricted, 0 } };
    CHECK(CreateRestrictedToken(tok, WRITE_RESTRICTED, 0, NULL, 0, NULL, 1, wr_sids, &wr) &&
          DuplicateTokenEx(wr, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenImpersonation, &wimp));
    CHECK(check(wimp, L"O:BAG:BAD:(A;;0x7;;;AU)", 0x5, NULL) == 0x5);   // reading is not checked twice
    CHECK(check(wimp, L"O:BAG:BAD:(A;;0x7;;;AU)", 0x2, NULL) == 0);
    CHECK(check(wimp, L"O:BAG:BAD:(A;;0x7;;;AU)(A;;0x2;;;WR)", 0x2, NULL) == 0x2);
    CHECK(check(rimp, L"O:BAG:BAD:(A;;0x7;;;WD)", 0x1, NULL) == 0x1);
    CHECK(check(rimp3, L"O:BAG:BAD:(A;;0x7;;;WD)", 0x1, NULL) == 0);   // restricted with no SIDs left: nothing passes
    CHECK(check(rimp3, L"O:BAG:BAD:(A;;0x7;;;WD)", MAXIMUM_ALLOWED, NULL) == 0);
    CHECK(check(rimp4, L"O:BAG:BAD:(A;;0x7;;;AU)", 0x1, NULL) == 0);   // reads still checked against WD and RC
    CHECK(check(rimp4, L"O:BAG:BAD:(A;;0x7;;;AU)(A;;0x1;;;WD)", 0x1, NULL) == 0x1);
    HANDLE wr2 = NULL, wimp2 = NULL;
    CHECK(CreateRestrictedToken(wr, WRITE_RESTRICTED, 0, NULL, 0, NULL, 0, NULL, &wr2) &&
          DuplicateTokenEx(wr2, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenImpersonation, &wimp2));
    CHECK(check(wimp2, L"O:BAG:BAD:(A;;0x7;;;AU)", 0x5, NULL) == 0x5 && check(wimp2, L"O:BAG:BAD:(A;;0x7;;;AU)", 0x2, NULL) == 0);
    BYTE ps[8]; DWORD pslen = sizeof ps, granted; BOOL status;
    PSECURITY_DESCRIPTOR osd2 = sddl(obj);
    CHECK(!AccessCheck(osd2, med, 0x1, &map, (PPRIVILEGE_SET)ps, &pslen, &granted, &status) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && pslen == sizeof(PRIVILEGE_SET));
    LocalFree(osd2);

    // ---- the security of objects ----
    PACL got = NULL; PSID go = NULL;
    CHECK(GetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &go, NULL, &got, NULL, &osd) == ERROR_SUCCESS);
    CHECK(go && sid_is(go, USER) && got && got->AceCount == 1 && ace_mask(ace_at(got, 0)) == 0x1fffff && EqualSid(ace_sid(ace_at(got, 0)), user));
    CHECK(GetSecurityDescriptorControl(osd, &control, &rev) && (control & SE_SELF_RELATIVE) && (control & SE_DACL_PRESENT) && IsValidSecurityDescriptor(osd));
    LocalFree(osd);
    CHECK(GetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL, &osd) == ERROR_PRIVILEGE_NOT_HELD);
    CHECK(GetSecurityInfo(NULL, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &got, NULL, &osd) == ERROR_INVALID_HANDLE);
    // the process: nothing checks its descriptor when another of the user's processes opens it, so one that would
    // keep the user from some of its rights is refused; one leaving the user everything is kept
    PSID system = sid_of(L"SY");
    BYTE alldaclbuf[64], sysdaclbuf[64], minedaclbuf[64]; PACL alldacl = (PACL)alldaclbuf, sysdacl = (PACL)sysdaclbuf, minedacl = (PACL)minedaclbuf;
    CHECK(InitializeAcl(alldacl, sizeof alldaclbuf, ACL_REVISION) && AddAccessAllowedAce(alldacl, ACL_REVISION, GENERIC_ALL, world));
    CHECK(InitializeAcl(sysdacl, sizeof sysdaclbuf, ACL_REVISION) && AddAccessAllowedAce(sysdacl, ACL_REVISION, GENERIC_ALL, system));
    CHECK(InitializeAcl(minedacl, sizeof minedaclbuf, ACL_REVISION) && AddAccessAllowedAce(minedacl, ACL_REVISION, GENERIC_ALL, user));
    CHECK(InitializeAcl(dacl, sizeof daclbuf, ACL_REVISION) && AddAccessAllowedAce(dacl, ACL_REVISION, 0x1f0003, world));
    CHECK(SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL, dacl, NULL) == ERROR_NOT_SUPPORTED);
    CHECK(SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, sysdacl, NULL) == ERROR_NOT_SUPPORTED);
    CHECK(SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL, alldacl, NULL) == ERROR_SUCCESS);
    CHECK(GetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &got, NULL, &osd) == ERROR_SUCCESS);
    CHECK(got && got->AceCount == 1 && ace_mask(ace_at(got, 0)) == 0x1fffff && EqualSid(ace_sid(ace_at(got, 0)), world) &&
          GetSecurityDescriptorControl(osd, &control, &rev) && (control & SE_DACL_PROTECTED));   // (GENERIC_ALL mapped: PROCESS_ALL_ACCESS)
    LocalFree(osd);
    CHECK(SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, world, NULL, NULL, NULL) == ERROR_INVALID_OWNER);
    CHECK(SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, sacl) == ERROR_PRIVILEGE_NOT_HELD);
    CHECK(SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, (PACL)badacl, NULL) == ERROR_INVALID_ACL);
    // a token keeps its label (a low one takes nothing from the user)
    CHECK(SetSecurityInfo(imp, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION, NULL, NULL, NULL, sacl) == ERROR_SUCCESS);
    PACL got_sacl = NULL;
    CHECK(GetSecurityInfo(imp, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION, NULL, NULL, NULL, &got_sacl, &osd) == ERROR_SUCCESS &&
          got_sacl && got_sacl->AceCount == 1 && ace_at(got_sacl, 0)->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE && EqualSid(ace_sid(ace_at(got_sacl, 0)), low));
    LocalFree(osd);
    BYTE highbuf[28]; PACL high_sacl = (PACL)highbuf; PSID high = sid_of(L"HI");
    CHECK(InitializeAcl(high_sacl, 28, ACL_REVISION) && AddMandatoryAce(high_sacl, ACL_REVISION, 0, 1, high));
    CHECK(SetSecurityInfo(imp, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION, NULL, NULL, NULL, high_sacl) == ERROR_INVALID_LABEL);
    // an event: EVENT_ALL_ACCESS is all there is to it; the handle has nowhere to keep a descriptor, so what the Mac
    // gives is what is read back
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, L"Local\\vxsec_event");
    CHECK(SetSecurityInfo(ev, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, dacl, NULL) == ERROR_SUCCESS);
    CHECK(SetSecurityInfo(ev, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, sysdacl, NULL) == ERROR_NOT_SUPPORTED);
    CHECK(GetSecurityInfo(ev, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &got, NULL, &osd) == ERROR_SUCCESS && got &&
          EqualSid(ace_sid(ace_at(got, 0)), user) && ace_mask(ace_at(got, 0)) == EVENT_ALL_ACCESS);
    LocalFree(osd);
    CHECK(GetNamedSecurityInfoW(L"Local\\vxsec_event", SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &go, NULL, NULL, NULL, &osd) == ERROR_SUCCESS && sid_is(go, USER));
    LocalFree(osd);
    CHECK(SetNamedSecurityInfoW(L"Local\\vxsec_event", SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, sysdacl, NULL) == ERROR_NOT_SUPPORTED);
    CHECK(SetNamedSecurityInfoW(L"Local\\vxsec_event", SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, alldacl, NULL) == ERROR_SUCCESS);
    CHECK(GetNamedSecurityInfoW(L"Local\\vxsec_event", SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &got, NULL, &osd) == ERROR_SUCCESS && got &&
          got->AceCount == 1 && EqualSid(ace_sid(ace_at(got, 0)), world) && ace_mask(ace_at(got, 0)) == EVENT_ALL_ACCESS);
    LocalFree(osd);
    CHECK(GetNamedSecurityInfoW(L"Local\\vxsec_none", SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &go, NULL, NULL, NULL, &osd) == ERROR_FILE_NOT_FOUND);
    CloseHandle(ev);
    // files: what the Mac's owner and mode bits say, read again each time, by name or by handle alike
    CreateDirectoryW(L"C:\\vxsec", NULL);
    HANDLE f = CreateFileW(L"C:\\vxsec\\a.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE);
    CHECK(GetSecurityInfo(f, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &go, NULL, NULL, NULL, &osd) == ERROR_SUCCESS && sid_is(go, USER));
    LocalFree(osd);
    BYTE fsd[256]; DWORD fneed = 0;
    CHECK(GetKernelObjectSecurity(f, OWNER_SECURITY_INFORMATION, fsd, sizeof fsd, &fneed) && GetSecurityDescriptorOwner(fsd, &got_sid, &defaulted) && EqualSid(got_sid, user));
    CHECK(GetNamedSecurityInfoW(L"C:\\vxsec\\a.txt", SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &go, NULL, &got, NULL, &osd) == ERROR_SUCCESS);
    DWORD mine_rights = sid_gets(got, user);              // a new file: the owner reads and writes it, does not run it, may chmod it
    CHECK(sid_is(go, USER) && (mine_rights & (FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA | WRITE_DAC)) == (FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA | WRITE_DAC) &&
          !(mine_rights & FILE_EXECUTE) && !(mine_rights & WRITE_OWNER));
    LocalFree(osd);
    CHECK(GetNamedSecurityInfoW(L"C:\\vxsec\\missing.txt", SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &got, NULL, &osd) == ERROR_FILE_NOT_FOUND);
    CHECK(GetNamedSecurityInfoW(L"C:\\vxsec_nowhere\\a.txt", SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &got, NULL, &osd) == ERROR_PATH_NOT_FOUND);
    // a DACL mode bits cannot carry (writing but not appending): refused, nothing changes
    CHECK(SetNamedSecurityInfoW(L"C:\\vxsec\\a.txt", SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, dacl, NULL) == ERROR_NOT_SUPPORTED);
    // only the user: carried out (the group's and everyone else's bits taken away), seen by name and by handle alike
    CHECK(SetSecurityInfo(f, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL, minedacl, NULL) == ERROR_SUCCESS);
    CHECK(GetNamedSecurityInfoW(L"c:\\VXSEC\\A.TXT", SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &got, NULL, &osd) == ERROR_SUCCESS && got &&
          !(others_get(got, user) & (FILE_READ_DATA | FILE_WRITE_DATA)) && (sid_gets(got, user) & FILE_WRITE_DATA));
    LocalFree(osd);
    CHECK(GetSecurityInfo(f, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &got, NULL, &osd) == ERROR_SUCCESS && got && !(others_get(got, user) & FILE_READ_DATA));
    LocalFree(osd);
    // what would give more (everyone everything) is let pass and not carried out; the owner only as it is
    CHECK(SetNamedSecurityInfoW(L"C:\\vxsec\\a.txt", SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, alldacl, NULL) == ERROR_SUCCESS);
    CHECK(GetNamedSecurityInfoW(L"C:\\vxsec\\a.txt", SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &got, NULL, &osd) == ERROR_SUCCESS && got && !(others_get(got, user) & FILE_READ_DATA));
    LocalFree(osd);
    CHECK(SetNamedSecurityInfoW(L"C:\\vxsec\\a.txt", SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, user, NULL, NULL, NULL) == ERROR_SUCCESS);
    CHECK(SetNamedSecurityInfoW(L"C:\\vxsec\\a.txt", SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, world, NULL, NULL, NULL) == ERROR_INVALID_OWNER);
    CHECK(GetNamedSecurityInfoA("C:\\vxsec\\a.txt", SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &go, NULL, NULL, NULL, &osd) == ERROR_SUCCESS && sid_is(go, USER));
    LocalFree(osd);
    CHECK(!GetFileSecurityW(L"C:\\vxsec\\a.txt", DACL_SECURITY_INFORMATION, NULL, 0, &fneed) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && fneed > 20);
    CHECK(fneed <= sizeof fsd && GetFileSecurityW(L"C:\\vxsec\\a.txt", DACL_SECURITY_INFORMATION, fsd, sizeof fsd, &fneed) && IsValidSecurityDescriptor(fsd));
    CloseHandle(f);
    DeleteFileW(L"C:\\vxsec\\a.txt"); RemoveDirectoryW(L"C:\\vxsec");
    // a folder the user does not own (the Mac's root folder, root's: SYSTEM here): read as it is; taking rights from
    // others there needs its owner (refused, nothing changes), what takes nothing is let pass
    if (GetNamedSecurityInfoW(L"Z:\\", SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &go, NULL, &got, NULL, &osd) == ERROR_SUCCESS && sid_is(go, L"S-1-5-18")) {
        CHECK(sid_gets(got, system) & FILE_LIST_DIRECTORY);
        LocalFree(osd);
        CHECK(SetNamedSecurityInfoW(L"Z:\\", SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, minedacl, NULL) == ERROR_ACCESS_DENIED);
        CHECK(SetNamedSecurityInfoW(L"Z:\\", SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);   // (a NULL DACL)
    } else CHECK(!"the root folder reads as root's");
    // registry keys: one that would keep the user out is refused (nothing checks a key's descriptor when it opens)
    HKEY key;
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\VXSecTest", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL) == ERROR_SUCCESS);
    RegCloseKey(key);
    CHECK(GetNamedSecurityInfoW(L"CURRENT_USER\\Software\\VXSecTest", SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, &got, NULL, &osd) == ERROR_SUCCESS &&
          got && ace_mask(ace_at(got, 0)) == KEY_ALL_ACCESS && EqualSid(ace_sid(ace_at(got, 0)), user));
    LocalFree(osd);
    CHECK(SetNamedSecurityInfoW(L"CURRENT_USER\\Software\\VXSecTest", SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, sysdacl, NULL) == ERROR_NOT_SUPPORTED);
    CHECK(SetNamedSecurityInfoW(L"CURRENT_USER\\Software\\VXSecTest", SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, alldacl, NULL) == ERROR_SUCCESS);
    CHECK(GetNamedSecurityInfoW(L"CURRENT_USER\\Software\\VXSecNone", SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, &got, NULL, &osd) == ERROR_FILE_NOT_FOUND);
    CHECK(GetNamedSecurityInfoW(L"NOWHERE\\Software", SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, &got, NULL, &osd) == ERROR_INVALID_PARAMETER);
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\VXSecTest");
    CHECK(!GetKernelObjectSecurity(GetCurrentProcess(), DACL_SECURITY_INFORMATION, NULL, 0, &fneed) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    CHECK(GetKernelObjectSecurity(GetCurrentProcess(), DACL_SECURITY_INFORMATION, fsd, sizeof fsd, &fneed) && IsValidSecurityDescriptor(fsd));
    PSECURITY_DESCRIPTOR ksd = sddl(L"D:P(A;;GA;;;BU)");
    CHECK(SetKernelObjectSecurity(GetCurrentProcess(), DACL_SECURITY_INFORMATION, ksd));
    CHECK(GetKernelObjectSecurity(GetCurrentProcess(), DACL_SECURITY_INFORMATION, fsd, sizeof fsd, &fneed) && GetSecurityDescriptorControl(fsd, &control, &rev) &&
          (control & SE_DACL_PROTECTED) && GetSecurityDescriptorDacl(fsd, &present, &got_acl, &defaulted) && present && EqualSid(ace_sid(ace_at(got_acl, 0)), users));
    LocalFree(ksd);
    ksd = sddl(L"D:P(A;;0x1f0003;;;BU)");
    CHECK(!SetKernelObjectSecurity(GetCurrentProcess(), DACL_SECURITY_INFORMATION, ksd) && GetLastError() == ERROR_NOT_SUPPORTED);
    LocalFree(ksd);
    CHECK(!SetKernelObjectSecurity(GetCurrentProcess(), DACL_SECURITY_INFORMATION, badsd) && GetLastError() == ERROR_INVALID_SECURITY_DESCR);

    // ---- trustees and explicit access ----
    TRUSTEE_W tr;
    BuildTrusteeWithSidW(&tr, world);
    CHECK(!tr.pMultipleTrustee && tr.MultipleTrusteeOperation == NO_MULTIPLE_TRUSTEE && tr.TrusteeForm == TRUSTEE_IS_SID &&
          tr.TrusteeType == TRUSTEE_IS_UNKNOWN && tr.ptstrName == (LPWSTR)world && GetTrusteeFormW(&tr) == TRUSTEE_IS_SID);
    EXPLICIT_ACCESS_W ea[3];
    memset(ea, 0, sizeof ea);
    ea[0].grfAccessPermissions = 0x1; ea[0].grfAccessMode = GRANT_ACCESS; BuildTrusteeWithSidW(&ea[0].Trustee, world);
    ea[1].grfAccessPermissions = 0x2; ea[1].grfAccessMode = DENY_ACCESS; BuildTrusteeWithSidW(&ea[1].Trustee, users);
    BuildExplicitAccessWithNameW(&ea[2], L"CURRENT_USER", 0x4, GRANT_ACCESS, NO_INHERITANCE);
    CHECK(ea[2].Trustee.TrusteeForm == TRUSTEE_IS_NAME && !wcscmp(ea[2].Trustee.ptstrName, L"CURRENT_USER"));
    BYTE oldbuf[128]; PACL old = (PACL)oldbuf;
    CHECK(InitializeAcl(old, sizeof oldbuf, ACL_REVISION) && AddAccessAllowedAce(old, ACL_REVISION, 0x10, world) &&
          AddAccessAllowedAceEx(old, ACL_REVISION, INHERITED_ACE, 0x20, users));
    PACL merged = NULL;
    CHECK(SetEntriesInAclW(3, ea, old, &merged) == ERROR_SUCCESS && merged && IsValidAcl(merged) && merged->AceCount == 4);
    if (merged && merged->AceCount == 4) {
        CHECK(ace_at(merged, 0)->AceType == ACCESS_DENIED_ACE_TYPE && ace_mask(ace_at(merged, 0)) == 0x2 && EqualSid(ace_sid(ace_at(merged, 0)), users));
        CHECK(ace_at(merged, 1)->AceType == ACCESS_ALLOWED_ACE_TYPE && ace_mask(ace_at(merged, 1)) == 0x11 && EqualSid(ace_sid(ace_at(merged, 1)), world));
        CHECK(ace_mask(ace_at(merged, 2)) == 0x4 && EqualSid(ace_sid(ace_at(merged, 2)), user));
        CHECK(ace_at(merged, 3)->AceFlags == INHERITED_ACE && ace_mask(ace_at(merged, 3)) == 0x20);
    }
    EXPLICIT_ACCESS_W set = ea[0];
    set.grfAccessMode = SET_ACCESS; set.grfAccessPermissions = 0x40;
    PACL replaced = NULL;
    CHECK(SetEntriesInAclW(1, &set, merged, &replaced) == ERROR_SUCCESS && replaced && replaced->AceCount == 4 && ace_mask(ace_at(replaced, 2)) == 0x40 &&
          EqualSid(ace_sid(ace_at(replaced, 2)), world));
    EXPLICIT_ACCESS_W revoke = ea[1];
    revoke.grfAccessMode = REVOKE_ACCESS;
    PACL revoked = NULL;
    CHECK(SetEntriesInAclW(1, &revoke, replaced, &revoked) == ERROR_SUCCESS && revoked && revoked->AceCount == 4);   // (deny and inherited ACEs stay)
    LocalFree(merged); LocalFree(replaced); LocalFree(revoked);
    EXPLICIT_ACCESS_W nobody;
    BuildExplicitAccessWithNameW(&nobody, L"NoSuchAccountVX", 0x1, GRANT_ACCESS, NO_INHERITANCE);
    CHECK(SetEntriesInAclW(1, &nobody, NULL, &merged) == ERROR_NONE_MAPPED);
    set.grfAccessMode = (ACCESS_MODE)9;
    CHECK(SetEntriesInAclW(1, &set, NULL, &merged) == ERROR_INVALID_PARAMETER);
    PACL empty_acl = NULL;
    CHECK(SetEntriesInAclW(0, NULL, NULL, &empty_acl) == ERROR_SUCCESS && empty_acl && empty_acl->AceCount == 0);
    LocalFree(empty_acl);
    TRUSTEE_W owner_tr, group_tr;
    BuildTrusteeWithNameW(&owner_tr, L"CURRENT_USER"); BuildTrusteeWithSidW(&group_tr, users);
    ULONG bsize = 0; PSECURITY_DESCRIPTOR bsd = NULL;
    CHECK(BuildSecurityDescriptorW(&owner_tr, &group_tr, 1, ea, 0, NULL, NULL, &bsize, &bsd) == ERROR_SUCCESS && IsValidSecurityDescriptor(bsd) &&
          bsize == GetSecurityDescriptorLength(bsd));
    CHECK(ConvertSecurityDescriptorToStringSecurityDescriptorW(bsd, 1, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &text, NULL) &&
          !wcscmp(text, L"O:S-1-5-21-1000-1000-1000-1001G:BUD:(A;;CC;;;WD)"));
    LocalFree(text); LocalFree(bsd);

    // ---- names ----
    WCHAR name[64], dom[64], me[64]; DWORD nn = 64, dn = 64, men = 64; SID_NAME_USE use;
    CHECK(LookupAccountSidW(NULL, world, name, &nn, dom, &dn, &use) && !wcscmp(name, L"Everyone") && !wcscmp(dom, L"") && use == SidTypeWellKnownGroup && nn == 8 && dn == 0);
    nn = dn = 64;
    CHECK(LookupAccountSidW(NULL, users, name, &nn, dom, &dn, &use) && !wcscmp(name, L"Users") && !wcscmp(dom, L"BUILTIN") && use == SidTypeAlias);
    nn = dn = 64;
    CHECK(GetUserNameW(me, &men) && LookupAccountSidW(NULL, user, name, &nn, dom, &dn, &use) && !wcscmp(name, me) && use == SidTypeUser);
    nn = 2; dn = 64;
    CHECK(!LookupAccountSidW(NULL, world, name, &nn, dom, &dn, &use) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && nn == 9);
    nn = dn = 64;
    CHECK(!LookupAccountSidW(NULL, domain, name, &nn, dom, &dn, &use) && GetLastError() == ERROR_NONE_MAPPED);
    BYTE found[SECURITY_MAX_SID_SIZE]; DWORD fn = sizeof found;
    dn = 64;
    CHECK(LookupAccountNameW(NULL, L"BUILTIN\\Users", found, &fn, dom, &dn, &use) && EqualSid(found, users) && !wcscmp(dom, L"BUILTIN") && use == SidTypeAlias);
    fn = sizeof found; dn = 64;
    CHECK(LookupAccountNameW(NULL, me, found, &fn, dom, &dn, &use) && EqualSid(found, user) && use == SidTypeUser);
    fn = sizeof found; dn = 64;
    CHECK(!LookupAccountNameW(NULL, L"NoSuchAccountVX", found, &fn, dom, &dn, &use) && GetLastError() == ERROR_NONE_MAPPED);
    fn = 4; dn = 64;
    CHECK(!LookupAccountNameW(NULL, L"Everyone", found, &fn, dom, &dn, &use) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && fn == 12);

    // ---- processes as a token's user, logons, the rest ----
    WCHAR self[MAX_PATH], cmd[MAX_PATH + 16] = L"\"";
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wcscat(cmd, self); wcscat(cmd, L"\" child");
    STARTUPINFOW startup = { sizeof startup }; PROCESS_INFORMATION pi;
    CHECK(!CreateProcessAsUserW(imp, NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &pi) && GetLastError() == ERROR_BAD_TOKEN_TYPE);
    DWORD code = 0;
    // a token giving less than the user's own (restricted, lower integrity, a privilege removed): the child would run
    // with the user's, so it is refused and nothing starts
    memset(&pi, 0, sizeof pi);
    CHECK(!CreateProcessAsUserW(rt, NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &pi) && GetLastError() == ERROR_NOT_SUPPORTED && !pi.hProcess);
    CHECK(!CreateProcessAsUserW(wr, NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &pi) && GetLastError() == ERROR_NOT_SUPPORTED);
    HANDLE low_primary = NULL, fewer = NULL, plain = NULL;
    TOKEN_MANDATORY_LABEL to_low = { { low, SE_GROUP_INTEGRITY } };
    CHECK(DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &low_primary) &&
          SetTokenInformation(low_primary, TokenIntegrityLevel, &to_low, sizeof to_low));
    CHECK(!CreateProcessAsUserW(low_primary, NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &pi) && GetLastError() == ERROR_NOT_SUPPORTED);
    TOKEN_PRIVILEGES drop = { 1, { { { 19, 0 }, SE_PRIVILEGE_REMOVED } } };
    CHECK(DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &fewer) && AdjustTokenPrivileges(fewer, FALSE, &drop, 0, NULL, NULL));
    CHECK(!CreateProcessAsUserW(fewer, NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &pi) && GetLastError() == ERROR_NOT_SUPPORTED);
    // the user's own token, duplicated: the child starts, with the user's token
    CHECK(DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &plain));
    CHECK(CreateProcessAsUserW(plain, NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &pi) && WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0 &&
          GetExitCodeProcess(pi.hProcess, &code) && code == 42);
    HANDLE other = NULL;
    CHECK(OpenProcessToken(pi.hProcess, TOKEN_QUERY, &other) && integrity_of(other) == SECURITY_MANDATORY_MEDIUM_RID && !IsTokenRestricted(other));
    CloseHandle(other); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    CloseHandle(low_primary); CloseHandle(fewer); CloseHandle(plain);
    // CreateProcess's hThread: a thread of another process, whose tokens are its own: refused as such (not as a bad handle)
    code = 0;
    CHECK(CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startup, &pi));
    CHECK(!SetThreadToken(&pi.hThread, imp) && GetLastError() == ERROR_NOT_SUPPORTED);
    CHECK(!SetThreadToken(&pi.hThread, NULL) && GetLastError() == ERROR_NOT_SUPPORTED);
    CHECK(!OpenThreadToken(pi.hThread, TOKEN_QUERY, TRUE, &x) && GetLastError() == ERROR_NOT_SUPPORTED);
    CHECK(!ImpersonateAnonymousToken(pi.hThread) && GetLastError() == ERROR_NOT_SUPPORTED);
    CHECK(nt_set_thread && nt_set_thread(pi.hThread, 5 /* ThreadImpersonationToken */, &imp, sizeof imp) == (LONG)0xC00000BB);   // STATUS_NOT_SUPPORTED
    ResumeThread(pi.hThread);
    CHECK(WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code) && code == 42);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    HANDLE nothing = (HANDLE)(intptr_t)0x1234;
    CHECK(!SetThreadToken(&nothing, imp) && GetLastError() == ERROR_INVALID_HANDLE);
    HANDLE logon = (HANDLE)1;
    CHECK(!LogonUserW(L"nobody", L".", L"", LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &logon) && !logon);   // nothing checks passwords: refused
    CHECK(RegDisablePredefinedCache() == ERROR_SUCCESS);
    LUID l1, l2;
    CHECK(AllocateLocallyUniqueId(&l1) && AllocateLocallyUniqueId(&l2) && (l1.LowPart != l2.LowPart || l1.HighPart != l2.HighPart));

    CHECK(CloseHandle(imp) && CloseHandle(ident) && CloseHandle(rt) && CloseHandle(rt2) && CloseHandle(rimp) && CloseHandle(med) && CloseHandle(wr) && CloseHandle(wimp));
    CHECK(CloseHandle(rt3) && CloseHandle(rimp3) && CloseHandle(rt4) && CloseHandle(rimp4) && CloseHandle(wr2) && CloseHandle(wimp2));
    CHECK(!GetTokenInformation(imp, TokenType, &type, sizeof type, &n) && GetLastError() == ERROR_INVALID_HANDLE);
    CHECK(CloseHandle(tok));
    printf("security: SIDs, ACLs, descriptors, SDDL, tokens, privileges, impersonation, AccessCheck, object security -> %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
