// crypt32 and wintrust with made-up certificates (tests/crypt32_certs.h: a test root, an RSA leaf it signed, a
// self-signed P-256 one) and made-up names: parsing, names as text, validity, key usages, stores, signatures checked by
// macOS (and changed ones refused), nothing trusted that was not verified, and DPAPI (round trips, entropy, tampering).
//   crypt32.exe                  all of it
//   crypt32.exe protect <file>   writes a DPAPI blob to <file>; "unprotect <file>" opens it in another process
#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <stdio.h>
#include <string.h>
#include "crypt32_certs.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf(" (error 0x%lx)\n", GetLastError()); failures++; } } while (0)

static PCCERT_CONTEXT root, leaf, ec;
static ULONGLONG ft(const FILETIME *f) { return (ULONGLONG)f->dwHighDateTime << 32 | f->dwLowDateTime; }
static FILETIME to_ft(ULONGLONG v) { FILETIME f = { (DWORD)v, (DWORD)(v >> 32) }; return f; }

static void name_is(PCCERT_CONTEXT c, DWORD type, DWORD flags, void *para, const WCHAR *want) {
    WCHAR buf[512];
    DWORD n = CertGetNameStringW(c, type, flags, para, buf, 512);
    CHECK(n == wcslen(want) + 1 && !wcscmp(buf, want), "CertGetNameStringW(%lu, 0x%lx) = %lu \"%ls\", want \"%ls\"", type, flags, n, buf, want);
}

static void test_parse(void) {
    CERT_INFO *i = leaf->pCertInfo;
    CHECK(leaf->dwCertEncodingType == X509_ASN_ENCODING && leaf->cbCertEncoded == sizeof cert_leaf &&
          !memcmp(leaf->pbCertEncoded, cert_leaf, sizeof cert_leaf), "encoded copy");
    CHECK(i->dwVersion == CERT_V3, "version %lu", i->dwVersion);
    static const BYTE serial[] = { 0xd6, 0xc4, 0xb3, 0xa2, 0x01 };   // 0x01A2B3C4D6, least significant byte first
    CHECK(i->SerialNumber.cbData == 5 && !memcmp(i->SerialNumber.pbData, serial, 5), "serial");
    CHECK(!strcmp(i->SignatureAlgorithm.pszObjId, szOID_RSA_SHA256RSA), "signature algorithm %s", i->SignatureAlgorithm.pszObjId);
    CHECK(i->SignatureAlgorithm.Parameters.cbData == 2 && i->SignatureAlgorithm.Parameters.pbData[0] == 5, "NULL parameters");
    CHECK(ft(&i->NotBefore) == 0x1d6dfd10c358000ull && ft(&i->NotAfter) == 0x1d91caad3594000ull, "leaf validity (UTCTime)");
    CHECK(ft(&root->pCertInfo->NotAfter) == 0x20298ccf0348000ull, "root NotAfter 2060 (GeneralizedTime)");
    CHECK(ft(&ec->pCertInfo->NotBefore) == 0x1d875b34f739400ull && ft(&ec->pCertInfo->NotAfter) == 0x1e3ac3d86745400ull, "ec validity");
    CHECK(!strcmp(i->SubjectPublicKeyInfo.Algorithm.pszObjId, szOID_RSA_RSA) && i->SubjectPublicKeyInfo.PublicKey.cbData == 270 &&
          i->SubjectPublicKeyInfo.PublicKey.cUnusedBits == 0, "RSA key");
    CHECK(!strcmp(ec->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId, szOID_ECC_PUBLIC_KEY) &&
          ec->pCertInfo->SubjectPublicKeyInfo.PublicKey.cbData == 65, "EC key");
    CHECK(i->cExtension == 4, "extensions %lu", i->cExtension);
    if (i->cExtension == 4) {
        CHECK(!strcmp(i->rgExtension[0].pszObjId, szOID_BASIC_CONSTRAINTS2) && !i->rgExtension[0].fCritical, "ext 0");
        CHECK(!strcmp(i->rgExtension[1].pszObjId, szOID_KEY_USAGE) && i->rgExtension[1].fCritical, "ext 1 critical");
        CHECK(!strcmp(i->rgExtension[2].pszObjId, szOID_ENHANCED_KEY_USAGE), "ext 2");
        CHECK(!strcmp(i->rgExtension[3].pszObjId, szOID_SUBJECT_ALT_NAME2) && i->rgExtension[3].Value.pbData[0] == 0x30, "ext 3");
    }
    CHECK(CertCompareCertificateName(X509_ASN_ENCODING, &leaf->pCertInfo->Issuer, &root->pCertInfo->Subject), "leaf issuer = root");
    CHECK(!CertCompareCertificateName(X509_ASN_ENCODING, &leaf->pCertInfo->Subject, &root->pCertInfo->Subject), "names differ");
    BYTE junk[] = { 0x30, 0x03, 0x02, 0x01, 0x01 };
    CHECK(!CertCreateCertificateContext(X509_ASN_ENCODING, junk, sizeof junk) && (GetLastError() >> 16) == 0x8009, "junk refused");
    CHECK(!CertCreateCertificateContext(X509_ASN_ENCODING, cert_leaf, 100), "truncated refused");
}

static void test_names(void) {
    name_is(leaf, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, L"leaf.vxrt.test");
    name_is(leaf, CERT_NAME_SIMPLE_DISPLAY_TYPE, CERT_NAME_ISSUER_FLAG, NULL, L"VXRT Test Root CA");
    name_is(leaf, CERT_NAME_FRIENDLY_DISPLAY_TYPE, 0, NULL, L"leaf.vxrt.test");
    name_is(leaf, CERT_NAME_EMAIL_TYPE, 0, NULL, L"alt@vxrt.test");           // the alternative name first
    name_is(leaf, CERT_NAME_DNS_TYPE, 0, NULL, L"leaf.vxrt.test");
    name_is(leaf, CERT_NAME_URL_TYPE, 0, NULL, L"https://vxrt.test/leaf");
    name_is(leaf, CERT_NAME_UPN_TYPE, 0, NULL, L"user@vxrt.test");
    name_is(leaf, CERT_NAME_ATTR_TYPE, 0, szOID_ORGANIZATIONAL_UNIT_NAME, L"Runtime");
    name_is(leaf, CERT_NAME_ATTR_TYPE, 0, szOID_LOCALITY_NAME, L"Shibuya");
    name_is(root, CERT_NAME_EMAIL_TYPE, 0, NULL, L"");
    name_is(root, CERT_NAME_URL_TYPE, 0, NULL, L"");
    name_is(root, CERT_NAME_DNS_TYPE, 0, NULL, L"VXRT Test Root CA");      // no DNS names: the CN
    name_is(ec, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, L"VXRT EC Test");
    DWORD x500 = CERT_X500_NAME_STR, rev = CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG, simple = CERT_SIMPLE_NAME_STR, oid = CERT_OID_NAME_STR;
    name_is(leaf, CERT_NAME_RDN_TYPE, 0, &x500, L"C=JP, S=Tokyo, L=Shibuya, O=VeRantyx Test, OU=Runtime, CN=leaf.vxrt.test, E=leaf@vxrt.test");
    name_is(leaf, CERT_NAME_RDN_TYPE, 0, &rev, L"E=leaf@vxrt.test, CN=leaf.vxrt.test, OU=Runtime, O=VeRantyx Test, L=Shibuya, S=Tokyo, C=JP");
    name_is(leaf, CERT_NAME_RDN_TYPE, 0, &simple, L"JP, Tokyo, Shibuya, VeRantyx Test, Runtime, leaf.vxrt.test, leaf@vxrt.test");
    name_is(root, CERT_NAME_RDN_TYPE, 0, &oid, L"2.5.4.6=JP, 2.5.4.10=VeRantyx Test, 2.5.4.3=VXRT Test Root CA");
    name_is(ec, CERT_NAME_RDN_TYPE, 0, &x500, L"O=VeRantyx Test, OU=\"Signing \"\"Lab\"\", Tokyo\", CN=VXRT EC Test");

    // every DNS name, as a multi-string
    WCHAR buf[128];
    DWORD n = CertGetNameStringW(leaf, CERT_NAME_DNS_TYPE, CERT_NAME_SEARCH_ALL_NAMES_FLAG, NULL, buf, 128);
    CHECK(n == 30 && !wcscmp(buf, L"leaf.vxrt.test") && !wcscmp(buf + 15, L"alt.vxrt.test") && !buf[29], "all DNS names (%lu)", n);
    n = CertGetNameStringW(leaf, CERT_NAME_EMAIL_TYPE, CERT_NAME_SEARCH_ALL_NAMES_FLAG, NULL, buf, 128);
    CHECK(n == 1 && !buf[0], "search-all is for DNS names only");
    // sizes and truncation
    CHECK(CertGetNameStringW(leaf, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, NULL, 0) == 15, "size");
    n = CertGetNameStringW(leaf, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, buf, 5);
    CHECK(n == 5 && !wcscmp(buf, L"leaf"), "truncated %lu", n);
    char a[128];
    n = CertGetNameStringA(leaf, CERT_NAME_SIMPLE_DISPLAY_TYPE, CERT_NAME_ISSUER_FLAG, NULL, a, sizeof a);
    CHECK(n == 18 && !strcmp(a, "VXRT Test Root CA"), "A %lu %s", n, a);

    // a made-up name with every kind of value: BMPString, UTF8String, a value with a leading space (quoted), an OCTET
    // STRING (hex), TeletexString that is UTF-8 and one that is not (8-bit)
    static const BYTE name[] = {
        0x30, 0x5e,
        0x31, 0x0f, 0x30, 0x0d, 0x06, 0x03, 0x55, 0x04, 0x03, 0x1e, 0x06, 0x00, 0xdc, 0x00, 0x6e, 0x00, 0xef,
        0x31, 0x1c, 0x30, 0x0d, 0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x06, 0xe6, 0x97, 0xa5, 0xe6, 0x9c, 0xac,
                    0x30, 0x0b, 0x06, 0x03, 0x55, 0x04, 0x0b, 0x13, 0x04, 0x20, 0x70, 0x61, 0x64,
        0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x2a, 0x03, 0x04, 0x04, 0x02, 0x01, 0x02,
        0x31, 0x0e, 0x30, 0x0c, 0x06, 0x03, 0x55, 0x04, 0x07, 0x14, 0x05, 0x63, 0x61, 0x66, 0xc3, 0xa9,
        0x31, 0x10, 0x30, 0x0e, 0x06, 0x03, 0x55, 0x04, 0x08, 0x14, 0x07, 0x4d, 0xfc, 0x6e, 0x63, 0x68, 0x65, 0x6e,
    };
    CERT_NAME_BLOB blob = { sizeof name, (BYTE *)name };
    WCHAR s[256];
    n = CertNameToStrW(X509_ASN_ENCODING, &blob, CERT_X500_NAME_STR, s, 256);
    const WCHAR *want = L"CN=Ünï, O=日本+ OU=\" pad\", OID.1.2.3.4=#04020102, L=café, S=München";
    CHECK(n == wcslen(want) + 1 && !wcscmp(s, want), "X500 \"%ls\"", s);
    n = CertNameToStrW(X509_ASN_ENCODING, &blob, CERT_SIMPLE_NAME_STR | CERT_NAME_STR_NO_QUOTING_FLAG | CERT_NAME_STR_SEMICOLON_FLAG, s, 256);
    want = L"Ünï; 日本 +  pad; #04020102; café; München";
    CHECK(n == wcslen(want) + 1 && !wcscmp(s, want), "simple \"%ls\"", s);
    CertNameToStrW(X509_ASN_ENCODING, &blob, CERT_X500_NAME_STR | CERT_NAME_STR_DISABLE_IE4_UTF8_FLAG, s, 256);
    CHECK(wcsstr(s, L"L=cafÃ©,") != NULL, "TeletexString as 8-bit \"%ls\"", s);
    n = CertNameToStrA(X509_ASN_ENCODING, &blob, CERT_X500_NAME_STR, a, sizeof a);
    CHECK(n == strlen(a) + 1 && !strncmp(a, "CN=\xc3\x9cn\xc3\xaf, O=\xe6\x97\xa5\xe6\x9c\xac+ OU=", 22), "A (UTF-8) \"%s\"", a);
}

static void test_validity_and_usage(void) {
    CERT_INFO *i = leaf->pCertInfo;
    FILETIME t = to_ft(ft(&i->NotBefore) - 1);
    CHECK(CertVerifyTimeValidity(&t, i) == -1, "before");
    t = i->NotBefore;
    CHECK(CertVerifyTimeValidity(&t, i) == 0, "at NotBefore");
    t = i->NotAfter;
    CHECK(CertVerifyTimeValidity(&t, i) == 0, "at NotAfter");
    t = to_ft(ft(&i->NotAfter) + 1);
    CHECK(CertVerifyTimeValidity(&t, i) == 1, "after");
    CHECK(CertVerifyTimeValidity(NULL, root->pCertInfo) == 0, "the root (2020-2060) is valid now");

    BYTE buf[512];
    DWORD size = 0;
    CHECK(CertGetEnhancedKeyUsage(leaf, 0, NULL, &size) && size > sizeof(CERT_ENHKEY_USAGE), "EKU size");
    DWORD small = sizeof(CERT_ENHKEY_USAGE);
    CHECK(!CertGetEnhancedKeyUsage(leaf, 0, (CERT_ENHKEY_USAGE *)buf, &small) && GetLastError() == ERROR_MORE_DATA && small == size, "EKU small");
    CERT_ENHKEY_USAGE *u = (CERT_ENHKEY_USAGE *)buf;
    size = sizeof buf;
    CHECK(CertGetEnhancedKeyUsage(leaf, 0, u, &size) && u->cUsageIdentifier == 2 && !strcmp(u->rgpszUsageIdentifier[0], szOID_PKIX_KP_SERVER_AUTH) &&
          !strcmp(u->rgpszUsageIdentifier[1], szOID_PKIX_KP_CLIENT_AUTH), "leaf EKU");
    size = sizeof buf;
    CHECK(CertGetEnhancedKeyUsage(ec, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, u, &size) && u->cUsageIdentifier == 1 &&
          !strcmp(u->rgpszUsageIdentifier[0], szOID_PKIX_KP_CODE_SIGNING), "ec EKU");
    size = sizeof buf;
    CHECK(CertGetEnhancedKeyUsage(root, 0, u, &size) && u->cUsageIdentifier == 0 && GetLastError() == (DWORD)CRYPT_E_NOT_FOUND, "no EKU: all uses");
    size = sizeof buf;
    CHECK(CertGetEnhancedKeyUsage(leaf, CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG, u, &size) && u->cUsageIdentifier == 0 &&
          GetLastError() == (DWORD)CRYPT_E_NOT_FOUND, "no EKU property");

    BYTE ku[2] = { 0xff, 0xff };
    CHECK(CertGetIntendedKeyUsage(X509_ASN_ENCODING, leaf->pCertInfo, ku, 2) && ku[0] == (CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_ENCIPHERMENT_KEY_USAGE) &&
          ku[1] == 0, "leaf key usage %02x %02x", ku[0], ku[1]);
    CHECK(CertGetIntendedKeyUsage(X509_ASN_ENCODING, root->pCertInfo, ku, 1) && ku[0] == (CERT_KEY_CERT_SIGN_KEY_USAGE | CERT_CRL_SIGN_KEY_USAGE), "root key usage");
    CERT_INFO bare = *leaf->pCertInfo;
    bare.cExtension = 0;
    ku[0] = 0xff;
    SetLastError(1);
    CHECK(!CertGetIntendedKeyUsage(X509_ASN_ENCODING, &bare, ku, 1) && ku[0] == 0 && GetLastError() == 0, "no key usage");
}

static void test_properties(void) {
    BYTE h[64];
    DWORD n = 0;
    CHECK(CertGetCertificateContextProperty(leaf, CERT_SHA1_HASH_PROP_ID, NULL, &n) && n == 20, "size");
    n = 10;
    CHECK(!CertGetCertificateContextProperty(leaf, CERT_SHA1_HASH_PROP_ID, h, &n) && GetLastError() == ERROR_MORE_DATA && n == 20, "small");
    n = sizeof h;
    CHECK(CertGetCertificateContextProperty(leaf, CERT_SHA1_HASH_PROP_ID, h, &n) && n == 20 && !memcmp(h, sha1_leaf, 20), "SHA-1");
    n = sizeof h;
    CHECK(CertGetCertificateContextProperty(root, CERT_HASH_PROP_ID, h, &n) && !memcmp(h, sha1_root, 20), "hash");
    n = sizeof h;
    CHECK(CertGetCertificateContextProperty(leaf, CERT_KEY_IDENTIFIER_PROP_ID, h, &n) && n == 20 && !memcmp(h, keyid_leaf, 20), "key id (computed)");
    PCERT_EXTENSION ski = CertFindExtension(szOID_SUBJECT_KEY_IDENTIFIER, root->pCertInfo->cExtension, root->pCertInfo->rgExtension);
    n = sizeof h;
    CHECK(ski && CertGetCertificateContextProperty(root, CERT_KEY_IDENTIFIER_PROP_ID, h, &n) && n == ski->Value.cbData - 2 &&
          !memcmp(h, ski->Value.pbData + 2, n), "key id (extension)");
    n = sizeof h;
    CHECK(!CertGetCertificateContextProperty(leaf, CERT_KEY_PROV_INFO_PROP_ID, h, &n) && GetLastError() == (DWORD)CRYPT_E_NOT_FOUND, "no key");
}

static int count(HCERTSTORE s) {
    int k = 0;
    for (PCCERT_CONTEXT c = NULL; (c = CertEnumCertificatesInStore(s, c));) k++;
    return k;
}

static void test_stores(void) {
    HCERTSTORE mem = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    CHECK(mem != NULL, "memory store");
    PCCERT_CONTEXT a = NULL, b = NULL;
    CHECK(CertAddEncodedCertificateToStore(mem, X509_ASN_ENCODING, cert_leaf, sizeof cert_leaf, CERT_STORE_ADD_NEW, &a) && a && a->hCertStore == mem, "add");
    CHECK(!CertAddEncodedCertificateToStore(mem, X509_ASN_ENCODING, cert_leaf, sizeof cert_leaf, CERT_STORE_ADD_NEW, NULL) &&
          GetLastError() == (DWORD)CRYPT_E_EXISTS, "add new twice");
    CHECK(CertAddEncodedCertificateToStore(mem, X509_ASN_ENCODING, cert_leaf, sizeof cert_leaf, CERT_STORE_ADD_USE_EXISTING, &b) && b == a, "use existing");
    CertFreeCertificateContext(b);
    CHECK(CertAddCertificateContextToStore(mem, root, CERT_STORE_ADD_ALWAYS, NULL), "add context");
    CHECK(CertAddCertificateContextToStore(mem, ec, CERT_STORE_ADD_REPLACE_EXISTING, NULL), "replace (none)");
    CHECK(CertAddCertificateContextToStore(mem, ec, CERT_STORE_ADD_REPLACE_EXISTING, NULL), "replace");
    CHECK(count(mem) == 3, "count %d", count(mem));

    CRYPT_HASH_BLOB hash = { 20, (BYTE *)sha1_leaf };
    PCCERT_CONTEXT f = CertFindCertificateInStore(mem, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &hash, NULL);
    CHECK(f == a, "find by hash");
    CertFreeCertificateContext(f);
    f = CertFindCertificateInStore(mem, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, L"vxrt test ROOT", NULL);
    CHECK(f && CertCompareCertificateName(X509_ASN_ENCODING, &f->pCertInfo->Subject, &root->pCertInfo->Subject), "find by subject text");
    CertFreeCertificateContext(f);
    int issued = 0;
    for (f = NULL; (f = CertFindCertificateInStore(mem, X509_ASN_ENCODING, 0, CERT_FIND_ISSUER_NAME, &root->pCertInfo->Subject, f));) issued++;
    CHECK(issued == 2 && GetLastError() == (DWORD)CRYPT_E_NOT_FOUND, "issued by the root: %d", issued);
    f = CertFindCertificateInStore(mem, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, ec, NULL);
    CHECK(f && f != ec && f->cbCertEncoded == ec->cbCertEncoded, "find existing");
    CHECK(CertDeleteCertificateFromStore(f) && count(mem) == 2, "delete");

    // a collection: siblings by priority, additions to the one that allows them
    HCERTSTORE coll = CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, NULL), mem2 = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    CHECK(CertAddStoreToCollection(coll, mem, 0, 1) && CertAddStoreToCollection(coll, mem2, CERT_PHYSICAL_STORE_ADD_ENABLE_FLAG, 2), "siblings");
    CHECK(!CertAddStoreToCollection(coll, coll, 0, 0), "not into itself");
    CHECK(CertAddCertificateContextToStore(coll, ec, CERT_STORE_ADD_ALWAYS, NULL) && count(mem2) == 1 && count(coll) == 3, "add to collection");
    f = CertEnumCertificatesInStore(coll, NULL);
    CHECK(f && f->hCertStore == mem2, "highest priority first");
    CertFreeCertificateContext(f);
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK(CertControlStore(coll, 0, CERT_STORE_CTRL_AUTO_RESYNC, NULL) && CertControlStore(coll, 0, CERT_STORE_CTRL_NOTIFY_CHANGE, &ev) &&
          CertControlStore(coll, 0, CERT_STORE_CTRL_RESYNC, &ev) && WaitForSingleObject(ev, 0) == WAIT_TIMEOUT, "control");
    CloseHandle(ev);
    CHECK(CertCloseStore(coll, 0) && CertCloseStore(mem2, 0), "close collection");

    // closing with a context held: closed all the same, the context stays valid
    CHECK(!CertCloseStore(mem, CERT_CLOSE_STORE_CHECK_FLAG) && GetLastError() == (DWORD)CRYPT_E_PENDING_CLOSE, "pending close");
    WCHAR buf[64];
    CHECK(CertGetNameStringW(a, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, buf, 64) == 15, "held context");
    PCCERT_CONTEXT d = CertDuplicateCertificateContext(a);
    CHECK(d == a && CertFreeCertificateContext(d) && CertFreeCertificateContext(a), "duplicate and free");

    // the system stores are empty: no root and no personal certificate comes from them
    HCERTSTORE sys = CertOpenSystemStoreW(0, L"ROOT");
    CHECK(sys && !CertEnumCertificatesInStore(sys, NULL) && GetLastError() == (DWORD)CRYPT_E_NOT_FOUND, "ROOT is empty");
    CertCloseStore(sys, 0);
    sys = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_READONLY_FLAG, L"CA");
    CHECK(sys && !CertAddEncodedCertificateToStore(sys, X509_ASN_ENCODING, cert_root, sizeof cert_root, CERT_STORE_ADD_ALWAYS, NULL), "read-only");
    CertCloseStore(sys, 0);
}

static void test_signatures(void) {
    CHECK(CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void *)leaf,
                                            CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void *)root, 0, NULL), "leaf by root (RSA)");
    CHECK(CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void *)root,
                                            CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void *)root, 0, NULL), "root self-signed");
    CHECK(CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void *)ec,
                                            CRYPT_VERIFY_CERT_SIGN_ISSUER_PUBKEY, &ec->pCertInfo->SubjectPublicKeyInfo, 0, NULL), "ECDSA self-signed");
    CRYPT_DATA_BLOB whole = { sizeof cert_leaf, (BYTE *)cert_leaf };
    CHECK(CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_BLOB, &whole,
                                            CRYPT_VERIFY_CERT_SIGN_ISSUER_PUBKEY, &root->pCertInfo->SubjectPublicKeyInfo, 0, NULL), "blob");
    // the wrong key, a key of the wrong type, no issuer, a chain: never valid
    CHECK(!CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void *)leaf,
                                             CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void *)leaf, 0, NULL) && GetLastError() == (DWORD)NTE_BAD_SIGNATURE, "wrong key");
    CHECK(!CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void *)leaf,
                                             CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void *)ec, 0, NULL) && GetLastError() == (DWORD)NTE_BAD_ALGID, "EC key for RSA");
    CHECK(!CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void *)root,
                                             CRYPT_VERIFY_CERT_SIGN_ISSUER_NULL, NULL, 0, NULL), "no issuer");
    CHECK(!CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void *)root,
                                             CRYPT_VERIFY_CERT_SIGN_ISSUER_CHAIN, (void *)root, 0, NULL), "chain issuer");
    CHECK(!CryptVerifyCertificateSignatureEx(0, 0x10000, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void *)leaf,
                                             CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void *)root, 0, NULL) && GetLastError() == ERROR_FILE_NOT_FOUND, "encoding");
    // changed bytes: in the signature, in the subject's name
    BYTE copy[sizeof cert_leaf];
    memcpy(copy, cert_leaf, sizeof copy);
    copy[sizeof copy - 1] ^= 0x01;
    PCCERT_CONTEXT bad = CertCreateCertificateContext(X509_ASN_ENCODING, copy, sizeof copy);
    CHECK(bad && !CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void *)bad,
                                                    CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void *)root, 0, NULL) && GetLastError() == (DWORD)NTE_BAD_SIGNATURE, "changed signature");
    CertFreeCertificateContext(bad);
    memcpy(copy, cert_leaf, sizeof copy);
    BYTE *cn = NULL;
    for (size_t i = 0; i + 14 <= sizeof copy && !cn; i++) if (!memcmp(copy + i, "leaf.vxrt.test", 14)) cn = copy + i;
    if (cn) cn[3] = 'g';                                                  // "leag.vxrt.test"
    bad = CertCreateCertificateContext(X509_ASN_ENCODING, copy, sizeof copy);
    CHECK(cn && bad && !CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void *)bad,
                                                          CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void *)root, 0, NULL), "changed subject");
    CertFreeCertificateContext(bad);

    // no private key, no client certificate chain
    HCERTSTORE mem = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    CertAddCertificateContextToStore(mem, root, CERT_STORE_ADD_ALWAYS, NULL);
    CertAddCertificateContextToStore(mem, leaf, CERT_STORE_ADD_ALWAYS, NULL);
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 1;
    DWORD spec = 1;
    BOOL frees = TRUE;
    CHECK(!CryptAcquireCertificatePrivateKey(leaf, 0, NULL, &key, &spec, &frees) && GetLastError() == (DWORD)CRYPT_E_NO_KEY_PROPERTY &&
          !key && !frees, "no private key");
    CERT_CHAIN_FIND_BY_ISSUER_PARA find = { sizeof find };
    CHECK(!CertFindChainInStore(mem, X509_ASN_ENCODING, 0, CERT_CHAIN_FIND_BY_ISSUER, &find, NULL) &&
          GetLastError() == (DWORD)CRYPT_E_NOT_FOUND, "no client chain");
    CHECK(CryptMsgClose(NULL), "close no message");
    CertCloseStore(mem, 0);
}

// ---- chains: judged by macOS -------------------------------------------------------------------------------------------
static DWORD policy(PCCERT_CHAIN_CONTEXT chain, LPCSTR which, DWORD flags, const WCHAR *server) {
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl = { { sizeof ssl }, AUTHTYPE_SERVER, 0, (WCHAR *)server };
    CERT_CHAIN_POLICY_PARA para = { sizeof para, flags, which == CERT_CHAIN_POLICY_SSL ? &ssl : NULL };
    CERT_CHAIN_POLICY_STATUS st = { sizeof st, 0xdeadbeef };
    if (!CertVerifyCertificateChainPolicy(which, chain, &para, &st)) return 0xdeadbeef;
    return st.dwError;
}

static void test_chains(void) {
    FILETIME in_2022 = to_ft(0x1d8754a895c8000ull), in_2023 = to_ft(0x1d9941c02234000ull);   // 2022-06-01, 2023-06-01
    HCERTSTORE extra = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL), roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    CertAddCertificateContextToStore(extra, root, CERT_STORE_ADD_ALWAYS, NULL);
    CertAddCertificateContextToStore(roots, root, CERT_STORE_ADD_ALWAYS, NULL);
    CERT_CHAIN_PARA para = { sizeof para };
    PCCERT_CHAIN_CONTEXT chain = NULL;

    // macOS does not trust the made-up root: the chain is built, and only its root is the problem
    CHECK(CertGetCertificateChain(NULL, leaf, &in_2022, extra, &para, 0, NULL, &chain) && chain, "chain");
    if (chain) {
        PCERT_SIMPLE_CHAIN sc = chain->rgpChain[0];
        CHECK(chain->cChain == 1 && sc->cElement == 2 && sc->rgpElement[0]->pCertContext == leaf &&
              CertCompareCertificateName(X509_ASN_ENCODING, &sc->rgpElement[1]->pCertContext->pCertInfo->Subject, &root->pCertInfo->Subject),
              "elements");
        CHECK(chain->TrustStatus.dwErrorStatus == CERT_TRUST_IS_UNTRUSTED_ROOT && sc->rgpElement[1]->TrustStatus.dwErrorStatus == CERT_TRUST_IS_UNTRUSTED_ROOT &&
              (sc->rgpElement[1]->TrustStatus.dwInfoStatus & CERT_TRUST_IS_SELF_SIGNED), "untrusted root: 0x%lx", chain->TrustStatus.dwErrorStatus);
        CHECK(policy(chain, CERT_CHAIN_POLICY_BASE, 0, NULL) == (DWORD)CERT_E_UNTRUSTEDROOT, "base");
        CHECK(policy(chain, CERT_CHAIN_POLICY_SSL, 0, L"leaf.vxrt.test") == (DWORD)CERT_E_UNTRUSTEDROOT, "ssl");
        // with the root excused, what macOS verified (signatures, names) decides
        CHECK(policy(chain, CERT_CHAIN_POLICY_BASE, CERT_CHAIN_POLICY_ALLOW_UNKNOWN_CA_FLAG, NULL) == 0, "base, unknown CA allowed");
        CHECK(policy(chain, CERT_CHAIN_POLICY_SSL, CERT_CHAIN_POLICY_ALLOW_UNKNOWN_CA_FLAG, L"leaf.vxrt.test") == 0, "ssl, unknown CA allowed");
        CHECK(policy(chain, CERT_CHAIN_POLICY_SSL, CERT_CHAIN_POLICY_ALLOW_UNKNOWN_CA_FLAG, L"other.vxrt.test") == (DWORD)CERT_E_CN_NO_MATCH, "name");
        CHECK(policy(chain, CERT_CHAIN_POLICY_MICROSOFT_ROOT, CERT_CHAIN_POLICY_ALLOW_UNKNOWN_CA_FLAG, NULL) == (DWORD)CERT_E_UNTRUSTEDROOT, "not Microsoft's");
        PCCERT_CHAIN_CONTEXT dup = CertDuplicateCertificateChain(chain);
        CHECK(dup == chain, "duplicate");
        CertFreeCertificateChain(dup);
        CertFreeCertificateChain(chain);
    }
    // without the root: a partial chain, never excused
    CHECK(CertGetCertificateChain(NULL, leaf, &in_2022, NULL, &para, CERT_CHAIN_DISABLE_AIA, NULL, &chain) &&
          (chain->TrustStatus.dwErrorStatus & CERT_TRUST_IS_PARTIAL_CHAIN) &&
          policy(chain, CERT_CHAIN_POLICY_BASE, CERT_CHAIN_POLICY_ALLOW_UNKNOWN_CA_FLAG, NULL) == (DWORD)CERT_E_CHAINING, "partial");
    CertFreeCertificateChain(chain);

    // an engine whose only root is the made-up one: trusted, for the right name, at the right time, for the right usage
    CERT_CHAIN_ENGINE_CONFIG cfg = { sizeof cfg };
    cfg.hExclusiveRoot = roots;
    HCERTCHAINENGINE engine = NULL;
    CHECK(CertCreateCertificateChainEngine(&cfg, &engine) && engine, "engine");
    CHECK(CertGetCertificateChain(engine, leaf, &in_2022, extra, &para, 0, NULL, &chain) && chain->TrustStatus.dwErrorStatus == 0,
          "trusted through the engine's root: 0x%lx", chain ? chain->TrustStatus.dwErrorStatus : 0);
    CHECK(policy(chain, CERT_CHAIN_POLICY_BASE, 0, NULL) == 0 && policy(chain, CERT_CHAIN_POLICY_SSL, 0, L"leaf.vxrt.test") == 0 &&
          policy(chain, CERT_CHAIN_POLICY_SSL, 0, L"alt.vxrt.test") == 0, "policies hold");
    CHECK(policy(chain, CERT_CHAIN_POLICY_SSL, 0, L"evil.vxrt.test") == (DWORD)CERT_E_CN_NO_MATCH, "another name");
    CertFreeCertificateChain(chain);
    CHECK(CertGetCertificateChain(engine, leaf, &in_2023, extra, &para, 0, NULL, &chain) &&
          chain->TrustStatus.dwErrorStatus == CERT_TRUST_IS_NOT_TIME_VALID && policy(chain, CERT_CHAIN_POLICY_BASE, 0, NULL) == (DWORD)CERT_E_EXPIRED &&
          policy(chain, CERT_CHAIN_POLICY_BASE, CERT_CHAIN_POLICY_IGNORE_NOT_TIME_VALID_FLAG, NULL) == 0, "expired: 0x%lx",
          chain ? chain->TrustStatus.dwErrorStatus : 0);
    CertFreeCertificateChain(chain);
    LPSTR code_signing[] = { szOID_PKIX_KP_CODE_SIGNING };
    CERT_CHAIN_PARA usage = { sizeof usage, { USAGE_MATCH_TYPE_AND, { 1, code_signing } } };
    CHECK(CertGetCertificateChain(engine, leaf, &in_2022, extra, &usage, 0, NULL, &chain) &&
          chain->TrustStatus.dwErrorStatus == CERT_TRUST_IS_NOT_VALID_FOR_USAGE && policy(chain, CERT_CHAIN_POLICY_BASE, 0, NULL) == (DWORD)CERT_E_WRONG_USAGE,
          "usage: 0x%lx", chain ? chain->TrustStatus.dwErrorStatus : 0);
    CertFreeCertificateChain(chain);

    // a changed signature is never trusted, whatever is excused
    BYTE copy[sizeof cert_leaf];
    memcpy(copy, cert_leaf, sizeof copy);
    copy[sizeof copy - 1] ^= 0x01;
    PCCERT_CONTEXT bad = CertCreateCertificateContext(X509_ASN_ENCODING, copy, sizeof copy);
    CHECK(bad && CertGetCertificateChain(engine, bad, &in_2022, extra, &para, 0, NULL, &chain) &&
          (chain->TrustStatus.dwErrorStatus & CERT_TRUST_IS_NOT_SIGNATURE_VALID) &&
          policy(chain, CERT_CHAIN_POLICY_SSL, 0xffff, L"leaf.vxrt.test") == (DWORD)TRUST_E_CERT_SIGNATURE, "changed signature: 0x%lx",
          chain ? chain->TrustStatus.dwErrorStatus : 0);
    CertFreeCertificateChain(chain);
    CertFreeCertificateContext(bad);
    CertFreeCertificateChainEngine(engine);

    // a chain context not made here is not trusted
    CERT_CHAIN_CONTEXT fake = { sizeof fake };
    CHECK(policy(&fake, CERT_CHAIN_POLICY_BASE, 0xffff, NULL) == (DWORD)CERT_E_UNTRUSTEDROOT, "made-up chain context");
    CertCloseStore(extra, 0);
    CertCloseStore(roots, 0);
}

// ---- DPAPI ----------------------------------------------------------------------------------------------------------
static int unprotects(DATA_BLOB *blob, DATA_BLOB *entropy, const void *want, DWORD n) {
    DATA_BLOB out = { 0 };
    if (!CryptUnprotectData(blob, NULL, entropy, NULL, NULL, 0, &out)) return 0;
    int same = out.cbData == n && (!n || !memcmp(out.pbData, want, n));
    LocalFree(out.pbData);
    return same;
}

static void test_dpapi(void) {
    static const char secret[] = "a made-up secret, not a password";
    DATA_BLOB in = { sizeof secret, (BYTE *)secret }, blob = { 0 }, blob2 = { 0 }, out = { 0 };
    CHECK(CryptProtectData(&in, L"made-up description", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &blob), "protect");
    CHECK(blob.cbData > in.cbData, "blob size %lu", blob.cbData);
    int plain_inside = 0;
    for (DWORD i = 0; i + 8 <= blob.cbData; i++) if (!memcmp(blob.pbData + i, "made-up", 7)) plain_inside = 1;
    CHECK(!plain_inside, "the blob does not show the data or its description");
    LPWSTR descr = NULL;
    CHECK(CryptUnprotectData(&blob, &descr, NULL, NULL, NULL, 0, &out) && out.cbData == in.cbData && !memcmp(out.pbData, secret, in.cbData) &&
          descr && !wcscmp(descr, L"made-up description"), "round trip");
    LocalFree(out.pbData);
    LocalFree(descr);
    CHECK(CryptProtectData(&in, NULL, NULL, NULL, NULL, 0, &blob2) && (blob2.cbData != blob.cbData || memcmp(blob2.pbData, blob.pbData, blob.cbData)),
          "each blob is different");
    descr = (LPWSTR)1;
    CHECK(CryptUnprotectData(&blob2, &descr, NULL, NULL, NULL, 0, &out) && descr && !descr[0], "no description");
    LocalFree(out.pbData);
    LocalFree(descr);
    LocalFree(blob2.pbData);

    // every byte changed is refused: the header, the ciphertext, the tag; and a shorter blob
    for (DWORD i = 0; i < blob.cbData; i++) {
        blob.pbData[i] ^= 0x20;
        int opened = unprotects(&blob, NULL, secret, in.cbData);
        blob.pbData[i] ^= 0x20;
        if (opened) { CHECK(0, "byte %lu changed and still opened", i); break; }
    }
    DATA_BLOB shorter = { blob.cbData - 1, blob.pbData };
    CHECK(!unprotects(&shorter, NULL, secret, in.cbData) && GetLastError() == ERROR_INVALID_DATA, "shortened");
    CHECK(unprotects(&blob, NULL, secret, in.cbData), "unchanged still opens");
    LocalFree(blob.pbData);

    // entropy is part of the key
    static const char e1[] = "made-up entropy", e2[] = "made-up entropx";
    DATA_BLOB ent = { sizeof e1, (BYTE *)e1 }, wrong = { sizeof e2, (BYTE *)e2 };
    CHECK(CryptProtectData(&in, NULL, &ent, NULL, NULL, 0, &blob), "protect with entropy");
    CHECK(!unprotects(&blob, NULL, secret, in.cbData) && GetLastError() == ERROR_INVALID_DATA, "without the entropy");
    CHECK(!unprotects(&blob, &wrong, secret, in.cbData), "with other entropy");
    CHECK(unprotects(&blob, &ent, secret, in.cbData), "with the entropy");
    LocalFree(blob.pbData);

    // the machine scope opens too, and turning the scope into the user's is refused
    CHECK(CryptProtectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_LOCAL_MACHINE, &blob) && unprotects(&blob, NULL, secret, in.cbData), "machine");
    blob.pbData[5] ^= 1;
    CHECK(!unprotects(&blob, NULL, secret, in.cbData), "scope changed");
    LocalFree(blob.pbData);

    DATA_BLOB empty = { 0, NULL };
    CHECK(CryptProtectData(&empty, NULL, NULL, NULL, NULL, 0, &blob) && CryptUnprotectData(&blob, NULL, NULL, NULL, NULL, 0, &out) && out.cbData == 0, "empty");
    LocalFree(out.pbData);
    LocalFree(blob.pbData);

    CRYPTPROTECT_PROMPTSTRUCT prompt = { sizeof prompt, CRYPTPROTECT_PROMPT_ON_PROTECT, NULL, L"made-up prompt" };
    CHECK(!CryptProtectData(&in, NULL, NULL, NULL, &prompt, CRYPTPROTECT_UI_FORBIDDEN, &blob) && GetLastError() == ERROR_PASSWORD_RESTRICTION, "no UI");
    BYTE windows_blob[64] = { 1, 0, 0, 0, 0xd0, 0x8c, 0x9d, 0xdf, 0x01, 0x15, 0xd1, 0x11, 0x8c, 0x7a, 0x00, 0xc0, 0x4f, 0xc2, 0x97, 0xeb };
    DATA_BLOB foreign = { sizeof windows_blob, windows_blob };
    CHECK(!unprotects(&foreign, NULL, secret, in.cbData), "a blob not made here");

    // memory, in place
    BYTE mem[32], orig[32];
    for (int i = 0; i < 32; i++) orig[i] = mem[i] = (BYTE)(i * 7 + 1);
    static const DWORD scopes[] = { CRYPTPROTECTMEMORY_SAME_PROCESS, CRYPTPROTECTMEMORY_CROSS_PROCESS, CRYPTPROTECTMEMORY_SAME_LOGON };
    for (int s = 0; s < 3; s++) {
        CHECK(CryptProtectMemory(mem, 32, scopes[s]) && memcmp(mem, orig, 32), "protect memory %d", s);
        CHECK(CryptUnprotectMemory(mem, 32, scopes[s]) && !memcmp(mem, orig, 32), "unprotect memory %d", s);
    }
    CHECK(CryptProtectMemory(mem, 32, CRYPTPROTECTMEMORY_SAME_PROCESS) && CryptUnprotectMemory(mem, 32, CRYPTPROTECTMEMORY_CROSS_PROCESS) &&
          memcmp(mem, orig, 32), "the scope is part of the key");
    CHECK(!CryptProtectMemory(mem, 15, CRYPTPROTECTMEMORY_SAME_PROCESS) && GetLastError() == ERROR_INVALID_PARAMETER, "not a multiple of 16");
    CHECK(!CryptProtectMemory(mem, 16, 7) && GetLastError() == ERROR_INVALID_PARAMETER, "flags");
}

// A blob one process writes and another opens: the key outlives the process (VXRT_ROOT/dpapi/user.key).
static int across_processes(const char *what, const char *path) {
    static const char secret[] = "made-up data kept across processes";
    if (!strcmp(what, "protect")) {
        DATA_BLOB in = { sizeof secret, (BYTE *)secret }, blob;
        if (!CryptProtectData(&in, L"made-up", NULL, NULL, NULL, 0, &blob)) { printf("FAIL protect\n"); return 1; }
        FILE *f = fopen(path, "wb");
        int ok = f && fwrite(blob.pbData, 1, blob.cbData, f) == blob.cbData;
        if (f) fclose(f);
        LocalFree(blob.pbData);
        printf("crypt32 protect: %s\n", ok ? "ok" : "FAIL");
        return !ok;
    }
    BYTE buf[1024];
    FILE *f = fopen(path, "rb");
    DWORD n = f ? (DWORD)fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    DATA_BLOB blob = { n, buf };
    int ok = n && unprotects(&blob, NULL, secret, sizeof secret);
    printf("crypt32 unprotect: %s\n", ok ? "ok" : "FAIL");
    return !ok;
}

// ---- wintrust ---------------------------------------------------------------------------------------------------
static LONG verify_file(const WCHAR *path, GUID action) {
    WINTRUST_FILE_INFO file = { sizeof file, path, NULL, NULL };
    WINTRUST_DATA data = { sizeof data };
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    LONG r = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &data);
    CHECK(data.hWVTStateData == NULL || WTHelperProvDataFromStateData(data.hWVTStateData) == NULL, "no provider data");
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &data);
    return r;
}

static void test_wintrust(void) {
    GUID v2 = WINTRUST_ACTION_GENERIC_VERIFY_V2, unknown = { 0x12345678, 0x1234, 0x5678, { 1, 2, 3, 4, 5, 6, 7, 8 } };
    WCHAR self[MAX_PATH], tmp[MAX_PATH], signed_pe[MAX_PATH], text[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    LONG r = verify_file(self, v2);
    CHECK(r == (LONG)TRUST_E_NOSIGNATURE, "this unsigned test: 0x%lx", r);
    CHECK(verify_file(self, unknown) == (LONG)TRUST_E_PROVIDER_UNKNOWN, "unknown action");

    // a made-up PE that carries an attribute certificate table (not a real signature): present, so unverified, so
    // not trusted; never success
    GetTempPathW(MAX_PATH, tmp);
    swprintf(signed_pe, MAX_PATH, L"%lsvx_madeup_signed.exe", tmp);
    swprintf(text, MAX_PATH, L"%lsvx_madeup.txt", tmp);
    BYTE pe[512] = { 'M', 'Z' };
    pe[0x3c] = 0x80;
    memcpy(pe + 0x80, "PE\0\0", 4);
    pe[0x80 + 4] = 0x64; pe[0x80 + 5] = 0x86;                              // AMD64
    pe[0x80 + 20] = 240;                                                   // SizeOfOptionalHeader
    BYTE *opt = pe + 0x80 + 24;
    opt[0] = 0x0b; opt[1] = 0x02;                                          // PE32+
    opt[108] = 16;                                                         // NumberOfRvaAndSizes
    BYTE *security = opt + 112 + 4 * 8;
    security[0] = 0x00; security[1] = 0x01; security[4] = 0x08;            // 8 bytes at 0x100
    HANDLE h = CreateFileW(signed_pe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD written;
    WriteFile(h, pe, sizeof pe, &written, NULL);
    CloseHandle(h);
    r = verify_file(signed_pe, v2);
    CHECK(r == (LONG)TRUST_E_SUBJECT_NOT_TRUSTED, "a signature present is not trusted: 0x%lx", r);
    h = CreateFileW(text, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(h, "made-up text", 12, &written, NULL);
    CloseHandle(h);
    r = verify_file(text, v2);
    CHECK(r != 0, "a text file is not trusted: 0x%lx", r);
    DeleteFileW(signed_pe);
    DeleteFileW(text);

    // no catalogs
    HCATADMIN admin = NULL;
    CHECK(CryptCATAdminAcquireContext(&admin, NULL, 0) && admin, "catalog admin");
    h = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    BYTE hash[64];
    DWORD size = sizeof hash;
    BOOL hashed = CryptCATAdminCalcHashFromFileHandle(h, &size, hash, 0);
    CloseHandle(h);
    CHECK(!CryptCATAdminEnumCatalogFromHash(admin, hash, hashed ? size : 20, 0, NULL), "no catalog has it");
    CHECK(CryptCATAdminReleaseContext(admin, 0), "release");
    CHECK(!WTHelperGetProvSignerFromChain(NULL, 0, FALSE, 0) && !WTHelperGetProvCertFromChain(NULL, 0), "no signer");
}

int main(int argc, char **argv) {
    if (argc == 3) return across_processes(argv[1], argv[2]);
    root = CertCreateCertificateContext(X509_ASN_ENCODING, cert_root, sizeof cert_root);
    leaf = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, cert_leaf, sizeof cert_leaf);
    ec = CertCreateCertificateContext(X509_ASN_ENCODING, cert_ec, sizeof cert_ec);
    CHECK(root && leaf && ec, "the test certificates parse");
    if (!root || !leaf || !ec) return 1;
    test_parse();
    test_names();
    test_validity_and_usage();
    test_properties();
    test_stores();
    test_signatures();
    test_chains();
    test_dpapi();
    test_wintrust();
    CertFreeCertificateContext(root);
    CertFreeCertificateContext(leaf);
    CertFreeCertificateContext(ec);
    if (failures) { printf("crypt32: %d failures\n", failures); return 1; }
    printf("crypt32: ok\n");
    return 0;
}
