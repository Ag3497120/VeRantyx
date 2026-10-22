// Locale names and LCIDs (desktop.c, system.c): names other than the user's en-US, LOCALE_RETURN_NUMBER (Elite Dangerous
// rejected its controller presets, made for "en-GB", when that came back as text), unknown names refused.
#include <windows.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
int main(void) {
    CHECK(LocaleNameToLCID(L"en-GB", 0) == 0x809);
    CHECK(LocaleNameToLCID(L"EN-us", 0) == 0x409);
    CHECK(LocaleNameToLCID(L"ja-JP", 0) == 0x411);
    CHECK(LocaleNameToLCID(L"xx-YY", 0) == 0 && GetLastError() == ERROR_INVALID_PARAMETER);
    WCHAR name[32];
    CHECK(LCIDToLocaleName(0x809, name, 32, 0) == 6 && !wcscmp(name, L"en-GB"));
    CHECK(LCIDToLocaleName(LOCALE_USER_DEFAULT, name, 32, 0) == 6 && !wcscmp(name, L"en-US"));
    DWORD lcid = 0;
    CHECK(GetLocaleInfoEx(L"en-GB", LOCALE_RETURN_NUMBER | LOCALE_ILANGUAGE, (WCHAR *)&lcid, sizeof lcid / sizeof(WCHAR)) == 2 && lcid == 0x809);
    WCHAR text[64];
    CHECK(GetLocaleInfoEx(L"en-GB", LOCALE_SENGLISHLANGUAGENAME, text, 64) && !wcscmp(text, L"English (United Kingdom)"));
    CHECK(GetLocaleInfoW(0x809, LOCALE_SISO3166CTRYNAME, text, 64) && !wcscmp(text, L"GB"));
    CHECK(GetLocaleInfoW(0x409, LOCALE_ILANGUAGE, text, 64) && !wcscmp(text, L"0409"));
    CHECK(GetLocaleInfoEx(L"xx-YY", LOCALE_ILANGUAGE, text, 64) == 0);
    printf("locale: names, LCIDs, numbers -> %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
