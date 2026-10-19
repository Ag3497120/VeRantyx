// Managed exceptions in the CLR host: throw, catch (by class and by a derived class), finally on leave and on the way
// out of a frame, nesting, rethrow, an exception crossing a call, and one nobody catches; and isinst against an
// interface (a class lists it in InterfaceImpl; an unrelated object does not), JavaScriptSerializer.Serialize of a
// class with public fields, ProtectedData (a round trip; a wrong entropy is a CryptographicException) and the
// DateTime constructor, String.Format(string, params object[]), isinst of an array against IEnumerable, and
// Delegate.Combine / Remove with a multicast Invoke, and a P/Invoke (a DllImport method of this assembly whose DLL
// and entry point the stubs below hand out: integers and a string marshalled, the result returned). The assembly is built here
// from bytes (no compiler for it): a class Test with a static int field Count and one static method per case, each
// returning what the case should leave (or failing with "unhandled managed exception ...").
#include "dotnet_metadata.h"
#include "dotnet_exec.h"
#include "vx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "dotnet eh test failed: %s\n", m); exit(1); } } while (0)

vx_process vx_proc;
void vx_log(const char *fmt, ...) { (void)fmt; }
// the P/Invoke target: "fake.dll"!Sum(a, b, text) = a + b + strlen(text), reached through the runtime's LoadLibraryA/GetProcAddress
static WINAPI void *fake_load_library(const char *dll) { return strcmp(dll, "fake.dll") ? NULL : (void *)1; }
static WINAPI uint64_t fake_sum(uint64_t a, uint64_t b, const char *text) { return a + b + (text ? strlen(text) : 0); }
static WINAPI void *fake_get_proc(void *module, const char *name) { return module == (void *)1 && !strcmp(name, "Sum") ? (void *)fake_sum : NULL; }
void *vx_find_export(const char *dll, const char *name) {
    (void)dll;
    if (!strcmp(name, "LoadLibraryA")) return (void *)fake_load_library;
    if (!strcmp(name, "GetProcAddress")) return (void *)fake_get_proc;
    return NULL;
}
const char *vx_special_folder_path(int csidl) { (void)csidl; return NULL; }
const vx_image *vx_image_at(uint64_t pc) { (void)pc; return NULL; }
void vx_windows_path(const char *unix_path, char *out, size_t capacity) { if (out && capacity) snprintf(out, capacity, "%s", unix_path ? unix_path : ""); }
int vx_unix_path(const char *windows_path, char *out, size_t capacity) { (void)windows_path; (void)out; (void)capacity; return 0; }
int vx_unix_path_write(const char *windows_path, char *out, size_t capacity, int keep_contents) { (void)windows_path; (void)out; (void)capacity; (void)keep_contents; return 0; }
void vx_set_last_error(DWORD error) { (void)error; }
DWORD vx_get_last_error(void) { return 0; }
DWORD vx_error_from_errno(int error) { (void)error; return 1; }

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, sizeof v); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof v); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, sizeof v); }
static uint32_t align4(uint32_t n) { return (n + 3u) & ~3u; }

typedef struct { uint16_t flags; uint16_t try_offset, try_length, handler_offset, handler_length; uint32_t token; } clause;
typedef struct { const char *name; const uint8_t *code; uint32_t length; uint8_t returns_int; clause clauses[4]; int clause_count; } method;

#define EXC "\x01\x00\x00\x0a"          /* MemberRef 1: Exception::.ctor */
#define ARGEXC "\x02\x00\x00\x0a"       /* MemberRef 2: ArgumentException::.ctor */
#define MARK "\x01\x00\x00\x06"         /* MethodDef 1: Mark */
#define THROWER "\x06\x00\x00\x06"      /* MethodDef 6: Thrower */
#define COUNT "\x01\x00\x00\x04"        /* Field 1: Count */

static const uint8_t code_mark[] = { 0x7e, 1, 0, 0, 4, 0x17, 0x58, 0x80, 1, 0, 0, 4, 0x2a };
static const uint8_t code_reset[] = { 0x16, 0x80, 1, 0, 0, 4, 0x2a };
// try { throw new Exception } catch (Exception) { } return 42
static const uint8_t code_catch[] = { 0x73, 1, 0, 0, 0x0a, 0x7a, 0x26, 0xde, 0x00, 0x1f, 0x2a, 0x2a };
// try { Mark } finally { Mark }  return Count      (the leave runs the finally)
static const uint8_t code_finally[] = { 0x28, 1, 0, 0, 6, 0xde, 6, 0x28, 1, 0, 0, 6, 0xdc, 0x7e, 1, 0, 0, 4, 0x2a };
// try { try { throw } finally { Mark } } catch (Exception) { }  return Count    (the finally runs on the way to the catch)
static const uint8_t code_nested[] = { 0x73, 1, 0, 0, 0x0a, 0x7a, 0x28, 1, 0, 0, 6, 0xdc, 0x26, 0xde, 0x00, 0x7e, 1, 0, 0, 4, 0x2a };
static const uint8_t code_thrower[] = { 0x73, 1, 0, 0, 0x0a, 0x7a };
// try { Thrower() } catch (Exception) { }  return 7     (the exception crosses a call)
static const uint8_t code_cross[] = { 0x28, 6, 0, 0, 6, 0xde, 3, 0x26, 0xde, 0x00, 0x1f, 0x07, 0x2a };
// try { try { throw } catch (Exception) { rethrow } } catch (Exception) { }  return 9
static const uint8_t code_rethrow[] = { 0x73, 1, 0, 0, 0x0a, 0x7a, 0xfe, 0x1a, 0x26, 0xde, 0x00, 0x1f, 0x09, 0x2a };
// throw new Exception, nobody catches
static const uint8_t code_uncaught[] = { 0x73, 1, 0, 0, 0x0a, 0x7a };
// try { throw new Exception } catch (ArgumentException) { }  return 1     (not an ArgumentException: not caught)
static const uint8_t code_mismatch[] = { 0x73, 1, 0, 0, 0x0a, 0x7a, 0x26, 0xde, 0x00, 0x1f, 0x01, 0x2a };
// try { throw new ArgumentException } catch (Exception) { }  return 5     (an ArgumentException is an Exception)
static const uint8_t code_derived[] = { 0x73, 2, 0, 0, 0x0a, 0x7a, 0x26, 0xde, 0x00, 0x1f, 0x05, 0x2a };

// new Impl; isinst IGreeter (TypeDef 3); ldnull; cgt.un        -> 1: Impl lists IGreeter in InterfaceImpl
static const uint8_t code_iface[] = { 0x73, 0x18, 0, 0, 6, 0x75, 3, 0, 0, 2, 0x14, 0xfe, 0x03, 0x2a };
// new Exception; isinst IGreeter; ldnull; cgt.un                -> 0: it does not
static const uint8_t code_notiface[] = { 0x73, 1, 0, 0, 0x0a, 0x75, 3, 0, 0, 2, 0x14, 0xfe, 0x03, 0x2a };
static const uint8_t code_impl_ctor[] = { 0x2a };
// Sum(3, 4, "{0}{1}") through P/Invoke (MethodDef 23, no body)      -> 13
static const uint8_t code_psum[] = { 0x19, 0x1a, 0x72, 1, 0, 0, 0x70, 0x28, 0x17, 0, 0, 6, 0x2a };
// Delegate.Combine(new Action(Mark), new Action(Mark)) as Action; Invoke(); Count        -> 2 (both entries ran)
#define NEW_ACTION 0x14, 0xfe, 0x06, 1, 0, 0, 6, 0x73, 12, 0, 0, 0x0a
static const uint8_t code_combine[] = { NEW_ACTION, NEW_ACTION, 0x28, 14, 0, 0, 0x0a, 0x74, 12, 0, 0, 1, 0x6f, 13, 0, 0, 0x0a,
                                        0x7e, 1, 0, 0, 4, 0x2a };
// Delegate.Remove(Combine(a, b), new Action(Mark)) as Action; Invoke(); Count             -> 1 (one entry left)
static const uint8_t code_remove[] = { NEW_ACTION, NEW_ACTION, 0x28, 14, 0, 0, 0x0a, NEW_ACTION, 0x28, 15, 0, 0, 0x0a,
                                       0x74, 12, 0, 0, 1, 0x6f, 13, 0, 0, 0x0a, 0x7e, 1, 0, 0, 4, 0x2a };
// new byte[1] is IEnumerable (TypeRef 11)      -> 1
static const uint8_t code_array_enum[] = { 0x17, 0x8d, 4, 0, 0, 1, 0x75, 11, 0, 0, 1, 0x14, 0xfe, 0x03, 0x2a };
// String.Format("{0}{1}", new object[] { s, s }).Length where s is "{0}{1}"                -> 12 (the array's elements are the items)
static const uint8_t code_format[] = { 0x72, 1, 0, 0, 0x70, 0x18, 0x8d, 3, 0, 0, 1,
                                       0x25, 0x16, 0x72, 1, 0, 0, 0x70, 0xa2, 0x25, 0x17, 0x72, 1, 0, 0, 0x70, 0xa2,
                                       0x28, 11, 0, 0, 0x0a, 0x6f, 5, 0, 0, 0x0a, 0x2a };
// (new DateTime(1970,1,2)).Subtract(new DateTime(1970,1,1)) == one day in ticks (864000000000)     -> 1
static const uint8_t code_datetime[] = { 0x20, 0xb2, 7, 0, 0, 0x17, 0x18, 0x73, 8, 0, 0, 0x0a, 0x20, 0xb2, 7, 0, 0, 0x17, 0x17, 0x73, 8, 0, 0, 0x0a,
                                         0x28, 9, 0, 0, 0x0a, 0x21, 0x00, 0xc0, 0x69, 0x2a, 0xc9, 0x00, 0x00, 0x00, 0xfe, 0x01, 0x2a };
// new JavaScriptSerializer().Serialize(new Data()).Length       -> {"Name":null,"Flag":false} is 26 long
static const uint8_t code_serialize[] = { 0x73, 3, 0, 0, 0x0a, 0x73, 0x19, 0, 0, 6, 0x6f, 4, 0, 0, 0x0a, 0x6f, 5, 0, 0, 0x0a, 0x2a };
// Unprotect(Protect(new byte[5] { 7 })) [0]                   -> 7
static const uint8_t code_protect[] = { 0x1b, 0x8d, 4, 0, 0, 1, 0x25, 0x16, 0x1f, 0x07, 0x9c, 0x14, 0x16, 0x28, 6, 0, 0, 0x0a,
                                        0x14, 0x16, 0x28, 7, 0, 0, 0x0a, 0x16, 0x91, 0x2a };
// try { Unprotect(Protect(data), new byte[1]) ; return 1 } catch (Exception) { return 2 }   (another entropy: refused)
static const uint8_t code_entropy[] = { 0x1b, 0x8d, 4, 0, 0, 1, 0x14, 0x16, 0x28, 6, 0, 0, 0x0a, 0x17, 0x8d, 4, 0, 0, 1, 0x16, 0x28, 7, 0, 0, 0x0a,
                                        0x26, 0xde, 0x03, 0x26, 0xde, 0x02, 0x17, 0x2a, 0x18, 0x2a };

#define EXCEPTION_TYPE 0x01000001u
#define ARGUMENT_TYPE 0x01000002u
static const method methods[] = {
    { "Mark", code_mark, sizeof code_mark, 0, { { 0 } }, 0 },
    { "Reset", code_reset, sizeof code_reset, 0, { { 0 } }, 0 },
    { "Catch", code_catch, sizeof code_catch, 1, { { 0, 0, 6, 6, 3, EXCEPTION_TYPE } }, 1 },
    { "Finally", code_finally, sizeof code_finally, 1, { { 2, 0, 7, 7, 6, 0 } }, 1 },
    { "Nested", code_nested, sizeof code_nested, 1, { { 2, 0, 6, 6, 6, 0 }, { 0, 0, 12, 12, 3, EXCEPTION_TYPE } }, 2 },
    { "Thrower", code_thrower, sizeof code_thrower, 0, { { 0 } }, 0 },
    { "Cross", code_cross, sizeof code_cross, 1, { { 0, 0, 7, 7, 3, EXCEPTION_TYPE } }, 1 },
    { "Rethrow", code_rethrow, sizeof code_rethrow, 1, { { 0, 0, 6, 6, 2, EXCEPTION_TYPE }, { 0, 0, 8, 8, 3, EXCEPTION_TYPE } }, 2 },
    { "Uncaught", code_uncaught, sizeof code_uncaught, 1, { { 0 } }, 0 },
    { "Mismatch", code_mismatch, sizeof code_mismatch, 1, { { 0, 0, 6, 6, 3, ARGUMENT_TYPE } }, 1 },
    { "Derived", code_derived, sizeof code_derived, 1, { { 0, 0, 6, 6, 3, EXCEPTION_TYPE } }, 1 },
    { "Iface", code_iface, sizeof code_iface, 1, { { 0 } }, 0 },
    { "NotIface", code_notiface, sizeof code_notiface, 1, { { 0 } }, 0 },
    { "SerializeData", code_serialize, sizeof code_serialize, 1, { { 0 } }, 0 },
    { "ProtectRound", code_protect, sizeof code_protect, 1, { { 0 } }, 0 },
    { "ProtectEntropy", code_entropy, sizeof code_entropy, 1, { { 0, 0, 28, 28, 3, EXCEPTION_TYPE } }, 1 },
    { "DateTimeCtor", code_datetime, sizeof code_datetime, 1, { { 0 } }, 0 },
    { "FormatArray", code_format, sizeof code_format, 1, { { 0 } }, 0 },
    { "ArrayEnumerable", code_array_enum, sizeof code_array_enum, 1, { { 0 } }, 0 },
    { "DelegateCombine", code_combine, sizeof code_combine, 1, { { 0 } }, 0 },
    { "DelegateRemove", code_remove, sizeof code_remove, 1, { { 0 } }, 0 },
    { "PInvokeSum", code_psum, sizeof code_psum, 1, { { 0 } }, 0 },
    { "Sum", NULL, 0, 1, { { 0 } }, 0 },                                          // the DllImport: no body, MethodDef 23
    { ".ctor", code_impl_ctor, sizeof code_impl_ctor, 0, { { 0 } }, 0 },      // Impl::.ctor, MethodDef 24
    { ".ctor", code_impl_ctor, sizeof code_impl_ctor, 0, { { 0 } }, 0 },      // Data::.ctor, MethodDef 25 (last)
};
#define NMETHODS (sizeof methods / sizeof methods[0])

static const char strings[] = "\0<Module>\0Fixture\0Test\0Count\0Mark\0Reset\0Catch\0Finally\0Nested\0Thrower\0Cross\0Rethrow\0Uncaught\0Mismatch\0Derived\0Iface\0NotIface\0SerializeData\0DateTimeCtor\0FormatArray\0ArrayEnumerable\0DelegateCombine\0DelegateRemove\0PInvokeSum\0Sum\0fake.dll\0Action\0Delegate\0Invoke\0Combine\0Remove\0IEnumerable\0System.Collections\0Format\0DateTime\0TimeSpan\0Subtract\0get_TotalSeconds\0ProtectRound\0ProtectEntropy\0IGreeter\0Impl\0Data\0Name\0Flag\0.ctor\0System\0Exception\0ArgumentException\0Object\0mscorlib\0Byte\0String\0ProtectedData\0DataProtectionScope\0Protect\0Unprotect\0System.Security.Cryptography\0System.Security\0JavaScriptSerializer\0Serialize\0get_Length\0System.Web.Script.Serialization\0System.Web.Extensions\0";
static uint16_t sidx(const char *value) {
    for (const char *entry = strings + 1; *entry || entry < strings + sizeof strings - 1; entry += strlen(entry) + 1)
        if (!strcmp(entry, value)) return (uint16_t)(entry - strings);
    CHECK(0, "a fixture string is missing");
    return 0;
}

static uint8_t *make_assembly(size_t *length) {
    const size_t size = 0x1800;
    uint8_t *image = calloc(1, size);
    CHECK(image, "allocation failed");
    image[0] = 'M'; image[1] = 'Z'; put32(image + 0x3c, 0x80);
    memcpy(image + 0x80, "PE\0\0", 4);
    uint8_t *coff = image + 0x84, *opt = image + 0x98;
    put16(coff, 0x014c); put16(coff + 2, 1); put16(coff + 16, 0xe0); put16(coff + 18, 0x0102);
    put16(opt, 0x010b); put32(opt + 16, 0x1000); put32(opt + 28, 0x400000);
    put32(opt + 56, 0x3000); put32(opt + 60, 0x200); put32(opt + 92, 16);
    put32(opt + 96 + 14 * 8, 0x1000); put32(opt + 96 + 14 * 8 + 4, 72);
    uint8_t *section = image + 0x178;
    put32(section + 8, (uint32_t)size - 0x200); put32(section + 12, 0x1000);
    put32(section + 16, (uint32_t)size - 0x200); put32(section + 20, 0x200);

    // method bodies from file offset 0xa00, 4-aligned; fat headers when there are clauses (or locals), else tiny
    uint32_t body_rva[NMETHODS];
    uint32_t at = 0xa00;
    for (size_t i = 0; i < NMETHODS; i++) {
        at = align4(at);
        body_rva[i] = 0x1000 + (at - 0x200);
        const method *m = &methods[i];
        if (!m->code) { body_rva[i] = 0; continue; }                 // a P/Invoke has no body
        if (!m->clause_count) {
            CHECK(m->length < 64, "tiny body too long");
            image[at++] = (uint8_t)(m->length << 2 | 2);
            memcpy(image + at, m->code, m->length); at += m->length;
        } else {
            put16(image + at, 0x301b);                       // fat, more sections, init locals, header 3 dwords
            put16(image + at + 2, 4);                        // max stack
            put32(image + at + 4, m->length);
            put32(image + at + 8, 0);                        // no locals
            at += 12;
            memcpy(image + at, m->code, m->length); at = align4(at + m->length);
            image[at] = 0x01;                                // an exception table, small format
            image[at + 1] = (uint8_t)(4 + 12 * m->clause_count);
            at += 4;
            for (int k = 0; k < m->clause_count; k++) {
                const clause *c = &m->clauses[k];
                put16(image + at, c->flags); put16(image + at + 2, c->try_offset); image[at + 4] = (uint8_t)c->try_length;
                put16(image + at + 5, c->handler_offset); image[at + 7] = (uint8_t)c->handler_length; put32(image + at + 8, c->token);
                at += 12;
            }
        }
    }
    CHECK(at < 0x1700, "bodies overflow the fixture");

    uint8_t *cor = image + 0x200;
    put32(cor, 72); put16(cor + 4, 2); put16(cor + 6, 5);
    put32(cor + 8, 0x1100); put16(cor + 16, 1); put32(cor + 20, 0x06000003);
    uint8_t *root = image + 0x300;
    put32(root, 0x424a5342); put16(root + 4, 1); put16(root + 6, 1);
    put32(root + 12, 12); memcpy(root + 16, "v4.0.30319", 10);
    put16(root + 28, 0); put16(root + 30, 5);

    // blobs: 0 empty; 1 int32(); 5 void(); 9 instance void(); 13 field int32
    // 16: field string; 19: field bool; 22: instance string(object); 27: instance int32(); 31: static byte[](byte[], byte[], enum)
    static const uint8_t blob[] = { 0, 3, 0, 0, 8, 3, 0, 0, 1, 3, 0x20, 0, 1, 2, 6, 8,
                                    2, 6, 0x0e, 2, 6, 0x02, 4, 0x20, 1, 0x0e, 0x1c, 3, 0x20, 0, 8,
                                    10, 0, 3, 0x1d, 5, 0x1d, 5, 0x1d, 5, 0x11, (6 << 2) | 1,
                                    6, 0x20, 3, 1, 8, 8, 8,                       // 42: instance void(int, int, int)
                                    6, 0x20, 1, 0x11, (10 << 2) | 1, 0x11, (9 << 2) | 1,   // 49: instance TimeSpan(DateTime)
                                    3, 0x20, 0, 0x0d,                            // 56: instance float64()
                                    6, 0, 2, 0x0e, 0x0e, 0x1d, 0x1c,             // 60: static string(string, object[])
                                    5, 0x20, 2, 1, 0x1c, 0x18,                   // 67: instance void(object, native int)
                                    8, 0, 2, 0x12, (13 << 2) | 1, 0x12, (13 << 2) | 1, 0x12, (13 << 2) | 1,     // 73: static Delegate(Delegate, Delegate)
                                    6, 0, 3, 8, 8, 8, 0x0e };                    // 82: static int32(int32, int32, string)
    const uint8_t sig_int = 1, sig_void = 5, sig_ctor = 9, sig_field = 13, sig_field_string = 16, sig_field_bool = 19,
                  sig_serialize = 22, sig_length = 27, sig_protect = 31, sig_ctor_iii = 42, sig_subtract = 49, sig_total = 56, sig_format = 60, sig_delegate_ctor = 67, sig_combine = 73, sig_sum = 82;

    uint8_t tb[2048] = { 0 };
    tb[4] = 2; tb[7] = 1;
    const int present[] = { 0, 1, 2, 4, 6, 9, 10, 26, 28, 32, 35 };
    uint64_t valid = 0;
    for (size_t i = 0; i < sizeof present / sizeof present[0]; i++) valid |= 1ull << present[i];
    put64(tb + 8, valid);
    const uint32_t counts[] = { 1, 13, 5, 3, NMETHODS, 1, 15, 1, 1, 1, 3 };
    size_t p = 24;
    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) { put32(tb + p, counts[i]); p += 4; }
    // Module
    put16(tb + p + 2, sidx("<Module>")); put16(tb + p + 4, 1); p += 10;
    // TypeRef: ResolutionScope (AssemblyRef 1 = 6), name, namespace
    put16(tb + p, 6); put16(tb + p + 2, sidx("Exception")); put16(tb + p + 4, sidx("System")); p += 6;
    put16(tb + p, 6); put16(tb + p + 2, sidx("ArgumentException")); put16(tb + p + 4, sidx("System")); p += 6;
    put16(tb + p, 6); put16(tb + p + 2, sidx("Object")); put16(tb + p + 4, sidx("System")); p += 6;
    put16(tb + p, 6); put16(tb + p + 2, sidx("Byte")); put16(tb + p + 4, sidx("System")); p += 6;                       // 4
    put16(tb + p, 10); put16(tb + p + 2, sidx("ProtectedData")); put16(tb + p + 4, sidx("System.Security.Cryptography")); p += 6;       // 5
    put16(tb + p, 10); put16(tb + p + 2, sidx("DataProtectionScope")); put16(tb + p + 4, sidx("System.Security.Cryptography")); p += 6; // 6
    put16(tb + p, 14); put16(tb + p + 2, sidx("JavaScriptSerializer")); put16(tb + p + 4, sidx("System.Web.Script.Serialization")); p += 6; // 7
    put16(tb + p, 6); put16(tb + p + 2, sidx("String")); put16(tb + p + 4, sidx("System")); p += 6;                     // 8
    put16(tb + p, 6); put16(tb + p + 2, sidx("DateTime")); put16(tb + p + 4, sidx("System")); p += 6;                   // 9
    put16(tb + p, 6); put16(tb + p + 2, sidx("TimeSpan")); put16(tb + p + 4, sidx("System")); p += 6;                   // 10
    put16(tb + p, 6); put16(tb + p + 2, sidx("IEnumerable")); put16(tb + p + 4, sidx("System.Collections")); p += 6;    // 11
    put16(tb + p, 6); put16(tb + p + 2, sidx("Action")); put16(tb + p + 4, sidx("System")); p += 6;                     // 12
    put16(tb + p, 6); put16(tb + p + 2, sidx("Delegate")); put16(tb + p + 4, sidx("System")); p += 6;                   // 13
    // TypeDef: <Module>, Fixture.Test (all the static methods but the last), Fixture.IGreeter (interface), Fixture.Impl (its .ctor)
    put32(tb + p, 0); put16(tb + p + 4, sidx("<Module>")); put16(tb + p + 6, 0); put16(tb + p + 8, 0); put16(tb + p + 10, 1); put16(tb + p + 12, 1); p += 14;
    put32(tb + p, 1); put16(tb + p + 4, sidx("Test")); put16(tb + p + 6, sidx("Fixture")); put16(tb + p + 8, (3 << 2) | 1); put16(tb + p + 10, 1); put16(tb + p + 12, 1); p += 14;
    put32(tb + p, 0xa1); put16(tb + p + 4, sidx("IGreeter")); put16(tb + p + 6, sidx("Fixture")); put16(tb + p + 8, 0); put16(tb + p + 10, 2); put16(tb + p + 12, NMETHODS - 1); p += 14;
    put32(tb + p, 1); put16(tb + p + 4, sidx("Impl")); put16(tb + p + 6, sidx("Fixture")); put16(tb + p + 8, (3 << 2) | 1); put16(tb + p + 10, 2); put16(tb + p + 12, NMETHODS - 1); p += 14;
    put32(tb + p, 1); put16(tb + p + 4, sidx("Data")); put16(tb + p + 6, sidx("Fixture")); put16(tb + p + 8, (3 << 2) | 1); put16(tb + p + 10, 2); put16(tb + p + 12, NMETHODS); p += 14;
    // Field: public static int Count
    put16(tb + p, 0x16); put16(tb + p + 2, sidx("Count")); put16(tb + p + 4, sig_field); p += 6;
    put16(tb + p, 0x06); put16(tb + p + 2, sidx("Name")); put16(tb + p + 4, sig_field_string); p += 6;       // Data.Name
    put16(tb + p, 0x06); put16(tb + p + 2, sidx("Flag")); put16(tb + p + 4, sig_field_bool); p += 6;         // Data.Flag
    // MethodDef: public static
    for (size_t i = 0; i < NMETHODS; i++) {
        const int is_ctor = !strcmp(methods[i].name, ".ctor"), is_pinvoke = !methods[i].code;
        put32(tb + p, body_rva[i]); put16(tb + p + 4, 0); put16(tb + p + 6, is_ctor ? 0x1886 : is_pinvoke ? 0x2093 : 0x16);
        put16(tb + p + 8, sidx(methods[i].name)); put16(tb + p + 10, is_ctor ? sig_ctor : is_pinvoke ? sig_sum : methods[i].returns_int ? sig_int : sig_void); put16(tb + p + 12, 1);
        p += 14;
    }
    // InterfaceImpl: Impl (TypeDef 4) implements IGreeter (TypeDef 3: (3 << 2) | 0)
    put16(tb + p, 4); put16(tb + p + 2, 3 << 2); p += 4;
    // MemberRef: Exception::.ctor (class TypeRef 1 = (1<<3)|1), ArgumentException::.ctor (TypeRef 2)
    put16(tb + p, (1 << 3) | 1); put16(tb + p + 2, sidx(".ctor")); put16(tb + p + 4, sig_ctor); p += 6;
    put16(tb + p, (2 << 3) | 1); put16(tb + p + 2, sidx(".ctor")); put16(tb + p + 4, sig_ctor); p += 6;
    put16(tb + p, (7 << 3) | 1); put16(tb + p + 2, sidx(".ctor")); put16(tb + p + 4, sig_ctor); p += 6;               // 3 JavaScriptSerializer
    put16(tb + p, (7 << 3) | 1); put16(tb + p + 2, sidx("Serialize")); put16(tb + p + 4, sig_serialize); p += 6;     // 4
    put16(tb + p, (8 << 3) | 1); put16(tb + p + 2, sidx("get_Length")); put16(tb + p + 4, sig_length); p += 6;       // 5 String
    put16(tb + p, (5 << 3) | 1); put16(tb + p + 2, sidx("Protect")); put16(tb + p + 4, sig_protect); p += 6;         // 6 ProtectedData
    put16(tb + p, (5 << 3) | 1); put16(tb + p + 2, sidx("Unprotect")); put16(tb + p + 4, sig_protect); p += 6;       // 7
    put16(tb + p, (9 << 3) | 1); put16(tb + p + 2, sidx(".ctor")); put16(tb + p + 4, sig_ctor_iii); p += 6;             // 8 DateTime
    put16(tb + p, (9 << 3) | 1); put16(tb + p + 2, sidx("Subtract")); put16(tb + p + 4, sig_subtract); p += 6;          // 9
    put16(tb + p, (10 << 3) | 1); put16(tb + p + 2, sidx("get_TotalSeconds")); put16(tb + p + 4, sig_total); p += 6;     // 10 TimeSpan
    put16(tb + p, (8 << 3) | 1); put16(tb + p + 2, sidx("Format")); put16(tb + p + 4, sig_format); p += 6;             // 11 String
    put16(tb + p, (12 << 3) | 1); put16(tb + p + 2, sidx(".ctor")); put16(tb + p + 4, sig_delegate_ctor); p += 6;      // 12 Action
    put16(tb + p, (12 << 3) | 1); put16(tb + p + 2, sidx("Invoke")); put16(tb + p + 4, sig_ctor); p += 6;              // 13
    put16(tb + p, (13 << 3) | 1); put16(tb + p + 2, sidx("Combine")); put16(tb + p + 4, sig_combine); p += 6;          // 14 Delegate
    put16(tb + p, (13 << 3) | 1); put16(tb + p + 2, sidx("Remove")); put16(tb + p + 4, sig_combine); p += 6;           // 15
    // ModuleRef: fake.dll; ImplMap: Sum (MethodDef 23) -> fake.dll!Sum, winapi + ANSI
    put16(tb + p, sidx("fake.dll")); p += 2;
    put16(tb + p, 0x0102); put16(tb + p + 2, (23 << 1) | 1); put16(tb + p + 4, sidx("Sum")); put16(tb + p + 6, 1); p += 8;
    // Assembly, AssemblyRef (mscorlib)
    put32(tb + p, 0); put16(tb + p + 4, 1); put16(tb + p + 18, sidx("Fixture")); p += 22;
    put16(tb + p, 4); put16(tb + p + 14, sidx("mscorlib")); p += 20;
    put16(tb + p, 4); put16(tb + p + 14, sidx("System.Security")); p += 20;
    put16(tb + p, 4); put16(tb + p + 14, sidx("System.Web.Extensions")); p += 20;

    const uint32_t table_offset = 108, table_size = (uint32_t)p;
    const uint32_t strings_offset = align4(table_offset + table_size);
    const uint32_t blob_offset = align4(strings_offset + (uint32_t)sizeof strings);
    const uint32_t guid_offset = align4(blob_offset + (uint32_t)sizeof blob);
    const uint32_t us_offset = align4(guid_offset + 16);
    // #US: offset 1 is the string "{0}{1}" (length byte 13 = 6 characters of 2 bytes + the trailing flag byte)
    const uint8_t user_strings[16] = { 0, 13, '{', 0, '0', 0, '}', 0, '{', 0, '1', 0, '}', 0, 0, 0 };
    uint32_t cursor = 32;
    struct { const char *name; uint32_t offset, size; } streams[] = {
        { "#~", table_offset, table_size }, { "#Strings", strings_offset, sizeof strings }, { "#Blob", blob_offset, sizeof blob },
        { "#GUID", guid_offset, 16 }, { "#US", us_offset, sizeof user_strings } };
    for (size_t i = 0; i < 5; i++) {
        put32(root + cursor, streams[i].offset); put32(root + cursor + 4, streams[i].size);
        size_t n = strlen(streams[i].name) + 1;
        memcpy(root + cursor + 8, streams[i].name, n);
        cursor += 8 + align4((uint32_t)n);
    }
    CHECK(cursor == table_offset, "stream headers have the wrong size");
    memcpy(root + table_offset, tb, table_size);
    memcpy(root + strings_offset, strings, sizeof strings);
    memcpy(root + blob_offset, blob, sizeof blob);
    memset(root + guid_offset, 0x5a, 16);
    memcpy(root + us_offset, user_strings, sizeof user_strings);
    put32(cor + 12, us_offset + (uint32_t)sizeof user_strings);          // the metadata's size
    CHECK(0x300 + us_offset + sizeof user_strings < 0xa00, "metadata overflows into the bodies");
    *length = size;
    return image;
}

static int run(const vx_dotnet_metadata *md, uint32_t method_index, int64_t *value, char *error, size_t capacity) {
    return vx_dotnet_execute_entry(md, 0x06000000u | (method_index + 1), value, error, capacity);
}

int main(void) {
    size_t length;
    uint8_t *image = make_assembly(&length);
    vx_pe_info pe;
    vx_dotnet_metadata md;
    CHECK(vx_pe_inspect(image, length, &pe) && pe.has_cli, "the fixture is not a CLI image");
    CHECK(vx_dotnet_metadata_open(image, length, &pe, &md), "the fixture's metadata does not open");
    static const struct { const char *name; int ok; int64_t value; const char *message; } cases[] = {
        { "Catch", 1, 42, NULL }, { "Finally", 1, 2, NULL }, { "Nested", 1, 1, NULL }, { "Cross", 1, 7, NULL },
        { "Rethrow", 1, 9, NULL }, { "Uncaught", 0, 0, "unhandled managed exception System.Exception" },
        { "Mismatch", 0, 0, "unhandled managed exception System.Exception" }, { "Derived", 1, 5, NULL },
        { "Iface", 1, 1, NULL }, { "NotIface", 1, 0, NULL },
        { "SerializeData", 1, 26, NULL }, { "ProtectRound", 1, 7, NULL }, { "ProtectEntropy", 1, 2, NULL },
        { "DateTimeCtor", 1, 1, NULL }, { "FormatArray", 1, 12, NULL }, { "ArrayEnumerable", 1, 1, NULL },
        { "DelegateCombine", 1, 2, NULL }, { "DelegateRemove", 1, 1, NULL }, { "PInvokeSum", 1, 13, NULL },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        size_t index = 0;
        for (; index < NMETHODS && strcmp(methods[index].name, cases[i].name); index++) {}
        int64_t value = -1;
        char error[256] = "";
        int ok = run(&md, (uint32_t)index, &value, error, sizeof error);
        int good = ok == cases[i].ok && (!ok ? strstr(error, cases[i].message) != NULL : value == cases[i].value);
        printf("  %-9s %s (%s %lld%s%s)\n", cases[i].name, good ? "ok" : "FAILED", ok ? "returned" : "failed:", (long long)value,
               ok ? "" : " ", ok ? "" : error);
        failed += !good;
    }
    printf("dotnet exceptions: catch, finally on leave and on unwinding, nesting, a call boundary, rethrow, a derived class, "
           "unhandled and mismatched, isinst against an interface, Serialize of a data class, ProtectedData, DateTime constructor, String.Format with an array, an array as IEnumerable, multicast delegates, P/Invoke: %s\n", failed ? "FAILED" : "ok");
    free(image);
    return failed ? 1 : 0;
}
