// msxml: MSXML's DOM, the DOMDocument a program makes through CoCreateInstance, for reading and writing XML (games
// and launchers keep settings and layouts in it). In-memory tree, parsed by a small reader (elements, attributes,
// text, CDATA, comments, processing
// instructions, the five named entities and numeric ones; no DTDs or namespaces), written back by save and xml.
// A subset of XPath for selectNodes and selectSingleNode: steps of names, * , . , .. , @name, text(), node(),
// // for descendants, [n], [@a], [@a='v'] and [name='v'] predicates.
//
// Interfaces, by the vtable order of msxml.h: IXMLDOMDocument2 (on IXMLDOMDocument, IXMLDOMNode, IDispatch),
// IXMLDOMElement, IXMLDOMAttribute, IXMLDOMText (on IXMLDOMCharacterData), IXMLDOMNodeList, IXMLDOMNamedNodeMap and
// IXMLDOMParseError. Every node belongs to its document, which owns the memory: a node keeps the document alive
// while referenced, and nodes removed from the tree stay allocated until the document goes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "vx.h"

typedef int32_t HRESULT;
typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
typedef int16_t VARIANT_BOOL;
#define VB_TRUE ((VARIANT_BOOL)-1)
#define S_OK 0
#define S_FALSE 1
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_POINTER ((HRESULT)0x80004003)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_FAIL ((HRESULT)0x80004005)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)

enum { VT_EMPTY = 0, VT_NULL = 1, VT_I2 = 2, VT_I4 = 3, VT_R4 = 4, VT_R8 = 5, VT_BSTR = 8, VT_DISPATCH = 9, VT_BOOL = 11,
       VT_UNKNOWN = 13, VT_I1 = 16, VT_UI1 = 17, VT_UI2 = 18, VT_UI4 = 19, VT_I8 = 20, VT_UI8 = 21, VT_INT = 22, VT_UINT = 23,
       VT_BYREF = 0x4000, VT_VARIANT = 12 };
typedef struct { uint16_t vt, r1, r2, r3; union { int64_t i8; int32_t i4; double r8; float r4; int16_t boolean; WCHAR *bstr; void *ptr; } v; uint64_t extra; } VARIANT;

enum { NODE_ELEMENT = 1, NODE_ATTRIBUTE = 2, NODE_TEXT = 3, NODE_CDATA_SECTION = 4, NODE_PROCESSING_INSTRUCTION = 7,
       NODE_COMMENT = 8, NODE_DOCUMENT = 9 };

static const GUID IID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IDispatch = { 0x00020400, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IXMLDOMNode = { 0x2933BF80, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMDocument = { 0x2933BF81, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMNodeList = { 0x2933BF82, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMNamedNodeMap = { 0x2933BF83, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMCharacterData = { 0x2933BF84, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMAttribute = { 0x2933BF85, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMElement = { 0x2933BF86, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMText = { 0x2933BF87, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMComment = { 0x2933BF88, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMProcessingInstruction = { 0x2933BF89, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMCDATASection = { 0x2933BF8A, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMDocument2 = { 0x2933BF95, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMDocument3 = { 0x2933BF96, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID IID_IXMLDOMParseError = { 0x3EFAA426, 0x272F, 0x11D2, { 0x83, 0x6F, 0x00, 0x00, 0xF8, 0x7A, 0x77, 0x82 } };
static const GUID CLSID_DOMDocument = { 0x2933BF90, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID CLSID_DOMDocument26 = { 0xF5078F1B, 0xC551, 0x11D3, { 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21 } };
static const GUID CLSID_DOMDocument30 = { 0xF5078F32, 0xC551, 0x11D3, { 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21 } };
static const GUID CLSID_DOMDocument40 = { 0x88D969C0, 0xF192, 0x11D4, { 0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5 } };
static const GUID CLSID_DOMDocument60 = { 0x88D96A05, 0xF192, 0x11D4, { 0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5 } };
static const GUID CLSID_FreeThreadedDOMDocument = { 0x2933BF91, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID CLSID_FreeThreadedDOMDocument30 = { 0xF5078F33, 0xC551, 0x11D3, { 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21 } };
static const GUID CLSID_FreeThreadedDOMDocument60 = { 0x88D96A06, 0xF192, 0x11D4, { 0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5 } };
static int same_guid(const GUID *a, const GUID *b) { return a && b && !memcmp(a, b, sizeof *a); }

void vx_com_register_class(const void *clsid, HRESULT (*create)(const void *iid, void **out));
int vx_unix_path(const char *win, char *out, size_t cap);
int vx_unix_path_write(const char *win, char *out, size_t cap, int keep_contents);

static int trace_xml(void) { static int on = -1; if (on < 0) on = getenv("VXRT_TRACE_XML") != NULL; return on; }

// ---- BSTRs, as oleaut32 (com.c) lays them out: a byte count before the characters, a terminator after -------------------
static WCHAR *bstr_from_utf16(const WCHAR *s, size_t n) {
    uint8_t *p = malloc(4 + n * 2 + 2);
    if (!p) return NULL;
    *(uint32_t *)p = (uint32_t)(n * 2);
    if (s) memcpy(p + 4, s, n * 2);
    p[4 + n * 2] = p[5 + n * 2] = 0;
    return (WCHAR *)(p + 4);
}
static WCHAR *bstr(const WCHAR *s) { return bstr_from_utf16(s, s ? vx_wcslen(s) : 0); }
static WCHAR *bstr_ascii(const char *s) {
    size_t n = strlen(s);
    WCHAR *b = bstr_from_utf16(NULL, n);
    if (b) for (size_t i = 0; i < n; i++) b[i] = (WCHAR)(uint8_t)s[i];
    return b;
}
static void bstr_free(WCHAR *b) { if (b) free((uint8_t *)b - 4); }
static uint32_t bstr_len(const WCHAR *b) { return b ? *(const uint32_t *)((const uint8_t *)b - 4) / 2 : 0; }
static WCHAR *wdup(const WCHAR *s, size_t n) {
    WCHAR *d = malloc((n + 1) * 2);
    if (!d) return NULL;
    memcpy(d, s, n * 2);
    d[n] = 0;
    return d;
}
static int weq(const WCHAR *a, const WCHAR *b) {
    if (!a || !b) return a == b;
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}
static int weq_ascii(const WCHAR *w, size_t n, const char *a) {
    for (size_t i = 0; i < n; i++) if (!a[i] || w[i] != (WCHAR)(uint8_t)a[i]) return 0;
    return !a[n];
}
static void variant_init(VARIANT *v) { memset(v, 0, sizeof *v); }
static WCHAR *variant_text(const VARIANT *v, char *scratch, size_t cap) {      // a BSTR or a number as text (not owned)
    if (!v) return NULL;
    if (v->vt == (VT_BYREF | VT_VARIANT)) return variant_text((const VARIANT *)v->v.ptr, scratch, cap);
    if (v->vt == VT_BSTR) return v->v.bstr;
    if (v->vt == (VT_BYREF | VT_BSTR)) return v->v.ptr ? *(WCHAR **)v->v.ptr : NULL;
    switch (v->vt) {
    case VT_I2: snprintf(scratch, cap, "%d", (int)v->v.boolean); break;
    case VT_I4: case VT_INT: snprintf(scratch, cap, "%d", v->v.i4); break;
    case VT_UI4: case VT_UINT: snprintf(scratch, cap, "%u", (unsigned)v->v.i4); break;
    case VT_I8: snprintf(scratch, cap, "%lld", (long long)v->v.i8); break;
    case VT_R4: snprintf(scratch, cap, "%g", (double)v->v.r4); break;
    case VT_R8: snprintf(scratch, cap, "%g", v->v.r8); break;
    case VT_BOOL: snprintf(scratch, cap, "%s", v->v.boolean ? "true" : "false"); break;
    case VT_UI1: case VT_I1: snprintf(scratch, cap, "%d", (int)(int8_t)v->v.i4); break;
    default: return NULL;
    }
    static __thread WCHAR wide[64];
    size_t n = vx_utf8_to_utf16(scratch, strlen(scratch), wide, 63);
    wide[n] = 0;
    return wide;
}

// ---- the tree ----------------------------------------------------------------------------------------------------------------
typedef struct xml_doc xml_doc;
typedef struct xml_node xml_node;
struct xml_node {
    const void **vtbl;                  // the node's COM identity: an element, attribute, text, ... vtable
    xml_doc *doc;
    int type;
    WCHAR *name, *value;                // element: tag; attribute: name and value; text/cdata/comment: value; pi: target and data
    xml_node *parent, *first, *last, *prev, *next;
    xml_node *attrs;                    // elements: a singly linked list of attribute nodes (their next)
    xml_node *all_next;                 // every node of the document, for freeing
};
struct xml_doc {
    const void **vtbl;
    LONG refs;
    xml_node *root;                     // the document node (NODE_DOCUMENT); its children: the declaration, comments, the element
    xml_node *all;
    WCHAR *url;
    int async, validate, resolve_externals, preserve_whitespace, parsed;
    LONG error_code, error_line, error_pos;
    WCHAR *error_reason;
};
typedef struct { const void **vtbl; LONG refs; xml_doc *doc; xml_node **items; LONG count, cursor; int attributes; xml_node *owner; } xml_list;
typedef struct { const void **vtbl; LONG refs; xml_doc *doc; } xml_error;

static const void *g_doc_vtbl[84], *g_element_vtbl[52], *g_attr_vtbl[46], *g_text_vtbl[52], *g_other_vtbl[51];
static const void *g_list_vtbl[12], *g_map_vtbl[17], *g_error_vtbl[14];

static void doc_addref(xml_doc *d) { __atomic_add_fetch(&d->refs, 1, __ATOMIC_ACQ_REL); }
static void doc_free(xml_doc *d) {
    for (xml_node *n = d->all; n;) { xml_node *next = n->all_next; free(n->name); free(n->value); free(n); n = next; }
    free(d->url); free(d->error_reason); free(d);
}
static void doc_release(xml_doc *d) { if (!__atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL)) doc_free(d); }

static xml_node *node_new(xml_doc *d, int type, const WCHAR *name, size_t name_len, const WCHAR *value, size_t value_len) {
    xml_node *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->doc = d; n->type = type;
    n->vtbl = type == NODE_ELEMENT ? g_element_vtbl : type == NODE_ATTRIBUTE ? g_attr_vtbl : type == NODE_TEXT || type == NODE_CDATA_SECTION ? g_text_vtbl : g_other_vtbl;
    if (type == NODE_DOCUMENT) n->vtbl = NULL;         // (the document object stands for it)
    n->name = name ? wdup(name, name_len) : NULL;
    n->value = value ? wdup(value, value_len) : NULL;
    n->all_next = d->all; d->all = n;
    return n;
}
static void node_append(xml_node *parent, xml_node *child) {
    child->parent = parent; child->next = NULL; child->prev = parent->last;
    if (parent->last) parent->last->next = child; else parent->first = child;
    parent->last = child;
}
static void node_unlink(xml_node *n) {
    if (!n->parent) return;
    if (n->prev) n->prev->next = n->next; else n->parent->first = n->next;
    if (n->next) n->next->prev = n->prev; else n->parent->last = n->prev;
    n->parent = NULL; n->prev = n->next = NULL;
}
static xml_node *attr_find(const xml_node *e, const WCHAR *name) {
    for (xml_node *a = e->attrs; a; a = a->next) if (weq(a->name, name)) return a;
    return NULL;
}
static void attr_set(xml_node *e, const WCHAR *name, const WCHAR *value, size_t value_len) {
    xml_node *a = attr_find(e, name);
    if (a) { free(a->value); a->value = wdup(value, value_len); return; }
    a = node_new(e->doc, NODE_ATTRIBUTE, name, vx_wcslen(name), value, value_len);
    if (!a) return;
    a->parent = e;
    if (!e->attrs) e->attrs = a;
    else { xml_node *t = e->attrs; while (t->next) t = t->next; t->next = a; a->prev = t; }
}
static xml_node *doc_element(const xml_doc *d) {
    for (xml_node *n = d->root ? d->root->first : NULL; n; n = n->next) if (n->type == NODE_ELEMENT) return n;
    return NULL;
}

// ---- the reader ----------------------------------------------------------------------------------------------------------------
typedef struct { const WCHAR *s; size_t n, i; xml_doc *doc; LONG line; } reader;
static int at(reader *r, const char *lit) {
    size_t k = strlen(lit);
    if (r->i + k > r->n) return 0;
    for (size_t j = 0; j < k; j++) if (r->s[r->i + j] != (WCHAR)(uint8_t)lit[j]) return 0;
    return 1;
}
static void fail(reader *r, const char *why) {
    if (r->doc->error_code) return;
    r->doc->error_code = (LONG)0xC00CE508; r->doc->error_line = r->line; r->doc->error_pos = (LONG)r->i;
    r->doc->error_reason = malloc((strlen(why) + 1) * 2);
    if (r->doc->error_reason) { size_t n = vx_utf8_to_utf16(why, strlen(why), r->doc->error_reason, strlen(why)); r->doc->error_reason[n] = 0; }
}
static int is_space(WCHAR c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int is_name(WCHAR c) { return c == ':' || c == '_' || c == '-' || c == '.' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80; }
static void skip_space(reader *r) { while (r->i < r->n && is_space(r->s[r->i])) { if (r->s[r->i] == '\n') r->line++; r->i++; } }
// Entities decoded into a new buffer (n characters in, the count out).
static WCHAR *decode(const WCHAR *s, size_t n, size_t *out_n) {
    WCHAR *d = malloc((n + 1) * 2);
    if (!d) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] != '&') { d[o++] = s[i++]; continue; }
        size_t j = i + 1;
        while (j < n && s[j] != ';' && j - i < 12) j++;
        if (j >= n || s[j] != ';') { d[o++] = s[i++]; continue; }
        size_t len = j - i - 1;
        const WCHAR *e = s + i + 1;
        uint32_t code = 0;
        if (weq_ascii(e, len, "lt")) code = '<'; else if (weq_ascii(e, len, "gt")) code = '>';
        else if (weq_ascii(e, len, "amp")) code = '&'; else if (weq_ascii(e, len, "quot")) code = '"';
        else if (weq_ascii(e, len, "apos")) code = '\'';
        else if (len > 1 && e[0] == '#') {
            int hex = e[1] == 'x' || e[1] == 'X';
            for (size_t k = hex ? 2 : 1; k < len; k++) {
                WCHAR c = e[k];
                int v = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (v < 0) { code = 0; break; }
                code = code * (hex ? 16 : 10) + (uint32_t)v;
            }
        }
        if (!code) { d[o++] = s[i++]; continue; }
        if (code >= 0x10000) { code -= 0x10000; d[o++] = (WCHAR)(0xD800 + (code >> 10)); d[o++] = (WCHAR)(0xDC00 + (code & 0x3FF)); }
        else d[o++] = (WCHAR)code;
        i = j + 1;
    }
    d[o] = 0;
    *out_n = o;
    return d;
}
static int parse_attributes(reader *r, xml_node *e) {
    for (;;) {
        skip_space(r);
        if (r->i >= r->n) { fail(r, "the element's tag does not end"); return 0; }
        if (r->s[r->i] == '>' || at(r, "/>")) return 1;
        size_t start = r->i;
        while (r->i < r->n && is_name(r->s[r->i])) r->i++;
        if (r->i == start) { fail(r, "an attribute name was expected"); return 0; }
        size_t name_len = r->i - start;
        skip_space(r);
        if (r->i >= r->n || r->s[r->i] != '=') { fail(r, "'=' was expected after the attribute name"); return 0; }
        r->i++;
        skip_space(r);
        if (r->i >= r->n || (r->s[r->i] != '"' && r->s[r->i] != '\'')) { fail(r, "a quoted attribute value was expected"); return 0; }
        WCHAR quote = r->s[r->i++];
        size_t vstart = r->i;
        while (r->i < r->n && r->s[r->i] != quote) { if (r->s[r->i] == '\n') r->line++; r->i++; }
        if (r->i >= r->n) { fail(r, "the attribute value does not end"); return 0; }
        WCHAR *name = wdup(r->s + start, name_len);
        size_t vn = 0;
        WCHAR *value = decode(r->s + vstart, r->i - vstart, &vn);
        if (name && value) attr_set(e, name, value, vn);
        free(name); free(value);
        r->i++;
    }
}
static int parse_content(reader *r, xml_node *parent);
static int parse_element(reader *r, xml_node *parent) {
    r->i++;                                                                 // '<'
    size_t start = r->i;
    while (r->i < r->n && is_name(r->s[r->i])) r->i++;
    if (r->i == start) { fail(r, "an element name was expected"); return 0; }
    xml_node *e = node_new(r->doc, NODE_ELEMENT, r->s + start, r->i - start, NULL, 0);
    if (!e) { fail(r, "out of memory"); return 0; }
    node_append(parent, e);
    if (!parse_attributes(r, e)) return 0;
    if (at(r, "/>")) { r->i += 2; return 1; }
    r->i++;                                                                 // '>'
    if (!parse_content(r, e)) return 0;
    if (!at(r, "</")) { fail(r, "the end tag is missing"); return 0; }
    r->i += 2;
    size_t estart = r->i;
    while (r->i < r->n && is_name(r->s[r->i])) r->i++;
    if (r->i - estart != vx_wcslen(e->name) || memcmp(r->s + estart, e->name, (r->i - estart) * 2)) { fail(r, "the end tag does not match the start tag"); return 0; }
    skip_space(r);
    if (r->i >= r->n || r->s[r->i] != '>') { fail(r, "'>' was expected after the end tag"); return 0; }
    r->i++;
    return 1;
}
static void add_text(reader *r, xml_node *parent, size_t start, size_t end) {
    int blank = 1;
    for (size_t k = start; k < end && blank; k++) blank = is_space(r->s[k]);
    if (blank && !r->doc->preserve_whitespace) return;
    size_t n = 0;
    WCHAR *t = decode(r->s + start, end - start, &n);
    if (!t) return;
    xml_node *node = node_new(r->doc, NODE_TEXT, NULL, 0, t, n);
    free(t);
    if (node) node_append(parent, node);
}
static int parse_content(reader *r, xml_node *parent) {
    while (r->i < r->n) {
        size_t start = r->i;
        while (r->i < r->n && r->s[r->i] != '<') { if (r->s[r->i] == '\n') r->line++; r->i++; }
        if (r->i > start) add_text(r, parent, start, r->i);
        if (r->i >= r->n) return 1;
        if (at(r, "</")) return 1;
        if (at(r, "<!--")) {
            size_t cstart = r->i + 4;
            while (r->i < r->n && !at(r, "-->")) r->i++;
            if (r->i >= r->n) { fail(r, "the comment does not end"); return 0; }
            xml_node *c = node_new(r->doc, NODE_COMMENT, NULL, 0, r->s + cstart, r->i - cstart);
            if (c) node_append(parent, c);
            r->i += 3;
        } else if (at(r, "<![CDATA[")) {
            size_t cstart = r->i + 9;
            while (r->i < r->n && !at(r, "]]>")) r->i++;
            if (r->i >= r->n) { fail(r, "the CDATA section does not end"); return 0; }
            xml_node *c = node_new(r->doc, NODE_CDATA_SECTION, NULL, 0, r->s + cstart, r->i - cstart);
            if (c) node_append(parent, c);
            r->i += 3;
        } else if (at(r, "<?")) {
            size_t pstart = r->i + 2, tend;
            r->i = pstart;
            while (r->i < r->n && is_name(r->s[r->i])) r->i++;
            tend = r->i;
            skip_space(r);
            size_t dstart = r->i;
            while (r->i < r->n && !at(r, "?>")) r->i++;
            if (r->i >= r->n) { fail(r, "the processing instruction does not end"); return 0; }
            xml_node *p = node_new(r->doc, NODE_PROCESSING_INSTRUCTION, r->s + pstart, tend - pstart, r->s + dstart, r->i - dstart);
            if (p) node_append(parent, p);
            r->i += 2;
        } else if (at(r, "<!")) {                                           // a DOCTYPE: skipped, brackets included
            int depth = 0;
            while (r->i < r->n) {
                WCHAR c = r->s[r->i++];
                if (c == '[') depth++; else if (c == ']') depth--; else if (c == '>' && depth <= 0) break;
            }
        } else if (!parse_element(r, parent)) return 0;
    }
    return 1;
}
static void doc_clear(xml_doc *d) {
    for (xml_node *n = d->all; n;) { xml_node *next = n->all_next; free(n->name); free(n->value); free(n); n = next; }
    d->all = NULL; d->root = NULL; d->parsed = 0;
    d->error_code = d->error_line = d->error_pos = 0;
    free(d->error_reason); d->error_reason = NULL;
}
static int doc_parse(xml_doc *d, const WCHAR *text, size_t n) {
    doc_clear(d);
    d->root = node_new(d, NODE_DOCUMENT, NULL, 0, NULL, 0);
    if (!d->root) return 0;
    reader r = { text, n, 0, d, 1 };
    if (n && text[0] == 0xFEFF) r.i = 1;
    int ok = parse_content(&r, d->root);
    if (ok && r.i < n) { fail(&r, "text after the document element"); ok = 0; }
    if (ok && !doc_element(d)) { fail(&r, "no document element"); ok = 0; }
    d->parsed = ok;
    if (!ok) {                                                              // as MSXML: a failed load leaves the document empty
        char *why = vx_utf8_dup(d->error_reason);
        vx_log("msxml: parse error at line %d: %s", (int)d->error_line, why ? why : "?");
        free(why);
        for (xml_node *k = d->all; k;) { xml_node *next = k->all_next; free(k->name); free(k->value); free(k); k = next; }
        d->all = NULL; d->root = NULL;
    }
    return ok;
}
// Bytes from a file or a loadXML string to UTF-16: UTF-8 (a BOM or none), UTF-16 with a BOM.
static WCHAR *to_utf16(const uint8_t *bytes, size_t n, size_t *out_n) {
    if (n >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) { *out_n = (n - 2) / 2; return wdup((const WCHAR *)(bytes + 2), *out_n); }
    if (n >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        *out_n = (n - 2) / 2;
        WCHAR *w = malloc((*out_n + 1) * 2);
        if (!w) return NULL;
        for (size_t i = 0; i < *out_n; i++) w[i] = (WCHAR)(bytes[2 + i * 2] << 8 | bytes[3 + i * 2]);
        w[*out_n] = 0;
        return w;
    }
    if (n >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) { bytes += 3; n -= 3; }
    WCHAR *w = malloc((n + 1) * 2);
    if (!w) return NULL;
    *out_n = vx_utf8_to_utf16((const char *)bytes, n, w, n);
    w[*out_n] = 0;
    return w;
}
static int doc_load_file(xml_doc *d, const WCHAR *win_path) {
    char *win = vx_utf8_dup(win_path), unix_path[1024];
    if (!win) return 0;
    int ok = vx_unix_path(win, unix_path, sizeof unix_path);
    FILE *f = ok ? fopen(unix_path, "rb") : NULL;
    if (trace_xml()) vx_log("msxml: load %s -> %s%s", win, ok ? unix_path : "(no path)", f ? "" : " (cannot open)");
    free(win);
    if (!f) { doc_clear(d); d->error_code = (LONG)0x800C0006; return 0; }    // INET_E_OBJECT_NOT_FOUND
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bytes = size > 0 ? malloc((size_t)size) : NULL;
    size_t got = bytes ? fread(bytes, 1, (size_t)size, f) : 0;
    fclose(f);
    size_t n = 0;
    WCHAR *text = bytes ? to_utf16(bytes, got, &n) : NULL;
    free(bytes);
    if (!text) { doc_clear(d); d->error_code = (LONG)0x800C0006; return 0; }
    free(d->url); d->url = wdup(win_path, vx_wcslen(win_path));
    ok = doc_parse(d, text, n);
    free(text);
    return ok;
}

// ---- the writer ----------------------------------------------------------------------------------------------------------------
typedef struct { char *s; size_t n, cap; } buf;
static void put(buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { size_t cap = (b->n + n + 1) * 2; char *t = realloc(b->s, cap); if (!t) return; b->s = t; b->cap = cap; }
    memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
}
static void put_wide(buf *b, const WCHAR *w, int escape, int attribute) {
    if (!w) return;
    for (size_t n = vx_wcslen(w), i = 0; i < n; i++) {
        WCHAR c = w[i];
        if (escape && c == '<') put(b, "&lt;", 4);
        else if (escape && c == '>') put(b, "&gt;", 4);
        else if (escape && c == '&') put(b, "&amp;", 5);
        else if (escape && attribute && c == '"') put(b, "&quot;", 6);
        else { char u[8]; size_t k = vx_utf16_to_utf8(w + i, c >= 0xD800 && c < 0xDC00 && i + 1 < n ? 2 : 1, u, sizeof u); put(b, u, k); if (c >= 0xD800 && c < 0xDC00) i++; }
    }
}
static void write_node(buf *b, const xml_node *n, int depth, int pretty) {
    if (pretty && n->type != NODE_TEXT && n->type != NODE_CDATA_SECTION) for (int i = 0; i < depth; i++) put(b, "\t", 1);
    switch (n->type) {
    case NODE_ELEMENT: {
        put(b, "<", 1); put_wide(b, n->name, 0, 0);
        for (const xml_node *a = n->attrs; a; a = a->next) { put(b, " ", 1); put_wide(b, a->name, 0, 0); put(b, "=\"", 2); put_wide(b, a->value, 1, 1); put(b, "\"", 1); }
        if (!n->first) { put(b, "/>", 2); break; }
        put(b, ">", 1);
        int only_text = n->first == n->last && (n->first->type == NODE_TEXT || n->first->type == NODE_CDATA_SECTION);
        if (pretty && !only_text) put(b, "\r\n", 2);
        for (const xml_node *c = n->first; c; c = c->next) {
            write_node(b, c, depth + 1, pretty && !only_text);
            if (pretty && !only_text) put(b, "\r\n", 2);
        }
        if (pretty && !only_text) for (int i = 0; i < depth; i++) put(b, "\t", 1);
        put(b, "</", 2); put_wide(b, n->name, 0, 0); put(b, ">", 1);
        break;
    }
    case NODE_TEXT: put_wide(b, n->value, 1, 0); break;
    case NODE_CDATA_SECTION: put(b, "<![CDATA[", 9); put_wide(b, n->value, 0, 0); put(b, "]]>", 3); break;
    case NODE_COMMENT: put(b, "<!--", 4); put_wide(b, n->value, 0, 0); put(b, "-->", 3); break;
    case NODE_PROCESSING_INSTRUCTION: put(b, "<?", 2); put_wide(b, n->name, 0, 0); if (n->value && n->value[0]) { put(b, " ", 1); put_wide(b, n->value, 0, 0); } put(b, "?>", 2); break;
    case NODE_ATTRIBUTE: put_wide(b, n->name, 0, 0); put(b, "=\"", 2); put_wide(b, n->value, 1, 1); put(b, "\"", 1); break;
    case NODE_DOCUMENT:
        for (const xml_node *c = n->first; c; c = c->next) { write_node(b, c, 0, pretty); put(b, "\r\n", 2); }
        break;
    }
}

// ---- text of a node ----------------------------------------------------------------------------------------------------------------
static void collect_text(const xml_node *n, buf *b) {
    if (n->type == NODE_TEXT || n->type == NODE_CDATA_SECTION) { put_wide(b, n->value, 0, 0); return; }
    if (n->type == NODE_ATTRIBUTE) { put_wide(b, n->value, 0, 0); return; }
    if (n->type == NODE_COMMENT || n->type == NODE_PROCESSING_INSTRUCTION) { put_wide(b, n->value, 0, 0); return; }
    for (const xml_node *c = n->first; c; c = c->next) if (c->type != NODE_COMMENT && c->type != NODE_PROCESSING_INSTRUCTION) collect_text(c, b);
}
static WCHAR *node_text_bstr(const xml_node *n) {
    buf b = { 0 };
    collect_text(n, &b);
    WCHAR *w = malloc((b.n + 1) * 2);
    size_t k = w ? vx_utf8_to_utf16(b.s ? b.s : "", b.n, w, b.n) : 0;
    WCHAR *r = bstr_from_utf16(w, k);
    free(w); free(b.s);
    return r;
}

// ---- COM objects: nodes --------------------------------------------------------------------------------------------------------------
// A node's interface pointer is the node itself (its first member is the vtable). AddRef/Release count on the document.
static int node_supports(const xml_node *n, const GUID *iid) {
    if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDispatch) || same_guid(iid, &IID_IXMLDOMNode)) return 1;
    switch (n->type) {
    case NODE_ELEMENT: return same_guid(iid, &IID_IXMLDOMElement);
    case NODE_ATTRIBUTE: return same_guid(iid, &IID_IXMLDOMAttribute);
    case NODE_TEXT: return same_guid(iid, &IID_IXMLDOMText) || same_guid(iid, &IID_IXMLDOMCharacterData);
    case NODE_CDATA_SECTION: return same_guid(iid, &IID_IXMLDOMCDATASection) || same_guid(iid, &IID_IXMLDOMText) || same_guid(iid, &IID_IXMLDOMCharacterData);
    case NODE_COMMENT: return same_guid(iid, &IID_IXMLDOMComment) || same_guid(iid, &IID_IXMLDOMCharacterData);
    case NODE_PROCESSING_INSTRUCTION: return same_guid(iid, &IID_IXMLDOMProcessingInstruction);
    }
    return 0;
}
static void *node_ref(xml_node *n) { if (!n) return NULL; if (n->type == NODE_DOCUMENT) { doc_addref(n->doc); return n->doc; } doc_addref(n->doc); return n; }
// The node behind an interface pointer a program hands back (a document object stands for its document node).
static xml_node *node_of(void *itf) {
    if (!itf) return NULL;
    if (*(const void ***)itf == g_doc_vtbl) return ((xml_doc *)itf)->root;
    return (xml_node *)itf;
}
static WINAPI HRESULT node_QueryInterface(xml_node *n, const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    if (!node_supports(n, iid)) { *out = NULL; return E_NOINTERFACE; }
    *out = node_ref(n);
    return S_OK;
}
static WINAPI DWORD node_AddRef(xml_node *n) { doc_addref(n->doc); return 2; }
static WINAPI DWORD node_Release(xml_node *n) { doc_release(n->doc); return 1; }
static WINAPI HRESULT disp_GetTypeInfoCount(void *self, DWORD *count) { (void)self; if (count) *count = 0; return S_OK; }
static WINAPI HRESULT disp_GetTypeInfo(void *self, DWORD i, DWORD lcid, void **out) { (void)self; (void)i; (void)lcid; if (out) *out = NULL; return E_NOTIMPL; }
static WINAPI HRESULT disp_GetIDsOfNames(void *self, const GUID *iid, WCHAR **names, DWORD n, DWORD lcid, LONG *ids) { (void)self; (void)iid; (void)names; (void)n; (void)lcid; (void)ids; return E_NOTIMPL; }
static WINAPI HRESULT disp_Invoke(void *self, LONG id, const GUID *iid, DWORD lcid, uint16_t flags, void *params, VARIANT *result, void *excep, DWORD *arg_err) {
    (void)self; (void)id; (void)iid; (void)lcid; (void)flags; (void)params; (void)result; (void)excep; (void)arg_err; return E_NOTIMPL;
}
static WINAPI HRESULT node_get_nodeName(xml_node *n, WCHAR **out) {
    if (!out) return E_POINTER;
    switch (n->type) {
    case NODE_ELEMENT: case NODE_ATTRIBUTE: case NODE_PROCESSING_INSTRUCTION: *out = bstr(n->name); break;
    case NODE_TEXT: *out = bstr_ascii("#text"); break;
    case NODE_CDATA_SECTION: *out = bstr_ascii("#cdata-section"); break;
    case NODE_COMMENT: *out = bstr_ascii("#comment"); break;
    default: *out = bstr_ascii("#document"); break;
    }
    return S_OK;
}
static WINAPI HRESULT node_get_nodeValue(xml_node *n, VARIANT *out) {
    if (!out) return E_POINTER;
    variant_init(out);
    if (n->type == NODE_ELEMENT || n->type == NODE_DOCUMENT) { out->vt = VT_NULL; return S_FALSE; }
    out->vt = VT_BSTR; out->v.bstr = bstr(n->value ? n->value : (const WCHAR[]){ 0 });
    return S_OK;
}
static WINAPI HRESULT node_put_nodeValue(xml_node *n, VARIANT value) {
    if (n->type == NODE_ELEMENT || n->type == NODE_DOCUMENT) return E_FAIL;
    char scratch[64];
    const WCHAR *t = variant_text(&value, scratch, sizeof scratch);
    free(n->value); n->value = wdup(t ? t : (const WCHAR[]){ 0 }, t ? vx_wcslen(t) : 0);
    return S_OK;
}
static WINAPI HRESULT node_get_nodeType(xml_node *n, LONG *out) { if (!out) return E_POINTER; *out = n->type; return S_OK; }
static WINAPI HRESULT node_get_parentNode(xml_node *n, void **out) {
    if (!out) return E_POINTER;
    xml_node *p = n->type == NODE_ATTRIBUTE ? NULL : n->parent;
    *out = node_ref(p);
    return p ? S_OK : S_FALSE;
}
static xml_list *list_new(xml_doc *d) {
    xml_list *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->vtbl = g_list_vtbl; l->refs = 1; l->doc = d; doc_addref(d);
    return l;
}
static int list_add(xml_list *l, xml_node *n) {
    if ((l->count & (l->count + 1)) == 0) {                                  // (grown at 0, 1, 3, 7, ...)
        xml_node **t = realloc(l->items, ((size_t)l->count + 1) * 2 * sizeof *t);
        if (!t) return 0;
        l->items = t;
    }
    l->items[l->count++] = n;
    return 1;
}
static WINAPI HRESULT node_get_childNodes(xml_node *n, void **out) {
    if (!out) return E_POINTER;
    xml_list *l = list_new(n->doc);
    if (!l) return E_OUTOFMEMORY;
    for (xml_node *c = n->first; c; c = c->next) list_add(l, c);
    *out = l;
    return S_OK;
}
static WINAPI HRESULT node_get_firstChild(xml_node *n, void **out) { if (!out) return E_POINTER; *out = node_ref(n->first); return n->first ? S_OK : S_FALSE; }
static WINAPI HRESULT node_get_lastChild(xml_node *n, void **out) { if (!out) return E_POINTER; *out = node_ref(n->last); return n->last ? S_OK : S_FALSE; }
static WINAPI HRESULT node_get_previousSibling(xml_node *n, void **out) { if (!out) return E_POINTER; xml_node *p = n->type == NODE_ATTRIBUTE ? NULL : n->prev; *out = node_ref(p); return p ? S_OK : S_FALSE; }
static WINAPI HRESULT node_get_nextSibling(xml_node *n, void **out) { if (!out) return E_POINTER; xml_node *p = n->type == NODE_ATTRIBUTE ? NULL : n->next; *out = node_ref(p); return p ? S_OK : S_FALSE; }
static WINAPI HRESULT node_get_attributes(xml_node *n, void **out) {
    if (!out) return E_POINTER;
    if (n->type != NODE_ELEMENT) { *out = NULL; return S_FALSE; }
    xml_list *l = list_new(n->doc);
    if (!l) return E_OUTOFMEMORY;
    l->vtbl = g_map_vtbl; l->attributes = 1; l->owner = n;
    for (xml_node *a = n->attrs; a; a = a->next) list_add(l, a);
    *out = l;
    return S_OK;
}
static WINAPI HRESULT node_insertBefore(xml_node *n, void *child_itf, VARIANT ref, void **out) {
    xml_node *child = node_of(child_itf);
    if (!child || child->doc != n->doc || child->type == NODE_ATTRIBUTE || child->type == NODE_DOCUMENT) return E_INVALIDARG;
    xml_node *before = NULL;
    if (ref.vt == VT_DISPATCH || ref.vt == VT_UNKNOWN) before = node_of(ref.v.ptr);
    node_unlink(child);
    if (before && before->parent == n) {
        child->parent = n; child->next = before; child->prev = before->prev;
        if (before->prev) before->prev->next = child; else n->first = child;
        before->prev = child;
    } else node_append(n, child);
    if (out) *out = node_ref(child);
    return S_OK;
}
static WINAPI HRESULT node_replaceChild(xml_node *n, void *new_itf, void *old_itf, void **out) {
    xml_node *nw = node_of(new_itf), *old = node_of(old_itf);
    if (!nw || !old || old->parent != n || nw->doc != n->doc) return E_INVALIDARG;
    node_unlink(nw);
    nw->parent = n; nw->prev = old->prev; nw->next = old->next;
    if (old->prev) old->prev->next = nw; else n->first = nw;
    if (old->next) old->next->prev = nw; else n->last = nw;
    old->parent = NULL; old->prev = old->next = NULL;
    if (out) *out = node_ref(old);
    return S_OK;
}
static WINAPI HRESULT node_removeChild(xml_node *n, void *child_itf, void **out) {
    xml_node *child = node_of(child_itf);
    if (!child || child->parent != n) return E_INVALIDARG;
    node_unlink(child);
    if (out) *out = node_ref(child);
    return S_OK;
}
static WINAPI HRESULT node_appendChild(xml_node *n, void *child_itf, void **out) {
    VARIANT none; variant_init(&none);
    return node_insertBefore(n, child_itf, none, out);
}
static WINAPI HRESULT node_hasChildNodes(xml_node *n, VARIANT_BOOL *out) { if (!out) return E_POINTER; *out = n->first ? VB_TRUE : 0; return n->first ? S_OK : S_FALSE; }
static WINAPI HRESULT node_get_ownerDocument(xml_node *n, void **out) { if (!out) return E_POINTER; doc_addref(n->doc); *out = n->doc; return S_OK; }
static xml_node *clone(xml_node *n, int deep) {
    xml_node *c = node_new(n->doc, n->type, n->name, n->name ? vx_wcslen(n->name) : 0, n->value, n->value ? vx_wcslen(n->value) : 0);
    if (!c) return NULL;
    for (xml_node *a = n->attrs; a; a = a->next) attr_set(c, a->name, a->value, a->value ? vx_wcslen(a->value) : 0);
    if (deep) for (xml_node *k = n->first; k; k = k->next) { xml_node *kc = clone(k, 1); if (kc) node_append(c, kc); }
    return c;
}
static WINAPI HRESULT node_cloneNode(xml_node *n, VARIANT_BOOL deep, void **out) {
    if (!out) return E_POINTER;
    xml_node *c = clone(n, deep != 0);
    *out = node_ref(c);
    return c ? S_OK : E_OUTOFMEMORY;
}
static WINAPI HRESULT node_get_nodeTypeString(xml_node *n, WCHAR **out) {
    if (!out) return E_POINTER;
    static const char *names[] = { "", "element", "attribute", "text", "cdatasection", "entityreference", "entity", "processinginstruction", "comment", "document" };
    *out = bstr_ascii(n->type >= 1 && n->type <= 9 ? names[n->type] : "");
    return S_OK;
}
static WINAPI HRESULT node_get_text(xml_node *n, WCHAR **out) { if (!out) return E_POINTER; *out = node_text_bstr(n); return S_OK; }
static WINAPI HRESULT node_put_text(xml_node *n, WCHAR *text) {
    if (n->type == NODE_ELEMENT || n->type == NODE_DOCUMENT) {
        for (xml_node *c = n->first; c;) { xml_node *next = c->next; node_unlink(c); c = next; }
        xml_node *t = node_new(n->doc, NODE_TEXT, NULL, 0, text, text ? vx_wcslen(text) : 0);
        if (t) node_append(n, t);
        return S_OK;
    }
    free(n->value); n->value = wdup(text ? text : (const WCHAR[]){ 0 }, text ? vx_wcslen(text) : 0);
    return S_OK;
}
static WINAPI HRESULT node_get_specified(xml_node *n, VARIANT_BOOL *out) { (void)n; if (!out) return E_POINTER; *out = VB_TRUE; return S_OK; }
static WINAPI HRESULT node_get_definition(xml_node *n, void **out) { (void)n; if (!out) return E_POINTER; *out = NULL; return S_FALSE; }
static WINAPI HRESULT node_get_nodeTypedValue(xml_node *n, VARIANT *out) {
    if (!out) return E_POINTER;
    variant_init(out);
    out->vt = VT_BSTR; out->v.bstr = node_text_bstr(n);
    return S_OK;
}
static WINAPI HRESULT node_put_nodeTypedValue(xml_node *n, VARIANT value) {
    char scratch[64];
    const WCHAR *t = variant_text(&value, scratch, sizeof scratch);
    return node_put_text(n, (WCHAR *)t);
}
static WINAPI HRESULT node_get_dataType(xml_node *n, VARIANT *out) { (void)n; if (!out) return E_POINTER; variant_init(out); out->vt = VT_NULL; return S_FALSE; }
static WINAPI HRESULT node_put_dataType(xml_node *n, WCHAR *type) { (void)n; (void)type; return S_OK; }
static WINAPI HRESULT node_get_xml(xml_node *n, WCHAR **out) {
    if (!out) return E_POINTER;
    buf b = { 0 };
    write_node(&b, n, 0, 1);
    WCHAR *w = malloc((b.n + 1) * 2);
    size_t k = w ? vx_utf8_to_utf16(b.s ? b.s : "", b.n, w, b.n) : 0;
    *out = bstr_from_utf16(w, k);
    free(w); free(b.s);
    return S_OK;
}
static WINAPI HRESULT node_transformNode(xml_node *n, void *sheet, WCHAR **out) { (void)n; (void)sheet; if (out) *out = NULL; return E_NOTIMPL; }
static int select_nodes(xml_node *context, const WCHAR *query, xml_list *into, int first_only);
static WINAPI HRESULT node_selectNodes(xml_node *n, WCHAR *query, void **out) {
    if (!out) return E_POINTER;
    xml_list *l = list_new(n->doc);
    if (!l) return E_OUTOFMEMORY;
    int ok = select_nodes(n, query, l, 0);
    if (trace_xml()) { char *q = vx_utf8_dup(query), *name = vx_utf8_dup(n->name); vx_log("msxml: selectNodes(%s) on <%s>: %d node(s)%s", q ? q : "?", name ? name : "#document", (int)l->count, ok ? "" : " (query not understood)"); free(q); free(name); }
    *out = l;
    return ok ? S_OK : E_FAIL;
}
static WINAPI HRESULT node_selectSingleNode(xml_node *n, WCHAR *query, void **out) {
    if (!out) return E_POINTER;
    xml_list l = { g_list_vtbl, 1, n->doc, NULL, 0, 0, 0, NULL };
    int ok = select_nodes(n, query, &l, 1);
    xml_node *found = l.count ? l.items[0] : NULL;
    if (trace_xml()) { char *q = vx_utf8_dup(query); vx_log("msxml: selectSingleNode(%s): %s%s", q ? q : "?", found ? "found" : "none", ok ? "" : " (query not understood)"); free(q); }
    free(l.items);
    *out = node_ref(found);
    return !ok ? E_FAIL : found ? S_OK : S_FALSE;
}
static WINAPI HRESULT node_get_parsed(xml_node *n, VARIANT_BOOL *out) { (void)n; if (!out) return E_POINTER; *out = VB_TRUE; return S_OK; }
static WINAPI HRESULT node_get_namespaceURI(xml_node *n, WCHAR **out) { (void)n; if (!out) return E_POINTER; *out = NULL; return S_FALSE; }
static WINAPI HRESULT node_get_prefix(xml_node *n, WCHAR **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (!n->name) return S_FALSE;
    for (size_t i = 0; n->name[i]; i++) if (n->name[i] == ':') { *out = bstr_from_utf16(n->name, i); return S_OK; }
    return S_FALSE;
}
static WINAPI HRESULT node_get_baseName(xml_node *n, WCHAR **out) {
    if (!out) return E_POINTER;
    if (!n->name) { *out = NULL; return S_FALSE; }
    const WCHAR *base = n->name;
    for (size_t i = 0; n->name[i]; i++) if (n->name[i] == ':') base = n->name + i + 1;
    *out = bstr(base);
    return S_OK;
}
static WINAPI HRESULT node_transformNodeToObject(xml_node *n, void *sheet, VARIANT out) { (void)n; (void)sheet; (void)out; return E_NOTIMPL; }

// ---- elements ----------------------------------------------------------------------------------------------------------------
static WINAPI HRESULT element_get_tagName(xml_node *n, WCHAR **out) { if (!out) return E_POINTER; *out = bstr(n->name); return S_OK; }
static WINAPI HRESULT element_getAttribute(xml_node *n, WCHAR *name, VARIANT *out) {
    if (!out) return E_POINTER;
    variant_init(out);
    xml_node *a = name ? attr_find(n, name) : NULL;
    if (!a) { out->vt = VT_NULL; return S_FALSE; }
    out->vt = VT_BSTR; out->v.bstr = bstr(a->value);
    return S_OK;
}
static WINAPI HRESULT element_setAttribute(xml_node *n, WCHAR *name, VARIANT value) {
    if (!name) return E_INVALIDARG;
    char scratch[64];
    const WCHAR *t = variant_text(&value, scratch, sizeof scratch);
    attr_set(n, name, t ? t : (const WCHAR[]){ 0 }, t ? vx_wcslen(t) : 0);
    return S_OK;
}
static WINAPI HRESULT element_removeAttribute(xml_node *n, WCHAR *name) {
    xml_node *a = name ? attr_find(n, name) : NULL;
    if (!a) return S_FALSE;
    if (a->prev) a->prev->next = a->next; else n->attrs = a->next;
    if (a->next) a->next->prev = a->prev;
    a->parent = NULL; a->prev = a->next = NULL;
    return S_OK;
}
static WINAPI HRESULT element_getAttributeNode(xml_node *n, WCHAR *name, void **out) {
    if (!out) return E_POINTER;
    xml_node *a = name ? attr_find(n, name) : NULL;
    *out = node_ref(a);
    return a ? S_OK : S_FALSE;
}
static WINAPI HRESULT element_setAttributeNode(xml_node *n, void *attr_itf, void **out) {
    xml_node *a = node_of(attr_itf);
    if (!a || a->type != NODE_ATTRIBUTE) return E_INVALIDARG;
    xml_node *old = attr_find(n, a->name);
    if (out) *out = node_ref(old);
    attr_set(n, a->name, a->value ? a->value : (const WCHAR[]){ 0 }, a->value ? vx_wcslen(a->value) : 0);
    return S_OK;
}
static WINAPI HRESULT element_removeAttributeNode(xml_node *n, void *attr_itf, void **out) {
    xml_node *a = node_of(attr_itf);
    if (!a || a->type != NODE_ATTRIBUTE) return E_INVALIDARG;
    if (out) *out = node_ref(a);
    return element_removeAttribute(n, a->name);
}
static void collect_by_tag(xml_node *n, const WCHAR *tag, xml_list *into) {
    for (xml_node *c = n->first; c; c = c->next) {
        if (c->type == NODE_ELEMENT && ((tag[0] == '*' && !tag[1]) || weq(c->name, tag))) list_add(into, c);
        collect_by_tag(c, tag, into);
    }
}
static WINAPI HRESULT element_getElementsByTagName(xml_node *n, WCHAR *tag, void **out) {
    if (!out) return E_POINTER;
    xml_list *l = list_new(n->doc);
    if (!l) return E_OUTOFMEMORY;
    if (tag) collect_by_tag(n, tag, l);
    *out = l;
    return S_OK;
}
static WINAPI HRESULT element_normalize(xml_node *n) { (void)n; return S_OK; }

// ---- attributes, text --------------------------------------------------------------------------------------------------------
static WINAPI HRESULT attr_get_name(xml_node *n, WCHAR **out) { if (!out) return E_POINTER; *out = bstr(n->name); return S_OK; }
static WINAPI HRESULT attr_get_value(xml_node *n, VARIANT *out) { return node_get_nodeValue(n, out); }
static WINAPI HRESULT attr_put_value(xml_node *n, VARIANT value) { return node_put_nodeValue(n, value); }
static WINAPI HRESULT text_get_data(xml_node *n, WCHAR **out) { if (!out) return E_POINTER; *out = bstr(n->value ? n->value : (const WCHAR[]){ 0 }); return S_OK; }
static WINAPI HRESULT text_put_data(xml_node *n, WCHAR *data) { free(n->value); n->value = wdup(data ? data : (const WCHAR[]){ 0 }, data ? vx_wcslen(data) : 0); return S_OK; }
static WINAPI HRESULT text_get_length(xml_node *n, LONG *out) { if (!out) return E_POINTER; *out = n->value ? (LONG)vx_wcslen(n->value) : 0; return S_OK; }
static WINAPI HRESULT text_substringData(xml_node *n, LONG offset, LONG count, WCHAR **out) {
    if (!out) return E_POINTER;
    LONG len = n->value ? (LONG)vx_wcslen(n->value) : 0;
    if (offset < 0 || offset > len || count < 0) return E_INVALIDARG;
    if (offset + count > len) count = len - offset;
    *out = bstr_from_utf16(n->value + offset, (size_t)count);
    return S_OK;
}
static WINAPI HRESULT text_appendData(xml_node *n, WCHAR *data) {
    size_t a = n->value ? vx_wcslen(n->value) : 0, b = data ? vx_wcslen(data) : 0;
    WCHAR *t = malloc((a + b + 1) * 2);
    if (!t) return E_OUTOFMEMORY;
    if (a) memcpy(t, n->value, a * 2);
    if (b) memcpy(t + a, data, b * 2);
    t[a + b] = 0;
    free(n->value); n->value = t;
    return S_OK;
}
static WINAPI HRESULT text_insertData(xml_node *n, LONG offset, WCHAR *data) {
    size_t a = n->value ? vx_wcslen(n->value) : 0, b = data ? vx_wcslen(data) : 0;
    if (offset < 0 || (size_t)offset > a) return E_INVALIDARG;
    WCHAR *t = malloc((a + b + 1) * 2);
    if (!t) return E_OUTOFMEMORY;
    memcpy(t, n->value, (size_t)offset * 2);
    if (b) memcpy(t + offset, data, b * 2);
    memcpy(t + offset + b, n->value + offset, (a - (size_t)offset) * 2);
    t[a + b] = 0;
    free(n->value); n->value = t;
    return S_OK;
}
static WINAPI HRESULT text_deleteData(xml_node *n, LONG offset, LONG count) {
    size_t a = n->value ? vx_wcslen(n->value) : 0;
    if (offset < 0 || (size_t)offset > a || count < 0) return E_INVALIDARG;
    if ((size_t)(offset + count) > a) count = (LONG)(a - (size_t)offset);
    memmove(n->value + offset, n->value + offset + count, (a - (size_t)offset - (size_t)count + 1) * 2);
    return S_OK;
}
static WINAPI HRESULT text_replaceData(xml_node *n, LONG offset, LONG count, WCHAR *data) {
    HRESULT r = text_deleteData(n, offset, count);
    return r == S_OK ? text_insertData(n, offset, data) : r;
}
static WINAPI HRESULT text_splitText(xml_node *n, LONG offset, void **out) {
    if (!out) return E_POINTER;
    size_t a = n->value ? vx_wcslen(n->value) : 0;
    if (offset < 0 || (size_t)offset > a) return E_INVALIDARG;
    xml_node *t = node_new(n->doc, n->type, NULL, 0, n->value + offset, a - (size_t)offset);
    if (!t) return E_OUTOFMEMORY;
    n->value[offset] = 0;
    if (n->parent) {
        t->parent = n->parent; t->prev = n; t->next = n->next;
        if (n->next) n->next->prev = t; else n->parent->last = t;
        n->next = t;
    }
    *out = node_ref(t);
    return S_OK;
}

// ---- node lists and named node maps ------------------------------------------------------------------------------------------
static WINAPI HRESULT list_QueryInterface(xml_list *l, const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDispatch) || (l->attributes ? same_guid(iid, &IID_IXMLDOMNamedNodeMap) : same_guid(iid, &IID_IXMLDOMNodeList))) {
        __atomic_add_fetch(&l->refs, 1, __ATOMIC_ACQ_REL); *out = l; return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI DWORD list_AddRef(xml_list *l) { return (DWORD)__atomic_add_fetch(&l->refs, 1, __ATOMIC_ACQ_REL); }
static WINAPI DWORD list_Release(xml_list *l) {
    LONG r = __atomic_sub_fetch(&l->refs, 1, __ATOMIC_ACQ_REL);
    if (!r) { doc_release(l->doc); free(l->items); free(l); }
    return (DWORD)r;
}
static WINAPI HRESULT list_get_item(xml_list *l, LONG i, void **out) {
    if (!out) return E_POINTER;
    xml_node *n = i >= 0 && i < l->count ? l->items[i] : NULL;
    *out = node_ref(n);
    return n ? S_OK : S_FALSE;
}
static WINAPI HRESULT list_get_length(xml_list *l, LONG *out) { if (!out) return E_POINTER; *out = l->count; return S_OK; }
static WINAPI HRESULT list_nextNode(xml_list *l, void **out) {
    if (!out) return E_POINTER;
    xml_node *n = l->cursor < l->count ? l->items[l->cursor++] : NULL;
    *out = node_ref(n);
    return n ? S_OK : S_FALSE;
}
static WINAPI HRESULT list_reset(xml_list *l) { l->cursor = 0; return S_OK; }
static WINAPI HRESULT list_get__newEnum(xml_list *l, void **out) { (void)l; if (out) *out = NULL; return E_NOTIMPL; }
static WINAPI HRESULT map_getNamedItem(xml_list *l, WCHAR *name, void **out) {
    if (!out) return E_POINTER;
    xml_node *found = NULL;
    for (LONG i = 0; i < l->count && !found; i++) if (weq(l->items[i]->name, name)) found = l->items[i];
    *out = node_ref(found);
    return found ? S_OK : S_FALSE;
}
static WINAPI HRESULT map_setNamedItem(xml_list *l, void *attr_itf, void **out) {
    if (!l->owner) return E_FAIL;
    HRESULT r = element_setAttributeNode(l->owner, attr_itf, out);
    l->count = 0;
    for (xml_node *a = l->owner->attrs; a; a = a->next) list_add(l, a);
    return r;
}
static WINAPI HRESULT map_removeNamedItem(xml_list *l, WCHAR *name, void **out) {
    if (!l->owner) return E_FAIL;
    xml_node *a = name ? attr_find(l->owner, name) : NULL;
    if (out) *out = node_ref(a);
    if (!a) return S_FALSE;
    element_removeAttribute(l->owner, name);
    l->count = 0;
    for (xml_node *k = l->owner->attrs; k; k = k->next) list_add(l, k);
    return S_OK;
}
static WINAPI HRESULT map_getQualifiedItem(xml_list *l, WCHAR *base, WCHAR *ns, void **out) { (void)ns; return map_getNamedItem(l, base, out); }
static WINAPI HRESULT map_removeQualifiedItem(xml_list *l, WCHAR *base, WCHAR *ns, void **out) { (void)ns; return map_removeNamedItem(l, base, out); }

// ---- parse errors --------------------------------------------------------------------------------------------------------------
static WINAPI HRESULT error_QueryInterface(xml_error *e, const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDispatch) || same_guid(iid, &IID_IXMLDOMParseError)) { __atomic_add_fetch(&e->refs, 1, __ATOMIC_ACQ_REL); *out = e; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI DWORD error_AddRef(xml_error *e) { return (DWORD)__atomic_add_fetch(&e->refs, 1, __ATOMIC_ACQ_REL); }
static WINAPI DWORD error_Release(xml_error *e) {
    LONG r = __atomic_sub_fetch(&e->refs, 1, __ATOMIC_ACQ_REL);
    if (!r) { doc_release(e->doc); free(e); }
    return (DWORD)r;
}
static WINAPI HRESULT error_get_errorCode(xml_error *e, LONG *out) { if (!out) return E_POINTER; *out = e->doc->error_code; return e->doc->error_code ? S_OK : S_FALSE; }
static WINAPI HRESULT error_get_url(xml_error *e, WCHAR **out) { if (!out) return E_POINTER; *out = e->doc->url ? bstr(e->doc->url) : NULL; return *out ? S_OK : S_FALSE; }
static WINAPI HRESULT error_get_reason(xml_error *e, WCHAR **out) { if (!out) return E_POINTER; *out = e->doc->error_reason ? bstr(e->doc->error_reason) : bstr_ascii(e->doc->error_code ? "The file could not be read." : ""); return S_OK; }
static WINAPI HRESULT error_get_srcText(xml_error *e, WCHAR **out) { (void)e; if (!out) return E_POINTER; *out = NULL; return S_FALSE; }
static WINAPI HRESULT error_get_line(xml_error *e, LONG *out) { if (!out) return E_POINTER; *out = e->doc->error_line; return S_OK; }
static WINAPI HRESULT error_get_linepos(xml_error *e, LONG *out) { if (!out) return E_POINTER; *out = e->doc->error_pos; return S_OK; }
static WINAPI HRESULT error_get_filepos(xml_error *e, LONG *out) { if (!out) return E_POINTER; *out = e->doc->error_pos; return S_OK; }

// ---- the document --------------------------------------------------------------------------------------------------------------
static WINAPI HRESULT doc_QueryInterface(xml_doc *d, const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDispatch) || same_guid(iid, &IID_IXMLDOMNode) ||
        same_guid(iid, &IID_IXMLDOMDocument) || same_guid(iid, &IID_IXMLDOMDocument2) || same_guid(iid, &IID_IXMLDOMDocument3)) {
        doc_addref(d); *out = d; return S_OK;
    }
    *out = NULL;
    if (trace_xml()) vx_log("msxml: the document does not offer interface {%08x-%04x-%04x-...}", iid->Data1, iid->Data2, iid->Data3);
    return E_NOINTERFACE;
}
static WINAPI DWORD doc_AddRef(xml_doc *d) { return (DWORD)__atomic_add_fetch(&d->refs, 1, __ATOMIC_ACQ_REL); }
static WINAPI DWORD doc_Release(xml_doc *d) {
    LONG r = __atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL);
    if (!r) doc_free(d);
    return (DWORD)r;
}
// IXMLDOMNode on the document: the same methods, on the document node.
#define DOC_NODE(name, params, ...) static WINAPI HRESULT doc_##name params { return node_##name(d->root, __VA_ARGS__); }
#define DOC_NODE0(name) static WINAPI HRESULT doc_##name(xml_doc *d) { return node_##name(d->root); }
DOC_NODE(get_nodeName, (xml_doc *d, WCHAR **out), out)
DOC_NODE(get_nodeValue, (xml_doc *d, VARIANT *out), out)
DOC_NODE(put_nodeValue, (xml_doc *d, VARIANT v), v)
DOC_NODE(get_nodeType, (xml_doc *d, LONG *out), out)
DOC_NODE(get_parentNode, (xml_doc *d, void **out), out)
DOC_NODE(get_childNodes, (xml_doc *d, void **out), out)
DOC_NODE(get_firstChild, (xml_doc *d, void **out), out)
DOC_NODE(get_lastChild, (xml_doc *d, void **out), out)
DOC_NODE(get_previousSibling, (xml_doc *d, void **out), out)
DOC_NODE(get_nextSibling, (xml_doc *d, void **out), out)
DOC_NODE(get_attributes, (xml_doc *d, void **out), out)
static WINAPI HRESULT doc_insertBefore(xml_doc *d, void *child, VARIANT ref, void **out) { return node_insertBefore(d->root, child, ref, out); }
static WINAPI HRESULT doc_replaceChild(xml_doc *d, void *nw, void *old, void **out) { return node_replaceChild(d->root, nw, old, out); }
DOC_NODE(removeChild, (xml_doc *d, void *child, void **out), child, out)
DOC_NODE(appendChild, (xml_doc *d, void *child, void **out), child, out)
DOC_NODE(hasChildNodes, (xml_doc *d, VARIANT_BOOL *out), out)
DOC_NODE(get_ownerDocument, (xml_doc *d, void **out), out)
DOC_NODE(cloneNode, (xml_doc *d, VARIANT_BOOL deep, void **out), deep, out)
DOC_NODE(get_nodeTypeString, (xml_doc *d, WCHAR **out), out)
DOC_NODE(get_text, (xml_doc *d, WCHAR **out), out)
DOC_NODE(put_text, (xml_doc *d, WCHAR *text), text)
DOC_NODE(get_specified, (xml_doc *d, VARIANT_BOOL *out), out)
DOC_NODE(get_definition, (xml_doc *d, void **out), out)
DOC_NODE(get_nodeTypedValue, (xml_doc *d, VARIANT *out), out)
DOC_NODE(put_nodeTypedValue, (xml_doc *d, VARIANT v), v)
DOC_NODE(get_dataType, (xml_doc *d, VARIANT *out), out)
DOC_NODE(put_dataType, (xml_doc *d, WCHAR *t), t)
DOC_NODE(get_xml, (xml_doc *d, WCHAR **out), out)
DOC_NODE(transformNode, (xml_doc *d, void *sheet, WCHAR **out), sheet, out)
DOC_NODE(selectNodes, (xml_doc *d, WCHAR *q, void **out), q, out)
DOC_NODE(selectSingleNode, (xml_doc *d, WCHAR *q, void **out), q, out)
DOC_NODE(get_parsed, (xml_doc *d, VARIANT_BOOL *out), out)
DOC_NODE(get_namespaceURI, (xml_doc *d, WCHAR **out), out)
DOC_NODE(get_prefix, (xml_doc *d, WCHAR **out), out)
DOC_NODE(get_baseName, (xml_doc *d, WCHAR **out), out)
DOC_NODE(transformNodeToObject, (xml_doc *d, void *sheet, VARIANT out), sheet, out)
static WINAPI HRESULT doc_get_doctype(xml_doc *d, void **out) { (void)d; if (!out) return E_POINTER; *out = NULL; return S_FALSE; }
static WINAPI HRESULT doc_get_implementation(xml_doc *d, void **out) { (void)d; if (!out) return E_POINTER; *out = NULL; return E_NOTIMPL; }
static WINAPI HRESULT doc_get_documentElement(xml_doc *d, void **out) {
    if (!out) return E_POINTER;
    xml_node *e = doc_element(d);
    *out = node_ref(e);
    return e ? S_OK : S_FALSE;
}
static WINAPI HRESULT doc_putref_documentElement(xml_doc *d, void *element_itf) {
    xml_node *e = node_of(element_itf);
    if (!e || e->type != NODE_ELEMENT || e->doc != d) return E_INVALIDARG;
    xml_node *old = doc_element(d);
    if (old) node_unlink(old);
    node_unlink(e);
    node_append(d->root, e);
    return S_OK;
}
static HRESULT make_node(xml_doc *d, int type, const WCHAR *name, const WCHAR *value, void **out) {
    if (!out) return E_POINTER;
    if (!d->root) { d->root = node_new(d, NODE_DOCUMENT, NULL, 0, NULL, 0); if (!d->root) return E_OUTOFMEMORY; }
    xml_node *n = node_new(d, type, name, name ? vx_wcslen(name) : 0, value, value ? vx_wcslen(value) : 0);
    *out = node_ref(n);
    return n ? S_OK : E_OUTOFMEMORY;
}
static WINAPI HRESULT doc_createElement(xml_doc *d, WCHAR *name, void **out) { if (!name || !name[0]) return E_INVALIDARG; return make_node(d, NODE_ELEMENT, name, NULL, out); }
static WINAPI HRESULT doc_createDocumentFragment(xml_doc *d, void **out) { (void)d; if (out) *out = NULL; return E_NOTIMPL; }
static WINAPI HRESULT doc_createTextNode(xml_doc *d, WCHAR *text, void **out) { return make_node(d, NODE_TEXT, NULL, text ? text : (const WCHAR[]){ 0 }, out); }
static WINAPI HRESULT doc_createComment(xml_doc *d, WCHAR *text, void **out) { return make_node(d, NODE_COMMENT, NULL, text ? text : (const WCHAR[]){ 0 }, out); }
static WINAPI HRESULT doc_createCDATASection(xml_doc *d, WCHAR *text, void **out) { return make_node(d, NODE_CDATA_SECTION, NULL, text ? text : (const WCHAR[]){ 0 }, out); }
static WINAPI HRESULT doc_createProcessingInstruction(xml_doc *d, WCHAR *target, WCHAR *data, void **out) { if (!target) return E_INVALIDARG; return make_node(d, NODE_PROCESSING_INSTRUCTION, target, data ? data : (const WCHAR[]){ 0 }, out); }
static WINAPI HRESULT doc_createAttribute(xml_doc *d, WCHAR *name, void **out) { if (!name || !name[0]) return E_INVALIDARG; return make_node(d, NODE_ATTRIBUTE, name, (const WCHAR[]){ 0 }, out); }
static WINAPI HRESULT doc_createEntityReference(xml_doc *d, WCHAR *name, void **out) { (void)d; (void)name; if (out) *out = NULL; return E_NOTIMPL; }
static WINAPI HRESULT doc_getElementsByTagName(xml_doc *d, WCHAR *tag, void **out) {
    if (!out) return E_POINTER;
    xml_list *l = list_new(d);
    if (!l) return E_OUTOFMEMORY;
    if (tag && d->root) collect_by_tag(d->root, tag, l);
    *out = l;
    return S_OK;
}
static WINAPI HRESULT doc_createNode(xml_doc *d, VARIANT type, WCHAR *name, WCHAR *ns, void **out) {
    (void)ns;
    int t = type.vt == VT_I4 || type.vt == VT_INT ? type.v.i4 : type.vt == VT_I2 ? type.v.boolean : 0;
    if (type.vt == VT_BSTR) { char scratch[64]; const WCHAR *w = variant_text(&type, scratch, sizeof scratch); char *a = vx_utf8_dup(w); t = a && !strcmp(a, "element") ? NODE_ELEMENT : a && !strcmp(a, "attribute") ? NODE_ATTRIBUTE : a && !strcmp(a, "text") ? NODE_TEXT : 0; free(a); }
    switch (t) {
    case NODE_ELEMENT: return doc_createElement(d, name, out);
    case NODE_ATTRIBUTE: return doc_createAttribute(d, name, out);
    case NODE_TEXT: return doc_createTextNode(d, (WCHAR *)(const WCHAR[]){ 0 }, out);
    case NODE_COMMENT: return doc_createComment(d, (WCHAR *)(const WCHAR[]){ 0 }, out);
    case NODE_CDATA_SECTION: return doc_createCDATASection(d, (WCHAR *)(const WCHAR[]){ 0 }, out);
    case NODE_PROCESSING_INSTRUCTION: return doc_createProcessingInstruction(d, name, NULL, out);
    }
    return E_INVALIDARG;
}
static WINAPI HRESULT doc_nodeFromID(xml_doc *d, WCHAR *id, void **out) { (void)d; (void)id; if (out) *out = NULL; return S_FALSE; }
static WINAPI HRESULT doc_load(xml_doc *d, VARIANT source, VARIANT_BOOL *ok) {
    char scratch[64];
    const WCHAR *path = variant_text(&source, scratch, sizeof scratch);
    int loaded = 0;
    if (path) loaded = doc_load_file(d, path);
    else if ((source.vt == VT_DISPATCH || source.vt == VT_UNKNOWN) && source.v.ptr && *(const void ***)source.v.ptr == g_doc_vtbl) {
        xml_doc *other = source.v.ptr;                                   // another document: its text
        buf b = { 0 };
        if (other->root) write_node(&b, other->root, 0, 1);
        size_t n = 0;
        WCHAR *w = to_utf16((const uint8_t *)(b.s ? b.s : ""), b.n, &n);
        loaded = w ? doc_parse(d, w, n) : 0;
        free(w); free(b.s);
    } else {
        if (trace_xml()) vx_log("msxml: load from a source of variant type %u is not supported", source.vt);
        doc_clear(d); d->error_code = (LONG)0x800C0006;
    }
    if (ok) *ok = loaded ? VB_TRUE : 0;
    return loaded ? S_OK : S_FALSE;
}
static WINAPI HRESULT doc_get_readyState(xml_doc *d, LONG *out) { (void)d; if (!out) return E_POINTER; *out = 4; return S_OK; }   // READYSTATE_COMPLETE
static WINAPI HRESULT doc_get_parseError(xml_doc *d, void **out) {
    if (!out) return E_POINTER;
    xml_error *e = calloc(1, sizeof *e);
    if (!e) return E_OUTOFMEMORY;
    e->vtbl = g_error_vtbl; e->refs = 1; e->doc = d; doc_addref(d);
    *out = e;
    return S_OK;
}
static WINAPI HRESULT doc_get_url(xml_doc *d, WCHAR **out) { if (!out) return E_POINTER; *out = d->url ? bstr(d->url) : NULL; return d->url ? S_OK : S_FALSE; }
static WINAPI HRESULT doc_get_async(xml_doc *d, VARIANT_BOOL *out) { if (!out) return E_POINTER; *out = d->async ? VB_TRUE : 0; return S_OK; }
static WINAPI HRESULT doc_put_async(xml_doc *d, VARIANT_BOOL v) { d->async = v != 0; return S_OK; }
static WINAPI HRESULT doc_abort(xml_doc *d) { (void)d; return S_OK; }
static WINAPI HRESULT doc_loadXML(xml_doc *d, WCHAR *text, VARIANT_BOOL *ok) {
    int loaded = text ? doc_parse(d, text, bstr_len(text) ? bstr_len(text) : vx_wcslen(text)) : 0;
    free(d->url); d->url = NULL;
    if (ok) *ok = loaded ? VB_TRUE : 0;
    return loaded ? S_OK : S_FALSE;
}
static WINAPI HRESULT doc_save(xml_doc *d, VARIANT dest) {
    char scratch[64];
    const WCHAR *path = variant_text(&dest, scratch, sizeof scratch);
    if (!path) return E_INVALIDARG;
    char *win = vx_utf8_dup(path), unix_path[1024];
    if (!win) return E_OUTOFMEMORY;
    int ok = vx_unix_path_write(win, unix_path, sizeof unix_path, 0);
    FILE *f = ok ? fopen(unix_path, "wb") : NULL;
    if (trace_xml()) vx_log("msxml: save %s -> %s%s", win, ok ? unix_path : "(no path)", f ? "" : " (cannot write)");
    free(win);
    if (!f) return E_FAIL;
    buf b = { 0 };
    if (d->root) write_node(&b, d->root, 0, 1);
    fwrite("\xEF\xBB\xBF", 1, 3, f);
    if (b.s) fwrite(b.s, 1, b.n, f);
    free(b.s);
    fclose(f);
    return S_OK;
}
static WINAPI HRESULT doc_get_validateOnParse(xml_doc *d, VARIANT_BOOL *out) { if (!out) return E_POINTER; *out = d->validate ? VB_TRUE : 0; return S_OK; }
static WINAPI HRESULT doc_put_validateOnParse(xml_doc *d, VARIANT_BOOL v) { d->validate = v != 0; return S_OK; }
static WINAPI HRESULT doc_get_resolveExternals(xml_doc *d, VARIANT_BOOL *out) { if (!out) return E_POINTER; *out = d->resolve_externals ? VB_TRUE : 0; return S_OK; }
static WINAPI HRESULT doc_put_resolveExternals(xml_doc *d, VARIANT_BOOL v) { d->resolve_externals = v != 0; return S_OK; }
static WINAPI HRESULT doc_get_preserveWhiteSpace(xml_doc *d, VARIANT_BOOL *out) { if (!out) return E_POINTER; *out = d->preserve_whitespace ? VB_TRUE : 0; return S_OK; }
static WINAPI HRESULT doc_put_preserveWhiteSpace(xml_doc *d, VARIANT_BOOL v) { d->preserve_whitespace = v != 0; return S_OK; }
static WINAPI HRESULT doc_put_onreadystatechange(xml_doc *d, VARIANT v) { (void)d; (void)v; return S_OK; }
static WINAPI HRESULT doc_put_ondataavailable(xml_doc *d, VARIANT v) { (void)d; (void)v; return S_OK; }
static WINAPI HRESULT doc_put_ontransformnode(xml_doc *d, VARIANT v) { (void)d; (void)v; return S_OK; }
static WINAPI HRESULT doc_get_namespaces(xml_doc *d, void **out) { (void)d; if (out) *out = NULL; return E_NOTIMPL; }
static WINAPI HRESULT doc_get_schemas(xml_doc *d, VARIANT *out) { (void)d; if (!out) return E_POINTER; variant_init(out); out->vt = VT_NULL; return S_FALSE; }
static WINAPI HRESULT doc_putref_schemas(xml_doc *d, VARIANT v) { (void)d; (void)v; return S_OK; }
static WINAPI HRESULT doc_validate(xml_doc *d, void **out) { if (out) return doc_get_parseError(d, out); return S_OK; }
static WINAPI HRESULT doc_setProperty(xml_doc *d, WCHAR *name, VARIANT value) {
    (void)d;
    if (trace_xml()) { char *n = vx_utf8_dup(name); char scratch[64]; char *v = vx_utf8_dup(variant_text(&value, scratch, sizeof scratch)); vx_log("msxml: setProperty(%s, %s) accepted", n ? n : "?", v ? v : "?"); free(n); free(v); }
    return S_OK;
}
static WINAPI HRESULT doc_getProperty(xml_doc *d, WCHAR *name, VARIANT *out) {
    (void)d; (void)name;
    if (!out) return E_POINTER;
    variant_init(out); out->vt = VT_BSTR; out->v.bstr = bstr_ascii("XPath");
    return S_OK;
}
static WINAPI HRESULT doc_validateNode(xml_doc *d, void *node, void **out) { (void)node; if (out) return doc_get_parseError(d, out); return S_OK; }
static WINAPI HRESULT doc_importNode(xml_doc *d, void *node_itf, VARIANT_BOOL deep, void **out) {
    if (!out) return E_POINTER;
    xml_node *n = node_of(node_itf);
    if (!n) return E_INVALIDARG;
    if (n->doc == d) return node_cloneNode(n, deep, out);
    // from another document: the same tree made here
    buf b = { 0 };
    write_node(&b, n, 0, 0);
    xml_doc tmp = { g_doc_vtbl, 1, NULL, NULL, NULL, 0, 0, 0, 1, 0, 0, 0, 0, NULL };
    size_t k = 0;
    WCHAR *w = to_utf16((const uint8_t *)(b.s ? b.s : ""), b.n, &k);
    free(b.s);
    if (!w || !doc_parse(&tmp, w, k)) { free(w); doc_clear(&tmp); return E_FAIL; }
    free(w);
    xml_node *copy = NULL;
    xml_node *src = doc_element(&tmp);
    if (src) {                                                           // re-made in this document
        copy = node_new(d, src->type, src->name, vx_wcslen(src->name), NULL, 0);
        if (copy) {
            for (xml_node *a = src->attrs; a; a = a->next) attr_set(copy, a->name, a->value, a->value ? vx_wcslen(a->value) : 0);
            if (deep) {
                buf inner = { 0 };
                for (xml_node *c = src->first; c; c = c->next) write_node(&inner, c, 0, 0);
                (void)inner;                                             // (children of imported nodes: elements and text, below)
                for (xml_node *c = src->first; c; c = c->next) {
                    xml_node *cc = node_new(d, c->type, c->name, c->name ? vx_wcslen(c->name) : 0, c->value, c->value ? vx_wcslen(c->value) : 0);
                    if (cc) node_append(copy, cc);
                }
                free(inner.s);
            }
        }
    }
    doc_clear(&tmp);
    *out = node_ref(copy);
    return copy ? S_OK : E_FAIL;
}

// ---- XPath (the subset) ----------------------------------------------------------------------------------------------------
// A step: an axis (child, descendant-or-self, self, parent, attribute), a name test, predicates. Evaluated from a set of
// context nodes to the next set, in document order, without duplicates.
typedef struct { xml_node **items; LONG count; } node_set;
static int set_add(node_set *s, xml_node *n) {
    for (LONG i = 0; i < s->count; i++) if (s->items[i] == n) return 1;
    if ((s->count & (s->count + 1)) == 0) {
        xml_node **t = realloc(s->items, ((size_t)s->count + 1) * 2 * sizeof *t);
        if (!t) return 0;
        s->items = t;
    }
    s->items[s->count++] = n;
    return 1;
}
static int name_matches(const xml_node *n, const WCHAR *test, size_t len) {
    if (len == 1 && test[0] == '*') return n->type == NODE_ELEMENT;
    if (weq_ascii(test, len, "node()")) return 1;
    if (weq_ascii(test, len, "text()")) return n->type == NODE_TEXT || n->type == NODE_CDATA_SECTION;
    if (weq_ascii(test, len, "comment()")) return n->type == NODE_COMMENT;
    if (n->type != NODE_ELEMENT || !n->name) return 0;
    return vx_wcslen(n->name) == len && !memcmp(n->name, test, len * 2);
}
static void descendants(xml_node *n, node_set *into) {
    for (xml_node *c = n->first; c; c = c->next) { set_add(into, c); descendants(c, into); }
}
static int predicate_holds(xml_node *n, LONG position, const WCHAR *p, size_t len) {
    while (len && is_space(p[0])) p++, len--;
    while (len && is_space(p[len - 1])) len--;
    if (!len) return 1;
    int number = 1;
    for (size_t i = 0; i < len; i++) if (p[i] < '0' || p[i] > '9') number = 0;
    if (number) { LONG want = 0; for (size_t i = 0; i < len; i++) want = want * 10 + (p[i] - '0'); return position == want; }
    if (weq_ascii(p, len, "last()")) return 0;                            // (not known here)
    // name or @name, optionally = 'value' / "value"
    size_t i = 0;
    int attribute = p[0] == '@';
    if (attribute) i = 1;
    size_t nstart = i;
    while (i < len && !is_space(p[i]) && p[i] != '=' && p[i] != '!') i++;
    size_t nlen = i - nstart;
    while (i < len && is_space(p[i])) i++;
    int negate = 0;
    if (i < len && p[i] == '!') { negate = 1; i++; }
    const WCHAR *value = NULL; size_t vlen = 0;
    if (i < len && p[i] == '=') {
        i++;
        while (i < len && is_space(p[i])) i++;
        if (i < len && (p[i] == '\'' || p[i] == '"')) {
            WCHAR q = p[i++];
            value = p + i;
            while (i < len && p[i] != q) i++;
            vlen = (size_t)(p + i - value);
        } else { value = p + i; vlen = len - i; }
    }
    int holds = 0;
    if (attribute) {
        xml_node *a = NULL;
        if (nlen == 1 && p[nstart] == '*') a = n->attrs;
        else for (xml_node *k = n->attrs; k; k = k->next) if (vx_wcslen(k->name) == nlen && !memcmp(k->name, p + nstart, nlen * 2)) { a = k; break; }
        holds = a && (!value || (vx_wcslen(a->value) == vlen && !memcmp(a->value, value, vlen * 2)));
    } else {
        for (xml_node *c = n->first; c && !holds; c = c->next) {
            if (!name_matches(c, p + nstart, nlen)) continue;
            if (!value) { holds = 1; break; }
            WCHAR *t = node_text_bstr(c);
            holds = t && bstr_len(t) == vlen && !memcmp(t, value, vlen * 2);
            bstr_free(t);
        }
    }
    return negate ? !holds : holds;
}
static int select_nodes(xml_node *context, const WCHAR *query, xml_list *into, int first_only) {
    if (!query) return 0;
    size_t n = vx_wcslen(query), i = 0;
    node_set current = { 0 };
    if (i < n && query[i] == '/') {                                       // absolute: from the document node
        xml_node *root = context;
        while (root->parent) root = root->parent;
        if (root->type != NODE_DOCUMENT && root->doc->root) root = root->doc->root;
        set_add(&current, root);
    } else set_add(&current, context);
    int ok = 1;
    while (ok && i < n) {
        int descend = 0;
        if (query[i] == '/') { i++; if (i < n && query[i] == '/') { descend = 1; i++; } }
        if (i >= n) break;
        while (i < n && is_space(query[i])) i++;
        if (i < n && query[i] == '|') { ok = 0; break; }                     // unions: not supported
        int axis_attr = 0, axis_self = 0, axis_parent = 0;
        if (query[i] == '@') { axis_attr = 1; i++; }
        else if (query[i] == '.' && (i + 1 >= n || query[i + 1] == '/' || query[i + 1] == '[')) { axis_self = 1; i++; }
        else if (query[i] == '.' && i + 1 < n && query[i + 1] == '.') { axis_parent = 1; i += 2; }
        else if (weq_ascii(query + i, 11 < n - i ? 11 : n - i, "descendant:") && i + 11 <= n) { descend = 1; i += 11; if (i < n && query[i] == ':') i++; if (weq_ascii(query + i, 8 <= n - i ? 8 : n - i, "or-self:")) i += 9; }
        else if (i + 6 <= n && weq_ascii(query + i, 6, "child:")) { i += 6; if (i < n && query[i] == ':') i++; }
        else if (i + 10 <= n && weq_ascii(query + i, 10, "attribute:")) { axis_attr = 1; i += 10; if (i < n && query[i] == ':') i++; }
        size_t tstart = i;
        while (i < n && query[i] != '/' && query[i] != '[') i++;
        size_t tlen = i - tstart;
        while (tlen && is_space(query[tstart + tlen - 1])) tlen--;
        node_set next = { 0 };
        for (LONG c = 0; c < current.count; c++) {
            xml_node *ctx = current.items[c];
            node_set candidates = { 0 };
            if (axis_self) set_add(&candidates, ctx);
            else if (axis_parent) { if (ctx->parent) set_add(&candidates, ctx->parent); }
            else if (axis_attr) { for (xml_node *a = ctx->attrs; a; a = a->next) set_add(&candidates, a); }
            else if (descend) { set_add(&candidates, ctx); descendants(ctx, &candidates); }
            else for (xml_node *k = ctx->first; k; k = k->next) set_add(&candidates, k);
            LONG position = 0;
            for (LONG k = 0; k < candidates.count; k++) {
                xml_node *cand = candidates.items[k];
                if (axis_attr) {
                    if (!(tlen == 1 && query[tstart] == '*') && !(vx_wcslen(cand->name) == tlen && !memcmp(cand->name, query + tstart, tlen * 2))) continue;
                } else if (!axis_self && !axis_parent && tlen && !name_matches(cand, query + tstart, tlen)) continue;
                else if ((axis_self || axis_parent) && tlen && !(tlen == 1 && query[tstart] == '*') && !weq_ascii(query + tstart, tlen, "node()") && !name_matches(cand, query + tstart, tlen)) continue;
                position++;
                // predicates
                size_t j = i;
                int holds = 1;
                while (holds && j < n && query[j] == '[') {
                    size_t pstart = ++j;
                    int depth = 1;
                    while (j < n && depth) { if (query[j] == '[') depth++; else if (query[j] == ']') depth--; if (depth) j++; }
                    holds = predicate_holds(cand, position, query + pstart, j - pstart);
                    if (j < n) j++;
                }
                if (holds) set_add(&next, cand);
            }
            free(candidates.items);
        }
        while (i < n && query[i] == '[') {                                  // past the predicates
            int depth = 1; i++;
            while (i < n && depth) { if (query[i] == '[') depth++; else if (query[i] == ']') depth--; i++; }
        }
        free(current.items);
        current = next;
    }
    for (LONG k = 0; k < current.count && (!first_only || !into->count); k++) list_add(into, current.items[k]);
    free(current.items);
    return ok;
}

// ---- vtables ------------------------------------------------------------------------------------------------------------------
#define DISPATCH_PART(qi, addref, release) (void *)qi, (void *)addref, (void *)release, (void *)disp_GetTypeInfoCount, (void *)disp_GetTypeInfo, (void *)disp_GetIDsOfNames, (void *)disp_Invoke
#define NODE_PART \
    (void *)node_get_nodeName, (void *)node_get_nodeValue, (void *)node_put_nodeValue, (void *)node_get_nodeType, (void *)node_get_parentNode, \
    (void *)node_get_childNodes, (void *)node_get_firstChild, (void *)node_get_lastChild, (void *)node_get_previousSibling, (void *)node_get_nextSibling, \
    (void *)node_get_attributes, (void *)node_insertBefore, (void *)node_replaceChild, (void *)node_removeChild, (void *)node_appendChild, \
    (void *)node_hasChildNodes, (void *)node_get_ownerDocument, (void *)node_cloneNode, (void *)node_get_nodeTypeString, (void *)node_get_text, \
    (void *)node_put_text, (void *)node_get_specified, (void *)node_get_definition, (void *)node_get_nodeTypedValue, (void *)node_put_nodeTypedValue, \
    (void *)node_get_dataType, (void *)node_put_dataType, (void *)node_get_xml, (void *)node_transformNode, (void *)node_selectNodes, \
    (void *)node_selectSingleNode, (void *)node_get_parsed, (void *)node_get_namespaceURI, (void *)node_get_prefix, (void *)node_get_baseName, \
    (void *)node_transformNodeToObject
static const void *g_element_vtbl[52] = {
    DISPATCH_PART(node_QueryInterface, node_AddRef, node_Release), NODE_PART,
    (void *)element_get_tagName, (void *)element_getAttribute, (void *)element_setAttribute, (void *)element_removeAttribute,
    (void *)element_getAttributeNode, (void *)element_setAttributeNode, (void *)element_removeAttributeNode,
    (void *)element_getElementsByTagName, (void *)element_normalize,
};
static const void *g_attr_vtbl[46] = {
    DISPATCH_PART(node_QueryInterface, node_AddRef, node_Release), NODE_PART,
    (void *)attr_get_name, (void *)attr_get_value, (void *)attr_put_value,
};
static const void *g_text_vtbl[52] = {
    DISPATCH_PART(node_QueryInterface, node_AddRef, node_Release), NODE_PART,
    (void *)text_get_data, (void *)text_put_data, (void *)text_get_length, (void *)text_substringData, (void *)text_appendData,
    (void *)text_insertData, (void *)text_deleteData, (void *)text_replaceData, (void *)text_splitText,
};
static const void *g_other_vtbl[51] = {                                     // comments (character data), processing instructions
    DISPATCH_PART(node_QueryInterface, node_AddRef, node_Release), NODE_PART,
    (void *)text_get_data, (void *)text_put_data, (void *)text_get_length, (void *)text_substringData, (void *)text_appendData,
    (void *)text_insertData, (void *)text_deleteData, (void *)text_replaceData,
};
static const void *g_doc_vtbl[84] = {
    DISPATCH_PART(doc_QueryInterface, doc_AddRef, doc_Release),
    (void *)doc_get_nodeName, (void *)doc_get_nodeValue, (void *)doc_put_nodeValue, (void *)doc_get_nodeType, (void *)doc_get_parentNode,
    (void *)doc_get_childNodes, (void *)doc_get_firstChild, (void *)doc_get_lastChild, (void *)doc_get_previousSibling, (void *)doc_get_nextSibling,
    (void *)doc_get_attributes, (void *)doc_insertBefore, (void *)doc_replaceChild, (void *)doc_removeChild, (void *)doc_appendChild,
    (void *)doc_hasChildNodes, (void *)doc_get_ownerDocument, (void *)doc_cloneNode, (void *)doc_get_nodeTypeString, (void *)doc_get_text,
    (void *)doc_put_text, (void *)doc_get_specified, (void *)doc_get_definition, (void *)doc_get_nodeTypedValue, (void *)doc_put_nodeTypedValue,
    (void *)doc_get_dataType, (void *)doc_put_dataType, (void *)doc_get_xml, (void *)doc_transformNode, (void *)doc_selectNodes,
    (void *)doc_selectSingleNode, (void *)doc_get_parsed, (void *)doc_get_namespaceURI, (void *)doc_get_prefix, (void *)doc_get_baseName,
    (void *)doc_transformNodeToObject,
    (void *)doc_get_doctype, (void *)doc_get_implementation, (void *)doc_get_documentElement, (void *)doc_putref_documentElement,
    (void *)doc_createElement, (void *)doc_createDocumentFragment, (void *)doc_createTextNode, (void *)doc_createComment,
    (void *)doc_createCDATASection, (void *)doc_createProcessingInstruction, (void *)doc_createAttribute, (void *)doc_createEntityReference,
    (void *)doc_getElementsByTagName, (void *)doc_createNode, (void *)doc_nodeFromID, (void *)doc_load, (void *)doc_get_readyState,
    (void *)doc_get_parseError, (void *)doc_get_url, (void *)doc_get_async, (void *)doc_put_async, (void *)doc_abort, (void *)doc_loadXML,
    (void *)doc_save, (void *)doc_get_validateOnParse, (void *)doc_put_validateOnParse, (void *)doc_get_resolveExternals,
    (void *)doc_put_resolveExternals, (void *)doc_get_preserveWhiteSpace, (void *)doc_put_preserveWhiteSpace,
    (void *)doc_put_onreadystatechange, (void *)doc_put_ondataavailable, (void *)doc_put_ontransformnode,
    (void *)doc_get_namespaces, (void *)doc_get_schemas, (void *)doc_putref_schemas, (void *)doc_validate, (void *)doc_setProperty, (void *)doc_getProperty,
    (void *)doc_validateNode, (void *)doc_importNode,
};
static const void *g_list_vtbl[12] = {
    DISPATCH_PART(list_QueryInterface, list_AddRef, list_Release),
    (void *)list_get_item, (void *)list_get_length, (void *)list_nextNode, (void *)list_reset, (void *)list_get__newEnum,
};
static const void *g_map_vtbl[17] = {
    DISPATCH_PART(list_QueryInterface, list_AddRef, list_Release),
    (void *)map_getNamedItem, (void *)map_setNamedItem, (void *)map_removeNamedItem, (void *)list_get_item, (void *)list_get_length,
    (void *)map_getQualifiedItem, (void *)map_removeQualifiedItem, (void *)list_nextNode, (void *)list_reset, (void *)list_get__newEnum,
};
static const void *g_error_vtbl[14] = {
    DISPATCH_PART(error_QueryInterface, error_AddRef, error_Release),
    (void *)error_get_errorCode, (void *)error_get_url, (void *)error_get_reason, (void *)error_get_srcText, (void *)error_get_line,
    (void *)error_get_linepos, (void *)error_get_filepos,
};

static HRESULT create_document(const void *iid, void **out) {
    xml_doc *d = calloc(1, sizeof *d);
    if (!d) return E_OUTOFMEMORY;
    d->vtbl = g_doc_vtbl; d->refs = 1; d->async = 1; d->resolve_externals = 1;
    HRESULT r = doc_QueryInterface(d, (const GUID *)iid, out);
    doc_Release(d);
    if (trace_xml()) vx_log("msxml: DOMDocument made = %#x", (unsigned)r);
    return r;
}
void vx_msxml_init(void) {
    static const GUID *const classes[] = { &CLSID_DOMDocument, &CLSID_DOMDocument26, &CLSID_DOMDocument30, &CLSID_DOMDocument40, &CLSID_DOMDocument60,
                                           &CLSID_FreeThreadedDOMDocument, &CLSID_FreeThreadedDOMDocument30, &CLSID_FreeThreadedDOMDocument60 };
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++) vx_com_register_class(classes[i], create_document);
}

#ifdef __aarch64__
#include "thunks/msxml.c.inc"
#endif
