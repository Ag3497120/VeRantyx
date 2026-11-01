#!/usr/bin/env python3
# Generates src/d3d_tables.h: the method order (vtable layout) and IID of each COM interface this runtime
# implements, read from MinGW-w64's widl-generated headers (d3d11.h, d3d11_1.h, dxgi.h, dxgi1_2.h ... dxgi1_6.h), and for every method a
# stub that reports itself as not implemented (d3d_not_implemented, defined by the includer).
#   tools/gen_com_tables.py <mingw include dir> > src/d3d_tables.h
#   tools/gen_com_tables.py <mingw include dir> audio > src/audio_tables.h
#   tools/gen_com_tables.py <mingw include dir> d3d12 > src/d3d12_tables.h
#   tools/gen_com_tables.py <mingw include dir> dwrite > src/dwrite_tables.h
import re, sys

AUDIO = {
    "mmdeviceapi.h": ["IMMDeviceEnumerator", "IMMDevice", "IMMDeviceCollection"],
    "audioclient.h": ["IAudioClient", "IAudioRenderClient", "IAudioClock", "ISimpleAudioVolume"],
    "propsys.h": ["IPropertyStore"],
}
INTERFACES = {
    "d3d11.h": ["ID3D11Device", "ID3D11DeviceContext", "ID3D11Texture2D", "ID3D11Buffer", "ID3D11RenderTargetView",
                "ID3D11DepthStencilView", "ID3D11ShaderResourceView", "ID3D11Resource", "ID3D11DeviceChild",
                "ID3D11VertexShader", "ID3D11PixelShader", "ID3D11InputLayout", "ID3D11BlendState",
                "ID3D11DepthStencilState", "ID3D11RasterizerState", "ID3D11SamplerState", "ID3D11ComputeShader",
                "ID3D11UnorderedAccessView", "ID3D11Texture1D", "ID3D11Texture3D", "ID3D11GeometryShader",
                "ID3D11HullShader", "ID3D11DomainShader", "ID3D11Query", "ID3D11Predicate", "ID3D11Asynchronous",
                "ID3D11Counter", "ID3D11ClassLinkage", "ID3D11CommandList"],
    "d3d11_1.h": ["ID3D11Device1", "ID3D11DeviceContext1", "ID3D11BlendState1", "ID3D11RasterizerState1",
                  "ID3DUserDefinedAnnotation"],
    "dxgi.h": ["IDXGIFactory", "IDXGIFactory1", "IDXGIAdapter", "IDXGIAdapter1", "IDXGIOutput", "IDXGISwapChain",
               "IDXGIDevice", "IDXGIDevice1", "IDXGIObject", "IDXGIDeviceSubObject", "IDXGISurface", "IDXGIResource",
               "IDXGIKeyedMutex"],
    "dxgi1_2.h": ["IDXGIFactory2", "IDXGISwapChain1", "IDXGIAdapter2", "IDXGIResource1", "IDXGIOutput1", "IDXGIDevice2"],
    "dxgi1_3.h": ["IDXGIFactory3", "IDXGISwapChain2", "IDXGIOutput2", "IDXGIOutput3", "IDXGIDevice3"],
    "dxgi1_4.h": ["IDXGIAdapter3", "IDXGIFactory4", "IDXGISwapChain3", "IDXGIOutput4"],
    "dxgi1_5.h": ["IDXGIFactory5", "IDXGISwapChain4", "IDXGIOutput5", "IDXGIDevice4"],
    "dxgi1_6.h": ["IDXGIFactory6", "IDXGIFactory7", "IDXGIAdapter4", "IDXGIOutput6"],
    "d3d11on12.h": ["ID3D11On12Device", "ID3D11On12Device1", "ID3D11On12Device2"],
}

def methods(text, name):
    m = re.search(r"typedef struct %sVtbl \{(.*?)\} %sVtbl;" % (name, name), text, re.S)
    if not m:
        sys.exit("no vtable for " + name)
    return re.findall(r"\(STDMETHODCALLTYPE \*(\w+)\)", m.group(1))

def uuid(text, name):
    m = re.search(r"__CRT_UUID_DECL\(%s, ([^)]*)\)" % name, text)
    if not m:
        return None                     # not a COM interface (XAudio2's voices): no IID
    v = [int(x.strip(), 16) for x in m.group(1).split(",")]
    return "{ 0x%08x, 0x%04x, 0x%04x, { %s } }" % (v[0], v[1], v[2], ", ".join("0x%02x" % b for b in v[3:]))

XAUDIO2 = {
    "xaudio2.h": ["IXAudio2", "IXAudio2SourceVoice", "IXAudio2SubmixVoice", "IXAudio2MasteringVoice"],
}
# Direct3D 12 (src/d3d12.m): the newest version of each interface is implemented with one table (each version's
# methods are the previous one's and more), the older versions are there for their IIDs.
D3D12 = {
    "d3d12.h": ["ID3D12Object", "ID3D12DeviceChild", "ID3D12Pageable", "ID3D12Heap", "ID3D12Heap1", "ID3D12Resource",
                "ID3D12Resource1", "ID3D12Resource2", "ID3D12CommandList", "ID3D12DescriptorHeap", "ID3D12QueryHeap",
                "ID3D12CommandSignature", "ID3D12GraphicsCommandList", "ID3D12GraphicsCommandList1",
                "ID3D12GraphicsCommandList2", "ID3D12GraphicsCommandList3", "ID3D12GraphicsCommandList4",
                "ID3D12GraphicsCommandList5", "ID3D12GraphicsCommandList6", "ID3D12GraphicsCommandList7",
                "ID3D12CommandQueue", "ID3D12RootSignature", "ID3D12PipelineState", "ID3D12Fence", "ID3D12Fence1",
                "ID3D12CommandAllocator", "ID3D12Device", "ID3D12Device1", "ID3D12Device2", "ID3D12Device3",
                "ID3D12Device4", "ID3D12Device5", "ID3D12Device6", "ID3D12Device7", "ID3D12Device8", "ID3D12Device9",
                "ID3D12Device10", "ID3D12RootSignatureDeserializer", "ID3D12VersionedRootSignatureDeserializer",
                "ID3D12PipelineLibrary", "ID3D12PipelineLibrary1"],
    "d3d12sdklayers.h": ["ID3D12Debug", "ID3D12Debug1", "ID3D12Debug2", "ID3D12Debug3", "ID3D12Debug4", "ID3D12Debug5"],
    "d3dcommon.h": ["ID3D10Blob"],
}
# DirectWrite (src/dwrite.m): one table per object, its newest version implemented (the older versions are there for
# their IIDs); "?" marks interfaces wanted for their IIDs only (also versions not offered: QueryInterface names them
# in traces and answers E_NOINTERFACE).
DWRITE = {
    "dwrite.h": ["?IDWriteFactory", "IDWriteLocalFontFileLoader", "IDWriteFontFileStream", "IDWriteFontFile",
                 "IDWriteLocalizedStrings", "?IDWriteRenderingParams", "?IDWriteFontFace", "?IDWriteFont", "?IDWriteFontList",
                 "?IDWriteFontFamily", "?IDWriteFontCollection", "IDWriteGlyphRunAnalysis", "?IDWriteGdiInterop",
                 "?IDWriteFontFileLoader", "?IDWriteFontCollectionLoader", "?IDWriteFontFileEnumerator",
                 "IDWriteTextFormat", "IDWriteTextLayout", "IDWriteTypography", "IDWriteBitmapRenderTarget",
                 "?IDWriteTextAnalysisSource", "?IDWriteTextAnalyzer", "?IDWriteTextRenderer", "?IDWritePixelSnapping",
                 "?IDWriteInlineObject", "?IDWriteNumberSubstitution"],
    "dwrite_1.h": ["?IDWriteFactory1", "?IDWriteFontFace1", "?IDWriteFont1", "?IDWriteRenderingParams1", "?IDWriteTextLayout1",
                   "?IDWriteBitmapRenderTarget1"],
    "dwrite_2.h": ["?IDWriteFactory2", "?IDWriteFontFace2", "?IDWriteFont2", "?IDWriteRenderingParams2", "?IDWriteFontFallback",
                   "?IDWriteTextFormat1", "?IDWriteTextLayout2", "IDWriteFontFallbackBuilder"],
    "dwrite_3.h": ["?IDWriteFactory3", "?IDWriteFactory4", "?IDWriteFactory5", "?IDWriteFactory6", "IDWriteFactory7",
                   "?IDWriteFontFace3", "?IDWriteFontFace4", "IDWriteFontFace5", "IDWriteFont3",
                   "IDWriteRenderingParams3", "?IDWriteFontCollection1", "?IDWriteFontCollection2", "IDWriteFontCollection3",
                   "?IDWriteFontFamily1", "IDWriteFontFamily2", "?IDWriteFontList1", "IDWriteFontList2",
                   "IDWriteFontFallback1", "IDWriteGdiInterop1", "IDWriteInMemoryFontFileLoader", "?IDWriteFontSet",
                   "?IDWriteFontSet1", "?IDWriteFontSet2", "IDWriteFontSet3", "?IDWriteFontFaceReference",
                   "IDWriteFontFaceReference1", "IDWriteFontResource", "?IDWriteFontSetBuilder", "?IDWriteFontSetBuilder1",
                   "IDWriteFontSetBuilder2", "IDWriteStringList", "IDWriteFontDownloadQueue", "?IDWriteFontFace6",
                   "?IDWriteFontSet4", "?IDWriteTextFormat3", "?IDWriteFontDownloadListener"],
}
if len(sys.argv) > 2 and sys.argv[2] == "dwrite":
    INTERFACES = DWRITE
if len(sys.argv) > 2 and sys.argv[2] == "d3d12":
    INTERFACES = D3D12
if len(sys.argv) > 2 and sys.argv[2] == "audio":
    INTERFACES = AUDIO
if len(sys.argv) > 2 and sys.argv[2] == "xaudio2":
    INTERFACES = XAUDIO2
print("// Generated by tools/gen_com_tables.py from MinGW-w64's headers (%s): the method order" % ", ".join(INTERFACES))
print("// of each interface (its vtable layout), its IID, and a stub per method for the methods not implemented yet.")
print("// Do not edit.")
for header, names in INTERFACES.items():
    text = open(sys.argv[1] + "/" + header).read()
    for n in names:
        if n.startswith("?"):
            print("static const GUID IID_%s = %s;" % (n[1:], uuid(text, n[1:])))
            continue
        ms = methods(text, n)
        print("static const char *const vt_%s[] = { %s };" % (n, ", ".join('"%s"' % x for x in ms)))
        for x in ms:
            print("static WINAPI HRESULT stub_%s_%s(void *self) { (void)self; return d3d_not_implemented(\"%s::%s\"); }" % (n, x, n, x))
        print("static void *const vtstub_%s[] = { %s };" % (n, ", ".join("(void *)stub_%s_%s" % (n, x) for x in ms)))
        print("#define VT_COUNT_%s %d" % (n, len(ms)))
        u = uuid(text, n)
        if u:
            print("static const GUID IID_%s = %s;" % (n, u))
