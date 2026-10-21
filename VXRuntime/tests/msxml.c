// MSXML's DOMDocument (msxml.c): parsing, the tree, attributes, the XPath subset, errors, building and saving a document,
// loading it back from a file. The XML is shaped like a game's control bindings (Elite Dangerous reads those through it).
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <msxml.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
static const GUID my_CLSID_DOMDocument = { 0x2933BF90, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID my_CLSID_DOMDocument60 = { 0x88D96A05, 0xF192, 0x11D4, { 0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5 } };
static const GUID my_IID_IXMLDOMDocument = { 0x2933BF81, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID my_IID_IXMLDOMDocument2 = { 0x2933BF95, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID my_IID_IXMLDOMElement = { 0x2933BF86, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };
static const GUID my_IID_IXMLDOMNode = { 0x2933BF80, 0x7B36, 0x11D2, { 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60 } };

static const WCHAR *binds =
    L"\xFEFF<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    L"<Root PresetName=\"DualShock4Controller\" MajorVersion=\"4\" MinorVersion=\"0\">\r\n"
    L"\t<!-- the ship's yaw -->\r\n"
    L"\t<YawAxisRaw>\r\n"
    L"\t\t<Binding Device=\"DualShock4\" Key=\"Joy_XAxis\" />\r\n"
    L"\t\t<Inverted Value=\"0\" />\r\n"
    L"\t\t<Deadzone Value=\"0.00000000\" />\r\n"
    L"\t</YawAxisRaw>\r\n"
    L"\t<UI_Select>\r\n"
    L"\t\t<Primary Device=\"DualShock4\" Key=\"Joy_2\" />\r\n"
    L"\t\t<Secondary Device=\"Keyboard\" Key=\"Key_Space\" />\r\n"
    L"\t</UI_Select>\r\n"
    L"\t<UI_Back>\r\n"
    L"\t\t<Primary Device=\"DualShock4\" Key=\"Joy_3\" />\r\n"
    L"\t\t<Secondary Device=\"{NoDevice}\" Key=\"\" />\r\n"
    L"\t</UI_Back>\r\n"
    L"\t<Note>Tom &amp; Jerry &lt;3 &#x41;&#66;<![CDATA[<raw>]]></Note>\r\n"
    L"\t<KeyboardLayout>en-GB</KeyboardLayout>\r\n"
    L"</Root>\r\n";

static int attr_is(IXMLDOMNode *node, const WCHAR *name, const WCHAR *value) {
    IXMLDOMElement *e = NULL;
    if (!node || IXMLDOMNode_QueryInterface(node, &my_IID_IXMLDOMElement, (void **)&e) != S_OK) return 0;
    VARIANT v; VariantInit(&v);
    BSTR n = SysAllocString(name);
    HRESULT r = IXMLDOMElement_getAttribute(e, n, &v);
    int ok = r == S_OK && v.vt == VT_BSTR && !wcscmp(v.bstrVal, value);
    VariantClear(&v); SysFreeString(n); IXMLDOMElement_Release(e);
    return ok;
}
static long count(IXMLDOMNodeList *l) { long n = -1; if (l) IXMLDOMNodeList_get_length(l, &n); return n; }

int main(void) {
    CHECK(CoInitializeEx(NULL, COINIT_MULTITHREADED) == S_OK);
    IXMLDOMDocument *doc = NULL;
    CHECK(CoCreateInstance(&my_CLSID_DOMDocument, NULL, CLSCTX_INPROC_SERVER, &my_IID_IXMLDOMDocument, (void **)&doc) == S_OK && doc);
    IUnknown *doc2 = NULL;
    CHECK(IXMLDOMDocument_QueryInterface(doc, &my_IID_IXMLDOMDocument2, (void **)&doc2) == S_OK && (void *)doc2 == (void *)doc);
    if (doc2) IUnknown_Release(doc2);
    IXMLDOMDocument *doc60 = NULL;
    CHECK(CoCreateInstance(&my_CLSID_DOMDocument60, NULL, CLSCTX_INPROC_SERVER, &my_IID_IXMLDOMDocument, (void **)&doc60) == S_OK && doc60);
    if (doc60) IXMLDOMDocument_Release(doc60);

    // Parsing and the tree.
    VARIANT_BOOL ok = 0;
    BSTR text = SysAllocString(binds);
    CHECK(IXMLDOMDocument_loadXML(doc, text, &ok) == S_OK && ok == VARIANT_TRUE);
    SysFreeString(text);
    IXMLDOMElement *root = NULL;
    CHECK(IXMLDOMDocument_get_documentElement(doc, &root) == S_OK && root);
    BSTR tag = NULL;
    CHECK(IXMLDOMElement_get_tagName(root, &tag) == S_OK && !wcscmp(tag, L"Root"));
    SysFreeString(tag);
    CHECK(attr_is((IXMLDOMNode *)root, L"PresetName", L"DualShock4Controller"));
    CHECK(attr_is((IXMLDOMNode *)root, L"MajorVersion", L"4"));
    VARIANT missing; VariantInit(&missing);
    BSTR none = SysAllocString(L"Nothing");
    CHECK(IXMLDOMElement_getAttribute(root, none, &missing) == S_FALSE && missing.vt == VT_NULL);
    SysFreeString(none);
    IXMLDOMNodeList *children = NULL;
    CHECK(IXMLDOMElement_get_childNodes(root, &children) == S_OK && count(children) == 6);   // comment, 3 groups, Note, KeyboardLayout
    IXMLDOMNode *first = NULL;
    CHECK(IXMLDOMNodeList_get_item(children, 0, &first) == S_OK && first);
    DOMNodeType type = 0;
    CHECK(IXMLDOMNode_get_nodeType(first, &type) == S_OK && type == NODE_COMMENT);
    BSTR name = NULL;
    CHECK(IXMLDOMNode_get_nodeName(first, &name) == S_OK && !wcscmp(name, L"#comment"));
    SysFreeString(name);
    IXMLDOMNode_Release(first);
    IXMLDOMNodeList_Release(children);

    // The XPath subset.
    IXMLDOMNodeList *groups = NULL;
    BSTR q = SysAllocString(L"*");
    CHECK(IXMLDOMElement_selectNodes(root, q, &groups) == S_OK && count(groups) == 5);
    SysFreeString(q);
    IXMLDOMNodeList_Release(groups);
    q = SysAllocString(L"Root/UI_Select/Primary");
    IXMLDOMNode *primary = NULL;
    CHECK(IXMLDOMDocument_selectSingleNode(doc, q, &primary) == S_OK && attr_is(primary, L"Key", L"Joy_2"));
    SysFreeString(q);
    if (primary) IXMLDOMNode_Release(primary);
    q = SysAllocString(L"//Primary[@Device='DualShock4']");
    IXMLDOMNodeList *primaries = NULL;
    CHECK(IXMLDOMDocument_selectNodes(doc, q, &primaries) == S_OK && count(primaries) == 2);
    SysFreeString(q);
    IXMLDOMNode *second = NULL;
    CHECK(IXMLDOMNodeList_get_item(primaries, 1, &second) == S_OK && attr_is(second, L"Key", L"Joy_3"));
    IXMLDOMNode *parent = NULL;
    BSTR pname = NULL;
    CHECK(IXMLDOMNode_get_parentNode(second, &parent) == S_OK && IXMLDOMNode_get_nodeName(parent, &pname) == S_OK && !wcscmp(pname, L"UI_Back"));
    SysFreeString(pname);
    if (parent) IXMLDOMNode_Release(parent);
    if (second) IXMLDOMNode_Release(second);
    IXMLDOMNodeList_Release(primaries);
    q = SysAllocString(L"./UI_Back/Secondary/@Device");
    IXMLDOMNode *device = NULL;
    VARIANT value; VariantInit(&value);
    CHECK(IXMLDOMElement_selectSingleNode(root, q, &device) == S_OK && IXMLDOMNode_get_nodeValue(device, &value) == S_OK && value.vt == VT_BSTR && !wcscmp(value.bstrVal, L"{NoDevice}"));
    VariantClear(&value); SysFreeString(q);
    if (device) IXMLDOMNode_Release(device);
    q = SysAllocString(L"Root/Nowhere");
    IXMLDOMNode *nowhere = (IXMLDOMNode *)1;
    CHECK(IXMLDOMDocument_selectSingleNode(doc, q, &nowhere) == S_FALSE && nowhere == NULL);
    SysFreeString(q);
    q = SysAllocString(L"Root/*[2]");
    IXMLDOMNode *by_position = NULL;
    BSTR bname = NULL;
    CHECK(IXMLDOMDocument_selectSingleNode(doc, q, &by_position) == S_OK && IXMLDOMNode_get_nodeName(by_position, &bname) == S_OK && !wcscmp(bname, L"UI_Select"));
    SysFreeString(bname); SysFreeString(q);
    if (by_position) IXMLDOMNode_Release(by_position);

    // Text, entities, CDATA; the attributes map.
    q = SysAllocString(L"Root/Note");
    IXMLDOMNode *note = NULL;
    BSTR note_text = NULL;
    CHECK(IXMLDOMDocument_selectSingleNode(doc, q, &note) == S_OK && IXMLDOMNode_get_text(note, &note_text) == S_OK && !wcscmp(note_text, L"Tom & Jerry <3 AB<raw>"));
    SysFreeString(note_text); SysFreeString(q);
    if (note) IXMLDOMNode_Release(note);
    IXMLDOMNamedNodeMap *attrs = NULL;
    CHECK(IXMLDOMElement_get_attributes(root, &attrs) == S_OK && attrs);
    long attr_count = 0;
    CHECK(IXMLDOMNamedNodeMap_get_length(attrs, &attr_count) == S_OK && attr_count == 3);
    BSTR minor = SysAllocString(L"MinorVersion");
    IXMLDOMNode *minor_node = NULL;
    VARIANT minor_value; VariantInit(&minor_value);
    CHECK(IXMLDOMNamedNodeMap_getNamedItem(attrs, minor, &minor_node) == S_OK && IXMLDOMNode_get_nodeValue(minor_node, &minor_value) == S_OK && !wcscmp(minor_value.bstrVal, L"0"));
    VariantClear(&minor_value); SysFreeString(minor);
    if (minor_node) IXMLDOMNode_Release(minor_node);
    IXMLDOMNamedNodeMap_Release(attrs);
    BSTR layout_q = SysAllocString(L"//KeyboardLayout");
    IXMLDOMNode *layout = NULL;
    BSTR layout_text = NULL;
    CHECK(IXMLDOMDocument_selectSingleNode(doc, layout_q, &layout) == S_OK && IXMLDOMNode_get_text(layout, &layout_text) == S_OK && !wcscmp(layout_text, L"en-GB"));
    SysFreeString(layout_text); SysFreeString(layout_q);
    if (layout) IXMLDOMNode_Release(layout);
    IXMLDOMElement_Release(root);

    // A parse error is reported, with its line.
    BSTR bad = SysAllocString(L"<Root><Open></Root>");
    CHECK(IXMLDOMDocument_loadXML(doc, bad, &ok) == S_FALSE && ok == VARIANT_FALSE);
    SysFreeString(bad);
    IXMLDOMParseError *error = NULL;
    long code = 0, line = 0;
    CHECK(IXMLDOMDocument_get_parseError(doc, &error) == S_OK && IXMLDOMParseError_get_errorCode(error, &code) == S_OK && code != 0);
    CHECK(IXMLDOMParseError_get_line(error, &line) == S_OK && line == 1);
    if (error) IXMLDOMParseError_Release(error);
    IXMLDOMElement *no_root = (IXMLDOMElement *)1;
    CHECK(IXMLDOMDocument_get_documentElement(doc, &no_root) == S_FALSE && no_root == NULL);

    // Building a document, saving it, loading it back from the file.
    BSTR empty = SysAllocString(L"<Custom/>");
    CHECK(IXMLDOMDocument_loadXML(doc, empty, &ok) == S_OK && ok == VARIANT_TRUE);
    SysFreeString(empty);
    IXMLDOMElement *custom = NULL, *binding = NULL;
    CHECK(IXMLDOMDocument_get_documentElement(doc, &custom) == S_OK && custom);
    BSTR bname2 = SysAllocString(L"Binding");
    CHECK(IXMLDOMDocument_createElement(doc, bname2, &binding) == S_OK && binding);
    SysFreeString(bname2);
    VARIANT dev; VariantInit(&dev); dev.vt = VT_BSTR; dev.bstrVal = SysAllocString(L"Keyboard");
    BSTR dname = SysAllocString(L"Device");
    CHECK(IXMLDOMElement_setAttribute(binding, dname, dev) == S_OK);
    VariantClear(&dev); SysFreeString(dname);
    VARIANT num; VariantInit(&num); num.vt = VT_I4; num.lVal = 7;
    BSTR nname = SysAllocString(L"Count");
    CHECK(IXMLDOMElement_setAttribute(binding, nname, num) == S_OK);
    SysFreeString(nname);
    IXMLDOMText *tnode = NULL;
    BSTR tvalue = SysAllocString(L"a < b");
    CHECK(IXMLDOMDocument_createTextNode(doc, tvalue, &tnode) == S_OK && tnode);
    SysFreeString(tvalue);
    CHECK(IXMLDOMElement_appendChild(binding, (IXMLDOMNode *)tnode, NULL) == S_OK);
    CHECK(IXMLDOMElement_appendChild(custom, (IXMLDOMNode *)binding, NULL) == S_OK);
    IXMLDOMText_Release(tnode);
    BSTR xml = NULL;
    CHECK(IXMLDOMElement_get_xml(custom, &xml) == S_OK && wcsstr(xml, L"<Binding Device=\"Keyboard\" Count=\"7\">a &lt; b</Binding>"));
    SysFreeString(xml);
    IXMLDOMElement_Release(binding); IXMLDOMElement_Release(custom);
    VARIANT where; VariantInit(&where); where.vt = VT_BSTR; where.bstrVal = SysAllocString(L"C:\\msxml_test.xml");
    CHECK(IXMLDOMDocument_save(doc, where) == S_OK);
    CHECK(IXMLDOMDocument_load(doc, where, &ok) == S_OK && ok == VARIANT_TRUE);
    BSTR url = NULL;
    CHECK(IXMLDOMDocument_get_url(doc, &url) == S_OK && !wcscmp(url, L"C:\\msxml_test.xml"));
    SysFreeString(url);
    q = SysAllocString(L"/Custom/Binding");
    IXMLDOMNode *back = NULL;
    BSTR back_text = NULL;
    CHECK(IXMLDOMDocument_selectSingleNode(doc, q, &back) == S_OK && attr_is(back, L"Count", L"7") && IXMLDOMNode_get_text(back, &back_text) == S_OK && !wcscmp(back_text, L"a < b"));
    SysFreeString(back_text); SysFreeString(q);
    if (back) IXMLDOMNode_Release(back);
    DeleteFileW(L"C:\\msxml_test.xml");
    VARIANT nowhere_v; VariantInit(&nowhere_v); nowhere_v.vt = VT_BSTR; nowhere_v.bstrVal = SysAllocString(L"C:\\no_such_folder\\none.xml");
    CHECK(IXMLDOMDocument_load(doc, nowhere_v, &ok) == S_FALSE && ok == VARIANT_FALSE);
    VariantClear(&nowhere_v); VariantClear(&where);
    IXMLDOMDocument_Release(doc);
    CoUninitialize();
    printf("msxml: DOMDocument, tree, XPath subset, errors, save and load -> %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
