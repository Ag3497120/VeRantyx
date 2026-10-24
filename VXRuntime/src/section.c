// ntdll's NtQuerySection: what a section (a file mapping object, CreateFileMapping in src/files.c) is. Chromium (the
// Windows Steam client's steamwebhelper) looks it up with GetProcAddress and calls it on its shared memory, without
// checking that it was found. Two classes:
//   SectionBasicInformation (0): SECTION_BASIC_INFORMATION { PVOID BaseAddress; ULONG AllocationAttributes;
//     LARGE_INTEGER MaximumSize; } (24 bytes): no base address (that is only for based sections), the SEC_* attributes
//     the section was made with (SEC_COMMIT or SEC_RESERVE for paging-file memory, SEC_FILE for a file's), its size;
//   SectionImageInformation (1): only for SEC_IMAGE sections; a data section answers STATUS_SECTION_NOT_IMAGE. This
//     runtime maps SEC_IMAGE sections as plain files, so it has no image facts to give: STATUS_NOT_IMPLEMENTED.
#include "vx.h"

#define STATUS_SUCCESS 0
#define STATUS_NOT_IMPLEMENTED 0xC0000002u
#define STATUS_INVALID_INFO_CLASS 0xC0000003u
#define STATUS_INFO_LENGTH_MISMATCH 0xC0000004u
#define STATUS_ACCESS_VIOLATION 0xC0000005u
#define STATUS_INVALID_HANDLE 0xC0000008u
#define STATUS_OBJECT_TYPE_MISMATCH 0xC0000024u
#define STATUS_SECTION_NOT_IMAGE 0xC0000049u
#define SEC_IMAGE 0x1000000u

int vx_mapping_info(HANDLE h, uint64_t *size, DWORD *attributes);   // files.c

static WINAPI DWORD NtQuerySection(HANDLE h, int cls, void *info, size_t length, size_t *returned) {
    uint64_t size; DWORD attributes;
    if (!vx_mapping_info(h, &size, &attributes))
        return vx_handle_object(h, NULL, NULL, NULL, NULL, 0, NULL) ? STATUS_OBJECT_TYPE_MISMATCH : STATUS_INVALID_HANDLE;
    switch (cls) {
    case 0: {                                              // SectionBasicInformation
        if (length < 24) return STATUS_INFO_LENGTH_MISMATCH;
        if (!info) return STATUS_ACCESS_VIOLATION;
        uint8_t *b = info;
        *(uint64_t *)b = 0;
        *(uint32_t *)(b + 8) = attributes; *(uint32_t *)(b + 12) = 0;
        *(uint64_t *)(b + 16) = size;
        if (returned) *returned = 24;
        return STATUS_SUCCESS;
    }
    case 1:                                                // SectionImageInformation
        if (!(attributes & SEC_IMAGE)) return STATUS_SECTION_NOT_IMAGE;
        vx_log("NtQuerySection: image facts of a SEC_IMAGE section are not available (mapped as a plain file)");
        return STATUS_NOT_IMPLEMENTED;
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

static const vx_export ntdll[] = { VX_EXPORT(NtQuerySection), { "ZwQuerySection", (void *)NtQuerySection } };

void vx_section_init(void) {
    vx_register_module("ntdll.dll", ntdll, sizeof ntdll / sizeof ntdll[0]);
}

#ifdef __aarch64__
#include "thunks/section.c.inc"
#endif
