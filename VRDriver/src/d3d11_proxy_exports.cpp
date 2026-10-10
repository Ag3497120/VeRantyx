#include <windows.h>
#include <d3d10.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <map>
#include <mutex>

HMODULE g_hOriginalD3D11 = NULL;

static const uintptr_t kFakeSharedBufferTag = 0xDEAD0000u;
static const uint64_t kFakeSharedTextureTag = 0xBE00000000000000ull;
static volatile LONG g_nextFakeTextureId = 0;

static HANDLE MakeFakeSharedTextureHandle(const D3D11_TEXTURE2D_DESC& desc) {
    uint32_t id = static_cast<uint32_t>(InterlockedIncrement(&g_nextFakeTextureId)) & 0x3FFFFu;
    if (id == 0) id = static_cast<uint32_t>(InterlockedIncrement(&g_nextFakeTextureId)) & 0x3FFFFu;
    uint64_t value = kFakeSharedTextureTag |
        ((static_cast<uint64_t>(desc.Width - 1) & 0x1FFFu) << 43) |
        ((static_cast<uint64_t>(desc.Height - 1) & 0x1FFFu) << 30) |
        ((static_cast<uint64_t>(desc.Format) & 0x00FFu) << 22) |
        ((static_cast<uint64_t>(desc.SampleDesc.Count) & 0x0Fu) << 18) |
        id;
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
}

static bool IsFakeSharedTextureHandle(uintptr_t value) {
    return sizeof(uintptr_t) >= 8 &&
        (static_cast<uint64_t>(value) & 0xFF00000000000000ull) == kFakeSharedTextureTag;
}

class ProxyIDXGIResource : public IDXGIResource {
private:
    IDXGIResource* pOriginal;
    HANDLE fakeSharedHandle;
    LONG refCount;
public:
    ProxyIDXGIResource(IDXGIResource* orig, HANDLE handle)
        : pOriginal(orig), fakeSharedHandle(handle), refCount(1) {}

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(IDXGIObject)) ||
            IsEqualIID(riid, __uuidof(IDXGIDeviceSubObject)) ||
            IsEqualIID(riid, __uuidof(IDXGIResource))) {
            *ppvObject = static_cast<IDXGIResource*>(this);
            AddRef();
            return S_OK;
        }
        return pOriginal->QueryInterface(riid, ppvObject);
    }
    virtual ULONG STDMETHODCALLTYPE AddRef(void) { return InterlockedIncrement(&refCount); }
    virtual ULONG STDMETHODCALLTYPE Release(void) {
        ULONG count = InterlockedDecrement(&refCount);
        if (count == 0) { pOriginal->Release(); delete this; }
        return count;
    }
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT dataSize, const void* data) {
        return pOriginal->SetPrivateData(name, dataSize, data);
    }
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* data) {
        return pOriginal->SetPrivateDataInterface(name, data);
    }
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* dataSize, void* data) {
        return pOriginal->GetPrivateData(name, dataSize, data);
    }
    virtual HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) {
        return pOriginal->GetParent(riid, parent);
    }
    virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) {
        return pOriginal->GetDevice(riid, device);
    }
    virtual HRESULT STDMETHODCALLTYPE GetSharedHandle(HANDLE* sharedHandle) {
        if (!sharedHandle) return E_POINTER;
        *sharedHandle = fakeSharedHandle;
        FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
        if(f) { fprintf(f, "Returned fake shared buffer handle %p\n", fakeSharedHandle); fclose(f); }
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetUsage(DXGI_USAGE* usage) { return pOriginal->GetUsage(usage); }
    virtual HRESULT STDMETHODCALLTYPE SetEvictionPriority(UINT priority) { return pOriginal->SetEvictionPriority(priority); }
    virtual HRESULT STDMETHODCALLTYPE GetEvictionPriority(UINT* priority) { return pOriginal->GetEvictionPriority(priority); }
};

class ProxyID3D11Buffer : public ID3D11Buffer {
private:
    ID3D11Buffer* pOriginal;
    HANDLE fakeSharedHandle;
    LONG refCount;
public:
    ProxyID3D11Buffer(ID3D11Buffer* orig, HANDLE handle)
        : pOriginal(orig), fakeSharedHandle(handle), refCount(1) {}

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ID3D11DeviceChild)) ||
            IsEqualIID(riid, __uuidof(ID3D11Resource)) ||
            IsEqualIID(riid, __uuidof(ID3D11Buffer))) {
            *ppvObject = static_cast<ID3D11Buffer*>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualIID(riid, __uuidof(IDXGIObject)) ||
            IsEqualIID(riid, __uuidof(IDXGIDeviceSubObject)) ||
            IsEqualIID(riid, __uuidof(IDXGIResource))) {
            IDXGIResource* resource = nullptr;
            HRESULT hr = pOriginal->QueryInterface(__uuidof(IDXGIResource), (void**)&resource);
            if (FAILED(hr)) return hr;
            *ppvObject = static_cast<IDXGIResource*>(new ProxyIDXGIResource(resource, fakeSharedHandle));
            return S_OK;
        }
        return pOriginal->QueryInterface(riid, ppvObject);
    }
    virtual ULONG STDMETHODCALLTYPE AddRef(void) { return InterlockedIncrement(&refCount); }
    virtual ULONG STDMETHODCALLTYPE Release(void) {
        ULONG count = InterlockedDecrement(&refCount);
        if (count == 0) { pOriginal->Release(); delete this; }
        return count;
    }
    virtual void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) { pOriginal->GetDevice(device); }
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* dataSize, void* data) {
        return pOriginal->GetPrivateData(name, dataSize, data);
    }
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT dataSize, const void* data) {
        return pOriginal->SetPrivateData(name, dataSize, data);
    }
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* data) {
        return pOriginal->SetPrivateDataInterface(name, data);
    }
    virtual void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* resourceDimension) { pOriginal->GetType(resourceDimension); }
    virtual void STDMETHODCALLTYPE SetEvictionPriority(UINT priority) { pOriginal->SetEvictionPriority(priority); }
    virtual UINT STDMETHODCALLTYPE GetEvictionPriority(void) { return pOriginal->GetEvictionPriority(); }
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC* desc) { pOriginal->GetDesc(desc); }
};

class ProxyID3D11Texture2D : public ID3D11Texture2D {
private:
    ID3D11Texture2D* pOriginal;
    HANDLE fakeSharedHandle;
    LONG refCount;
public:
    ProxyID3D11Texture2D(ID3D11Texture2D* orig, HANDLE handle)
        : pOriginal(orig), fakeSharedHandle(handle), refCount(1) {}

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ID3D11DeviceChild)) ||
            IsEqualIID(riid, __uuidof(ID3D11Resource)) ||
            IsEqualIID(riid, __uuidof(ID3D11Texture2D))) {
            *ppvObject = static_cast<ID3D11Texture2D*>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualIID(riid, __uuidof(IDXGIObject)) ||
            IsEqualIID(riid, __uuidof(IDXGIDeviceSubObject)) ||
            IsEqualIID(riid, __uuidof(IDXGIResource))) {
            IDXGIResource* resource = nullptr;
            HRESULT hr = pOriginal->QueryInterface(__uuidof(IDXGIResource), (void**)&resource);
            if (FAILED(hr)) return hr;
            *ppvObject = static_cast<IDXGIResource*>(new ProxyIDXGIResource(resource, fakeSharedHandle));
            return S_OK;
        }
        return pOriginal->QueryInterface(riid, ppvObject);
    }
    virtual ULONG STDMETHODCALLTYPE AddRef(void) { return InterlockedIncrement(&refCount); }
    virtual ULONG STDMETHODCALLTYPE Release(void) {
        ULONG count = InterlockedDecrement(&refCount);
        if (count == 0) { pOriginal->Release(); delete this; }
        return count;
    }
    virtual void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) { pOriginal->GetDevice(device); }
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* dataSize, void* data) {
        return pOriginal->GetPrivateData(name, dataSize, data);
    }
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT dataSize, const void* data) {
        return pOriginal->SetPrivateData(name, dataSize, data);
    }
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* data) {
        return pOriginal->SetPrivateDataInterface(name, data);
    }
    virtual void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* resourceDimension) { pOriginal->GetType(resourceDimension); }
    virtual void STDMETHODCALLTYPE SetEvictionPriority(UINT priority) { pOriginal->SetEvictionPriority(priority); }
    virtual UINT STDMETHODCALLTYPE GetEvictionPriority(void) { return pOriginal->GetEvictionPriority(); }
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* desc) { pOriginal->GetDesc(desc); }
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
        if(f) { fprintf(f, "[Verantyx] d3d11.dll proxy loaded!\n"); fclose(f); }
    }
    return TRUE;
}

void EnsureD3D11Loaded() {
    if (!g_hOriginalD3D11) {
        // With the DXMT backend, system32\d3d11.dll is DXMT for the game and
        // DXVK is kept beside it as d3d11_dxvk.dll; the compositor stays on DXVK.
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat_s(path, "\\d3d11_dxvk.dll");
        g_hOriginalD3D11 = LoadLibraryA(path);
        if (!g_hOriginalD3D11) {
            GetSystemDirectoryA(path, MAX_PATH);
            strcat_s(path, "\\d3d11.dll");
            g_hOriginalD3D11 = LoadLibraryA(path);
        }
    }
}

class ProxyID3D11Device : public ID3D11Device {
private:
    ID3D11Device* pOriginal;
public:
    ProxyID3D11Device(ID3D11Device* orig) : pOriginal(orig) {}

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) { return pOriginal->QueryInterface(riid, ppvObject); }
    virtual ULONG STDMETHODCALLTYPE AddRef(void) { return pOriginal->AddRef(); }
    virtual ULONG STDMETHODCALLTYPE Release(void) {
        ULONG count = pOriginal->Release();
        if (count == 0) delete this;
        return count;
    }

    virtual HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Buffer **ppBuffer) {
        if (!pDesc || !ppBuffer) return E_INVALIDARG;
        D3D11_BUFFER_DESC desc = *pDesc;
        const UINT sharedFlags = D3D11_RESOURCE_MISC_SHARED |
            D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
            D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        const bool requestedShared = (desc.MiscFlags & sharedFlags) != 0;
        if (requestedShared) {
            FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
            if(f) { fprintf(f, "Stripped shared-resource flags 0x%08X from Buffer %u\n", desc.MiscFlags & sharedFlags, desc.ByteWidth); fclose(f); }
            desc.MiscFlags &= ~sharedFlags;
        }
        ID3D11Buffer* createdBuffer = nullptr;
        HRESULT hr = pOriginal->CreateBuffer(&desc, pInitialData, &createdBuffer);
        if (FAILED(hr) || !createdBuffer) {
            if (requestedShared) {
                FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
                if(f) { fprintf(f, "CreateBuffer failed for shared buffer %u: 0x%08X\n", desc.ByteWidth, hr); fclose(f); }
            }
            return hr;
        }
        if (requestedShared) {
            uintptr_t fakeValue = kFakeSharedBufferTag | (desc.ByteWidth & 0xFFFFu);
            *ppBuffer = new ProxyID3D11Buffer(createdBuffer, reinterpret_cast<HANDLE>(fakeValue));
        } else {
            *ppBuffer = createdBuffer;
        }
        return hr;
    }
    
    virtual HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture1D **ppTexture1D) { return pOriginal->CreateTexture1D(pDesc, pInitialData, ppTexture1D); }
    
    virtual HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D) {
        if (!pDesc || !ppTexture2D) return E_INVALIDARG;
        D3D11_TEXTURE2D_DESC desc = *pDesc;
        const UINT sharedFlags = D3D11_RESOURCE_MISC_SHARED |
            D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
            D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        const bool requestedShared = (desc.MiscFlags & sharedFlags) != 0;
        if (requestedShared) {
            FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
            if(f) { fprintf(f, "Stripped shared-resource flags 0x%08X from Texture2D %ux%u\n", desc.MiscFlags & sharedFlags, desc.Width, desc.Height); fclose(f); }
            desc.MiscFlags &= ~sharedFlags;
            D3D11_TEXTURE2D_DESC actualDesc = desc;
            ID3D11Texture2D* createdTexture = nullptr;
            HRESULT hr = pOriginal->CreateTexture2D(&actualDesc, pInitialData, &createdTexture);
            if (FAILED(hr) && actualDesc.SampleDesc.Count > 1) {
                actualDesc.SampleDesc.Count = 1;
                actualDesc.SampleDesc.Quality = 0;
                hr = pOriginal->CreateTexture2D(&actualDesc, pInitialData, &createdTexture);
                FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
                if(f) {
                    fprintf(f, "Texture2D %ux%u format=%u MSAA fallback to single-sample: 0x%08X\n",
                        desc.Width, desc.Height, desc.Format, hr);
                    fclose(f);
                }
            }
            if (SUCCEEDED(hr) && createdTexture) {
                *ppTexture2D = new ProxyID3D11Texture2D(createdTexture, MakeFakeSharedTextureHandle(actualDesc));
                FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
                if(f) { fprintf(f, "Created shared Texture2D compatibility resource %ux%u format=%u samples=%u\n", actualDesc.Width, actualDesc.Height, actualDesc.Format, actualDesc.SampleDesc.Count); fclose(f); }
            } else {
                FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
                if(f) { fprintf(f, "CreateTexture2D failed for shared texture %ux%u format=%u: 0x%08X\n", actualDesc.Width, actualDesc.Height, actualDesc.Format, hr); fclose(f); }
            }
            return hr;
        }
        return pOriginal->CreateTexture2D(&desc, pInitialData, ppTexture2D);
    }
    
    virtual HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture3D **ppTexture3D) { return pOriginal->CreateTexture3D(pDesc, pInitialData, ppTexture3D); }
    virtual HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc, ID3D11ShaderResourceView **ppSRView) { return pOriginal->CreateShaderResourceView(pResource, pDesc, ppSRView); }
    virtual HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource *pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc, ID3D11UnorderedAccessView **ppUAView) { return pOriginal->CreateUnorderedAccessView(pResource, pDesc, ppUAView); }
    virtual HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource *pResource, const D3D11_RENDER_TARGET_VIEW_DESC *pDesc, ID3D11RenderTargetView **ppRTView) { return pOriginal->CreateRenderTargetView(pResource, pDesc, ppRTView); }
    virtual HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource *pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc, ID3D11DepthStencilView **ppDepthStencilView) { return pOriginal->CreateDepthStencilView(pResource, pDesc, ppDepthStencilView); }
    virtual HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs, UINT NumElements, const void *pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout **ppInputLayout) { return pOriginal->CreateInputLayout(pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout); }
    virtual HRESULT STDMETHODCALLTYPE CreateVertexShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11VertexShader **ppVertexShader) { return pOriginal->CreateVertexShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader); }
    virtual HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11GeometryShader **ppGeometryShader) { return pOriginal->CreateGeometryShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader); }
    virtual HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void *pShaderBytecode, SIZE_T BytecodeLength, const D3D11_SO_DECLARATION_ENTRY *pSODeclaration, UINT NumEntries, const UINT *pBufferStrides, UINT NumStrides, UINT RasterizedStream, ID3D11ClassLinkage *pClassLinkage, ID3D11GeometryShader **ppGeometryShader) { return pOriginal->CreateGeometryShaderWithStreamOutput(pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries, pBufferStrides, NumStrides, RasterizedStream, pClassLinkage, ppGeometryShader); }
    virtual HRESULT STDMETHODCALLTYPE CreatePixelShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11PixelShader **ppPixelShader) { return pOriginal->CreatePixelShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader); }
    virtual HRESULT STDMETHODCALLTYPE CreateHullShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11HullShader **ppHullShader) { return pOriginal->CreateHullShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppHullShader); }
    virtual HRESULT STDMETHODCALLTYPE CreateDomainShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11DomainShader **ppDomainShader) { return pOriginal->CreateDomainShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppDomainShader); }
    virtual HRESULT STDMETHODCALLTYPE CreateComputeShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11ComputeShader **ppComputeShader) { return pOriginal->CreateComputeShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader); }
    virtual HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage **ppLinkage) { return pOriginal->CreateClassLinkage(ppLinkage); }
    virtual HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC *pBlendStateDesc, ID3D11BlendState **ppBlendState) { return pOriginal->CreateBlendState(pBlendStateDesc, ppBlendState); }
    virtual HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *pDepthStencilDesc, ID3D11DepthStencilState **ppDepthStencilState) { return pOriginal->CreateDepthStencilState(pDepthStencilDesc, ppDepthStencilState); }
    virtual HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC *pRasterizerDesc, ID3D11RasterizerState **ppRasterizerState) { return pOriginal->CreateRasterizerState(pRasterizerDesc, ppRasterizerState); }
    virtual HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC *pSamplerDesc, ID3D11SamplerState **ppSamplerState) { return pOriginal->CreateSamplerState(pSamplerDesc, ppSamplerState); }
    virtual HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC *pQueryDesc, ID3D11Query **ppQuery) { return pOriginal->CreateQuery(pQueryDesc, ppQuery); }
    virtual HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC *pPredicateDesc, ID3D11Predicate **ppPredicate) { return pOriginal->CreatePredicate(pPredicateDesc, ppPredicate); }
    virtual HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC *pCounterDesc, ID3D11Counter **ppCounter) { return pOriginal->CreateCounter(pCounterDesc, ppCounter); }
    virtual HRESULT STDMETHODCALLTYPE CreateDeferredContext(UINT ContextFlags, ID3D11DeviceContext **ppDeferredContext) { return pOriginal->CreateDeferredContext(ContextFlags, ppDeferredContext); }
    virtual HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, void **ppResource) {
        uintptr_t handleValue = reinterpret_cast<uintptr_t>(hResource);
        if (IsFakeSharedTextureHandle(handleValue)) {
            if (!ppResource) return E_POINTER;
            *ppResource = nullptr;
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = static_cast<UINT>(((handleValue >> 43) & 0x1FFFu) + 1);
            desc.Height = static_cast<UINT>(((handleValue >> 30) & 0x1FFFu) + 1);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = static_cast<DXGI_FORMAT>((handleValue >> 22) & 0x00FFu);
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (desc.Width == 0 || desc.Height == 0 || desc.Format == DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;

            ID3D11Texture2D* createdTexture = nullptr;
            HRESULT hr = pOriginal->CreateTexture2D(&desc, nullptr, &createdTexture);
            if (SUCCEEDED(hr) && createdTexture) {
                ProxyID3D11Texture2D* proxyTexture = new ProxyID3D11Texture2D(createdTexture, hResource);
                hr = proxyTexture->QueryInterface(ReturnedInterface, ppResource);
                proxyTexture->Release();
            }
            FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
            if(f) { fprintf(f, "OpenSharedResource fake texture %ux%u format=%u result=0x%08X\n", desc.Width, desc.Height, desc.Format, hr); fclose(f); }
            return hr;
        }
        if ((handleValue & 0xFFFF0000u) != kFakeSharedBufferTag) {
            return pOriginal->OpenSharedResource(hResource, ReturnedInterface, ppResource);
        }
        if (!ppResource) return E_POINTER;
        *ppResource = nullptr;

        UINT byteWidth = static_cast<UINT>(handleValue & 0xFFFFu);
        if (byteWidth == 0 || (byteWidth % 16) != 0) return E_INVALIDARG;
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = byteWidth;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Buffer* createdBuffer = nullptr;
        HRESULT hr = pOriginal->CreateBuffer(&desc, nullptr, &createdBuffer);
        if (SUCCEEDED(hr) && createdBuffer) {
            ID3D11Buffer* proxyBuffer = new ProxyID3D11Buffer(createdBuffer, hResource);
            hr = proxyBuffer->QueryInterface(ReturnedInterface, ppResource);
            proxyBuffer->Release();
        }
        FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
        if(f) { fprintf(f, "OpenSharedResource fake constant buffer %u result=0x%08X\n", byteWidth, hr); fclose(f); }
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT Format, UINT *pFormatSupport) { return pOriginal->CheckFormatSupport(Format, pFormatSupport); }
    virtual HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount, UINT *pNumQualityLevels) { return pOriginal->CheckMultisampleQualityLevels(Format, SampleCount, pNumQualityLevels); }
    virtual void STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO *pCounterInfo) { pOriginal->CheckCounterInfo(pCounterInfo); }
    virtual HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC *pDesc, D3D11_COUNTER_TYPE *pType, UINT *pActiveCounters, LPSTR szName, UINT *pNameLength, LPSTR szUnits, UINT *pUnitsLength, LPSTR szDescription, UINT *pDescriptionLength) { return pOriginal->CheckCounter(pDesc, pType, pActiveCounters, szName, pNameLength, szUnits, pUnitsLength, szDescription, pDescriptionLength); }
    virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize) { return pOriginal->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize); }
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) { return pOriginal->GetPrivateData(guid, pDataSize, pData); }
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) { return pOriginal->SetPrivateData(guid, DataSize, pData); }
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) { return pOriginal->SetPrivateDataInterface(guid, pData); }
    virtual D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel(void) { return pOriginal->GetFeatureLevel(); }
    virtual UINT STDMETHODCALLTYPE GetCreationFlags(void) { return pOriginal->GetCreationFlags(); }
    virtual HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason(void) { return pOriginal->GetDeviceRemovedReason(); }
    virtual void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext **ppImmediateContext) { pOriginal->GetImmediateContext(ppImmediateContext); }
    virtual HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) { return pOriginal->SetExceptionMode(RaiseFlags); }
    virtual UINT STDMETHODCALLTYPE GetExceptionMode(void) { return pOriginal->GetExceptionMode(); }
};

typedef HRESULT (STDMETHODCALLTYPE *CreateBufferProc)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
typedef HRESULT (STDMETHODCALLTYPE *CreateTexture2DProc)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
typedef HRESULT (STDMETHODCALLTYPE *OpenSharedResourceProc)(ID3D11Device*, HANDLE, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE *OpenSharedResource1Proc)(ID3D11Device1*, HANDLE, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE *GetSharedHandleProc)(IDXGIResource*, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *CreateSharedHandleProc)(IDXGIResource1*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR, HANDLE*);

static CreateBufferProc g_originalCreateBuffer = nullptr;
static CreateTexture2DProc g_originalCreateTexture2D = nullptr;
static OpenSharedResourceProc g_originalOpenSharedResource = nullptr;
static OpenSharedResource1Proc g_originalOpenSharedResource1 = nullptr;
static GetSharedHandleProc g_originalGetSharedHandle = nullptr;
static CreateSharedHandleProc g_originalCreateSharedHandle = nullptr;
static std::map<uintptr_t, HANDLE> g_fakeResourceHandles;
static std::map<uintptr_t, ID3D11Resource*> g_fakeHandleResources;
static std::mutex g_fakeResourceMutex;

#pragma pack(push, 1)
struct ProxySharedFrame {
    uint32_t sequenceNumber;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    float renderHeadTransform[16];
    double visionTimestamp;
};

struct ProxySharedHands {
    float headTransform[16];
    float leftTransform[16];
    float rightTransform[16];
    uint8_t leftTracked;
    uint8_t rightTracked;
    uint8_t leftTrigger;
    uint8_t rightTrigger;
    uint32_t rightButtons;
    uint32_t leftButtons;
    float rightStickX;
    float rightStickY;
    float leftStickX;
    float leftStickY;
    float rightVelocity[3];
    float leftVelocity[3];
    double visionTimestamp;
    float leftTangents[4];
    float rightTangents[4];
    uint8_t leftGrip;
    uint8_t rightGrip;
    uint8_t headTracked;
    // Hand velocities in SteamVR space from the headset (hand tracking fused
    // with Joy-Con inertial sensors). Linear ones use the fields above.
    float leftAngularVelocity[3];
    float rightAngularVelocity[3];
    uint8_t velocityValid;
};
#pragma pack(pop)

struct ProxySharedHeader {
    volatile LONG latestReadyIndex;
    volatile LONG pendingTextureHandle;
    volatile LONG captureRequestSequence;
    volatile LONG captureAckSequence;
    volatile LONG captureSourceMode;
    volatile LONG poseSequence;
    uint32_t padding[10];
};

static const size_t kProxyMaxPixelBytes = 4096ull * 4096ull * 8ull;
static const size_t kProxyFrameBlockSize = sizeof(ProxySharedFrame) + kProxyMaxPixelBytes;
static const size_t kProxyHandsOffset = sizeof(ProxySharedHeader) + 2 * kProxyFrameBlockSize;
static const size_t kProxySharedMapSize = kProxyHandsOffset + sizeof(ProxySharedHands) + 1024;
static volatile LONG g_captureWorkerStarted = 0;
static const uint32_t kProxyReadbackRingSize = 3;
struct ProxyReadbackSlot {
    ID3D11Texture2D* texture = nullptr;
    bool pending = false;
    float headTransform[16] = {0};
    double visionTimestamp = 0.0;
};
static ProxyReadbackSlot g_proxyReadbackRing[kProxyReadbackRingSize];
static ID3D11Device* g_proxyReadbackDevice = nullptr;
static uint32_t g_proxyReadbackWidth = 0;
static uint32_t g_proxyReadbackHeight = 0;
static DXGI_FORMAT g_proxyReadbackFormat = DXGI_FORMAT_UNKNOWN;
static uint32_t g_proxyReadbackIndex = 0;
static_assert(sizeof(ProxySharedFrame) == 88, "Shared frame format must match the driver and encoder");
static_assert(sizeof(ProxySharedHeader) == 64, "Shared header format must match the encoder");
static_assert(sizeof(ProxySharedHands) == 312, "Shared hand format must match the driver and encoder");

static HRESULT STDMETHODCALLTYPE Hooked_GetSharedHandle(IDXGIResource* resource, HANDLE* sharedHandle);
static HRESULT STDMETHODCALLTYPE Hooked_CreateSharedHandle(IDXGIResource1* resource, const SECURITY_ATTRIBUTES* attributes, DWORD access, LPCWSTR name, HANDLE* sharedHandle);

static IUnknown* GetCanonicalIUnknown(IUnknown* object) {
    if (!object) return nullptr;
    IUnknown* canonical = nullptr;
    if (SUCCEEDED(object->QueryInterface(__uuidof(IUnknown), (void**)&canonical))) return canonical;
    return nullptr;
}

static bool ReplaceVTableEntry(void** vtable, size_t index, void* hook, void** original) {
    if (!vtable || !hook || !original) return false;
    if (vtable[index] == hook) return true;
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    if (!*original) *original = vtable[index];
    vtable[index] = hook;
    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &ignored);
    return true;
}

static bool GetRegisteredHandle(IUnknown* object, HANDLE* handle) {
    IUnknown* canonical = GetCanonicalIUnknown(object);
    if (!canonical) return false;
    uintptr_t key = reinterpret_cast<uintptr_t>(canonical);
    bool foundHandle = false;
    {
        std::lock_guard<std::mutex> lock(g_fakeResourceMutex);
        auto found = g_fakeResourceHandles.find(key);
        if (found != g_fakeResourceHandles.end()) {
            if (handle) *handle = found->second;
            foundHandle = true;
        }
    }
    canonical->Release();
    return foundHandle;
}

static void RegisterFakeSharedResource(ID3D11Resource* resource, HANDLE handle) {
    if (!resource) return;
    IUnknown* canonical = GetCanonicalIUnknown(resource);
    if (canonical) {
        uintptr_t key = reinterpret_cast<uintptr_t>(canonical);
        const uintptr_t fullHandle = reinterpret_cast<uintptr_t>(handle);
        const uintptr_t truncatedHandle = static_cast<uintptr_t>(static_cast<uint32_t>(fullHandle));
        {
            std::lock_guard<std::mutex> lock(g_fakeResourceMutex);
            g_fakeResourceHandles[key] = handle;

            auto registerHandle = [resource](uintptr_t handleKey) {
                auto existing = g_fakeHandleResources.find(handleKey);
                if (existing != g_fakeHandleResources.end() && existing->second == resource) return;
                resource->AddRef();
                if (existing != g_fakeHandleResources.end()) {
                    existing->second->Release();
                    existing->second = resource;
                } else {
                    g_fakeHandleResources[handleKey] = resource;
                }
            };

            registerHandle(fullHandle);
            // The current runtime transports the compatibility handle to
            // IVRVirtualDisplay::Present as its low 32 bits. Register that
            // alias in the texture-owning process so it resolves to the exact
            // D3D resource instead of being reopened by DXVK as an OS handle.
            if (IsFakeSharedTextureHandle(fullHandle) && truncatedHandle != fullHandle)
                registerHandle(truncatedHandle);
        }
        canonical->Release();
    }

    IDXGIResource* dxgiResource = nullptr;
    if (SUCCEEDED(resource->QueryInterface(__uuidof(IDXGIResource), (void**)&dxgiResource))) {
        void** vtable = *(void***)dxgiResource;
        ReplaceVTableEntry(vtable, 8, (void*)Hooked_GetSharedHandle, (void**)&g_originalGetSharedHandle);
        dxgiResource->Release();
    }

    IDXGIResource1* dxgiResource1 = nullptr;
    if (SUCCEEDED(resource->QueryInterface(__uuidof(IDXGIResource1), (void**)&dxgiResource1))) {
        void** vtable = *(void***)dxgiResource1;
        ReplaceVTableEntry(vtable, 13, (void*)Hooked_CreateSharedHandle, (void**)&g_originalCreateSharedHandle);
        dxgiResource1->Release();
    }
}

static HRESULT ReturnRegisteredResource(HANDLE handle, REFIID requestedInterface, void** outResource) {
    if (!outResource) return E_POINTER;
    *outResource = nullptr;
    uintptr_t key = reinterpret_cast<uintptr_t>(handle);
    ID3D11Resource* resource = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_fakeResourceMutex);
        auto found = g_fakeHandleResources.find(key);
        if (found == g_fakeHandleResources.end()) return E_INVALIDARG;
        resource = found->second;
        resource->AddRef();
    }
    HRESULT hr = resource->QueryInterface(requestedInterface, outResource);
    resource->Release();
    return hr;
}

static void LogProxyCapture(const char* message) {
    FILE* f = fopen("C:\\vr_shared_frame.log", "a");
    if (f) {
        fprintf(f, "%s\n", message);
        fclose(f);
    }
}

static bool IsProxyCaptureFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

static void ResetProxyReadbackRing() {
    for (uint32_t i = 0; i < kProxyReadbackRingSize; ++i) {
        if (g_proxyReadbackRing[i].texture) {
            g_proxyReadbackRing[i].texture->Release();
            g_proxyReadbackRing[i].texture = nullptr;
        }
        g_proxyReadbackRing[i].pending = false;
    }
    if (g_proxyReadbackDevice) {
        g_proxyReadbackDevice->Release();
        g_proxyReadbackDevice = nullptr;
    }
    g_proxyReadbackWidth = 0;
    g_proxyReadbackHeight = 0;
    g_proxyReadbackFormat = DXGI_FORMAT_UNKNOWN;
    g_proxyReadbackIndex = 0;
}

static bool EnsureProxyReadbackRing(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& sourceDesc) {
    if (!device) return false;
    bool ready = g_proxyReadbackDevice == device &&
        g_proxyReadbackWidth == sourceDesc.Width &&
        g_proxyReadbackHeight == sourceDesc.Height &&
        g_proxyReadbackFormat == sourceDesc.Format;
    for (uint32_t i = 0; ready && i < kProxyReadbackRingSize; ++i) {
        ready = g_proxyReadbackRing[i].texture != nullptr;
    }
    if (ready) return true;

    ResetProxyReadbackRing();
    D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    device->AddRef();
    g_proxyReadbackDevice = device;
    for (uint32_t i = 0; i < kProxyReadbackRingSize; ++i) {
        const HRESULT result = device->CreateTexture2D(&stagingDesc, nullptr, &g_proxyReadbackRing[i].texture);
        if (FAILED(result) || !g_proxyReadbackRing[i].texture) {
            ResetProxyReadbackRing();
            return false;
        }
    }
    g_proxyReadbackWidth = sourceDesc.Width;
    g_proxyReadbackHeight = sourceDesc.Height;
    g_proxyReadbackFormat = sourceDesc.Format;
    return true;
}

static bool ReadProxyHandsSnapshot(ProxySharedHeader* header, uint8_t* mapping, ProxySharedHands* snapshot) {
    if (!header || !mapping || !snapshot) return false;
    const ProxySharedHands* sharedHands = reinterpret_cast<const ProxySharedHands*>(mapping + kProxyHandsOffset);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG before = InterlockedCompareExchange(&header->poseSequence, 0, 0);
        if (before & 1) continue;
        MemoryBarrier();
        memcpy(snapshot, sharedHands, sizeof(*snapshot));
        MemoryBarrier();
        const LONG after = InterlockedCompareExchange(&header->poseSequence, 0, 0);
        if (before == after && !(after & 1)) return true;
    }
    return false;
}

static bool CaptureRequestedTexture(
    ProxySharedHeader* header,
    uint8_t* mapping,
    uintptr_t handleValue,
    uint32_t* frameSequence) {
    ID3D11Texture2D* sourceTexture = nullptr;
    HANDLE compatibilityHandle = reinterpret_cast<HANDLE>(handleValue);
    HRESULT result = ReturnRegisteredResource(
        compatibilityHandle,
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&sourceTexture)
    );
    if (FAILED(result) || !sourceTexture) {
        static LONG misses = 0;
        if ((InterlockedIncrement(&misses) % 120) == 1) {
            char message[160];
            snprintf(message, sizeof(message),
                "[Verantyx] Compositor capture resource not found for handle 0x%08lx pid=%lu",
                (unsigned long)static_cast<uint32_t>(handleValue),
                (unsigned long)GetCurrentProcessId());
            LogProxyCapture(message);
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceTexture->GetDesc(&sourceDesc);
    if (sourceDesc.Width == 0 || sourceDesc.Height == 0 ||
        sourceDesc.Width > 4096 || sourceDesc.Height > 4096 ||
        sourceDesc.ArraySize != 1 || sourceDesc.SampleDesc.Count != 1 ||
        !IsProxyCaptureFormat(sourceDesc.Format)) {
        sourceTexture->Release();
        return false;
    }

    ID3D11Device* device = nullptr;
    sourceTexture->GetDevice(&device);
    if (!device) {
        sourceTexture->Release();
        return false;
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) {
        device->Release();
        sourceTexture->Release();
        return false;
    }

    if (!EnsureProxyReadbackRing(device, sourceDesc)) {
        context->Release();
        device->Release();
        sourceTexture->Release();
        return false;
    }

    ID3D10Multithread* multithread = nullptr;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D10Multithread), (void**)&multithread)) && multithread) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Enter();
    }
    ProxyReadbackSlot& slot = g_proxyReadbackRing[g_proxyReadbackIndex];
    bool publishedFrame = false;
    if (slot.pending) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        result = context->Map(slot.texture, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (multithread) { multithread->Leave(); multithread->Release(); }
            context->Release();
            device->Release();
            sourceTexture->Release();
            return false;
        }
        if (FAILED(result) || !mapped.pData) {
            static LONG mapFailures = 0;
            if ((InterlockedIncrement(&mapFailures) % 120) == 1) {
                char message[144];
                snprintf(message, sizeof(message), "[Verantyx] Nonblocking compositor readback Map failed: 0x%08lx", (unsigned long)result);
                LogProxyCapture(message);
            }
            if (multithread) { multithread->Leave(); multithread->Release(); }
            context->Release();
            device->Release();
            sourceTexture->Release();
            return false;
        }

        // Map has completed without waiting for the GPU. Let the compositor use
        // its immediate context while this worker copies the mapped staging rows.
        if (multithread) multithread->Leave();
        const LONG latestReadyIndex = InterlockedCompareExchange(&header->latestReadyIndex, 0, 0);
        const uint32_t writeIndex = static_cast<uint32_t>(latestReadyIndex ^ 1);
        uint8_t* frameBlock = mapping + sizeof(ProxySharedHeader) + writeIndex * kProxyFrameBlockSize;
        ProxySharedFrame* frame = reinterpret_cast<ProxySharedFrame*>(frameBlock);
        uint8_t* destination = frameBlock + sizeof(ProxySharedFrame);
        const size_t rowBytes = static_cast<size_t>(sourceDesc.Width) * 4;
        const uint8_t* source = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t row = 0; row < sourceDesc.Height; ++row) {
            memcpy(destination + static_cast<size_t>(row) * rowBytes,
                source + static_cast<size_t>(row) * mapped.RowPitch, rowBytes);
        }
        if (multithread) multithread->Enter();
        context->Unmap(slot.texture, 0);

        frame->sequenceNumber = ++(*frameSequence);
        if (frame->sequenceNumber == 0) frame->sequenceNumber = ++(*frameSequence);
        frame->width = sourceDesc.Width;
        frame->height = sourceDesc.Height;
        frame->format = static_cast<uint32_t>(sourceDesc.Format);
        memcpy(frame->renderHeadTransform, slot.headTransform, sizeof(frame->renderHeadTransform));
        frame->visionTimestamp = slot.visionTimestamp;
        MemoryBarrier();
        InterlockedExchange(&header->latestReadyIndex, static_cast<LONG>(writeIndex));
        slot.pending = false;
        publishedFrame = true;
    }

    context->CopyResource(slot.texture, sourceTexture);
    ProxySharedHands handsSnapshot = {};
    if (ReadProxyHandsSnapshot(header, mapping, &handsSnapshot)) {
        memcpy(slot.headTransform, handsSnapshot.headTransform, sizeof(slot.headTransform));
        slot.visionTimestamp = handsSnapshot.visionTimestamp;
    } else {
        memset(slot.headTransform, 0, sizeof(slot.headTransform));
        slot.headTransform[0] = slot.headTransform[5] = slot.headTransform[10] = slot.headTransform[15] = 1.0f;
        slot.visionTimestamp = 0.0;
    }
    slot.pending = true;
    g_proxyReadbackIndex = (g_proxyReadbackIndex + 1) % kProxyReadbackRingSize;
    if (multithread) {
        multithread->Leave();
        multithread->Release();
    }

    if (publishedFrame && *frameSequence % 90 == 1) {
        char message[192];
        snprintf(message, sizeof(message),
            "[Verantyx] Compositor copied frame %u (%ux%u) via asynchronous readback handle 0x%08lx pid=%lu",
            *frameSequence, sourceDesc.Width, sourceDesc.Height,
            (unsigned long)static_cast<uint32_t>(handleValue),
            (unsigned long)GetCurrentProcessId());
        LogProxyCapture(message);
    }

    context->Release();
    device->Release();
    sourceTexture->Release();
    return SUCCEEDED(result);
}

static DWORD WINAPI ProxyFrameCaptureThread(LPVOID) {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mappingHandle = nullptr;
    uint8_t* mapping = nullptr;
    ProxySharedHeader* header = nullptr;
    LONG lastRequestSequence = 0;
    uint32_t frameSequence = 0;

    while (true) {
        if (!mapping) {
            if (file == INVALID_HANDLE_VALUE) {
                file = CreateFileA("C:\\vr_shared_frame.dat", GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) {
                    Sleep(250);
                    continue;
                }
            }
            mappingHandle = CreateFileMappingA(file, nullptr, PAGE_READWRITE,
                static_cast<DWORD>((kProxySharedMapSize >> 32) & 0xFFFFFFFFu),
                static_cast<DWORD>(kProxySharedMapSize & 0xFFFFFFFFu), nullptr);
            if (!mappingHandle) {
                CloseHandle(file);
                file = INVALID_HANDLE_VALUE;
                Sleep(250);
                continue;
            }
            mapping = static_cast<uint8_t*>(MapViewOfFile(mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, kProxySharedMapSize));
            if (!mapping) {
                CloseHandle(mappingHandle);
                CloseHandle(file);
                mappingHandle = nullptr;
                file = INVALID_HANDLE_VALUE;
                Sleep(250);
                continue;
            }
            header = reinterpret_cast<ProxySharedHeader*>(mapping);
            LogProxyCapture("[Verantyx] Compositor frame capture worker attached to shared frame memory");
        }

        if (InterlockedCompareExchange(&header->captureSourceMode, 0, 0) == 2) {
            // The game's capture proxy owns the stream while it submits
            // (padding[9] is its last Submit tick). During a level load it
            // stops for 30 s or more; copy the compositor's loading screen then.
            const DWORD lastSubmit = (DWORD)InterlockedCompareExchange(
                reinterpret_cast<volatile LONG*>(&header->padding[9]), 0, 0);
            if (lastSubmit == 0 || GetTickCount() - lastSubmit < 1500) {
                Sleep(10);
                continue;
            }
        }

        const LONG requestSequence = InterlockedCompareExchange(&header->captureRequestSequence, 0, 0);
        if (requestSequence != 0 && requestSequence != lastRequestSequence) {
            MemoryBarrier();
            const uintptr_t handleValue = static_cast<uintptr_t>(
                static_cast<uint32_t>(InterlockedCompareExchange(&header->pendingTextureHandle, 0, 0)));
            CaptureRequestedTexture(header, mapping, handleValue, &frameSequence);
            InterlockedExchange(&header->captureAckSequence, requestSequence);
            lastRequestSequence = requestSequence;
        } else {
            Sleep(1);
        }
    }
    return 0;
}

static bool IsVRCompositorProcess() {
    char imagePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, imagePath, ARRAYSIZE(imagePath)) == 0) return false;
    char* basename = strrchr(imagePath, '\\');
    if (!basename) basename = strrchr(imagePath, '/');
    basename = basename ? basename + 1 : imagePath;
    return _stricmp(basename, "vrcompositor.exe") == 0;
}

static void StartProxyFrameCaptureWorker() {
    if (!IsVRCompositorProcess() || InterlockedCompareExchange(&g_captureWorkerStarted, 1, 0) != 0) return;
    HANDLE thread = CreateThread(nullptr, 0, ProxyFrameCaptureThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedExchange(&g_captureWorkerStarted, 0);
        LogProxyCapture("[Verantyx] Could not start compositor frame capture worker");
    }
}

static HRESULT CreateFakeSharedResource(ID3D11Device* device, HANDLE handle, REFIID requestedInterface, void** outResource) {
    uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    if (!outResource) return E_POINTER;
    *outResource = nullptr;

    if (IsFakeSharedTextureHandle(value)) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(((value >> 43) & 0x1FFFu) + 1);
        desc.Height = static_cast<UINT>(((value >> 30) & 0x1FFFu) + 1);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = static_cast<DXGI_FORMAT>((value >> 22) & 0x00FFu);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (desc.Width == 0 || desc.Height == 0 || desc.Format == DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
        ID3D11Texture2D* texture = nullptr;
        HRESULT hr = g_originalCreateTexture2D(device, &desc, nullptr, &texture);
        if (FAILED(hr) || !texture) return hr;
        RegisterFakeSharedResource(texture, handle);
        hr = texture->QueryInterface(requestedInterface, outResource);
        texture->Release();
        FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
        if(f) { fprintf(f, "OpenSharedResource fake texture %ux%u format=%u result=0x%08X\n", desc.Width, desc.Height, desc.Format, hr); fclose(f); }
        return hr;
    }

    if ((value & 0xFFFF0000u) == kFakeSharedBufferTag) {
        UINT byteWidth = static_cast<UINT>(value & 0xFFFFu);
        if (byteWidth == 0 || (byteWidth % 16) != 0) return E_INVALIDARG;
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = byteWidth;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Buffer* buffer = nullptr;
        HRESULT hr = g_originalCreateBuffer(device, &desc, nullptr, &buffer);
        if (FAILED(hr) || !buffer) return hr;
        RegisterFakeSharedResource(buffer, handle);
        hr = buffer->QueryInterface(requestedInterface, outResource);
        buffer->Release();
        FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
        if(f) { fprintf(f, "OpenSharedResource fake constant buffer %u result=0x%08X\n", byteWidth, hr); fclose(f); }
        return hr;
    }
    return E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE Hooked_GetSharedHandle(IDXGIResource* resource, HANDLE* sharedHandle) {
    if (!sharedHandle) return E_POINTER;
    if (GetRegisteredHandle(resource, sharedHandle)) {
        FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
        if(f) { fprintf(f, "Returned registered shared resource handle %p\n", *sharedHandle); fclose(f); }
        return S_OK;
    }
    if (!g_originalGetSharedHandle) return E_NOTIMPL;

    HRESULT hr = g_originalGetSharedHandle(resource, sharedHandle);
    if (SUCCEEDED(hr) && *sharedHandle) {
        // DXVK/Wine handles are not always reopenable through a second D3D11
        // device, even when both devices live in this process. Keep the actual
        // resource associated with its returned handle so OpenSharedResource
        // can hand the object back directly when that is possible.
        ID3D11Resource* d3dResource = nullptr;
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Resource), (void**)&d3dResource)) && d3dResource) {
            RegisterFakeSharedResource(d3dResource, *sharedHandle);
            d3dResource->Release();
        }
        FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
        if(f) { fprintf(f, "Registered native shared handle %p for in-process lookup, process=%lu\n", *sharedHandle, GetCurrentProcessId()); fclose(f); }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateSharedHandle(
    IDXGIResource1* resource,
    const SECURITY_ATTRIBUTES* attributes,
    DWORD access,
    LPCWSTR name,
    HANDLE* sharedHandle) {
    if (!sharedHandle) return E_POINTER;
    if (GetRegisteredHandle(resource, sharedHandle)) return S_OK;

    ID3D11Texture2D* texture = nullptr;
    if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture)) && texture) {
        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc(&desc);
        if (desc.Width > 0 && desc.Width <= 0x3FFFu &&
            desc.Height > 0 && desc.Height <= 0x3FFFu &&
            desc.Format != DXGI_FORMAT_UNKNOWN && desc.SampleDesc.Count > 0) {
            HANDLE compatibleHandle = MakeFakeSharedTextureHandle(desc);
            RegisterFakeSharedResource(texture, compatibleHandle);
            *sharedHandle = compatibleHandle;
            FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
            if(f) { fprintf(f, "CreateSharedHandle returned compatibility handle %p for %ux%u format=%u process=%lu\n", compatibleHandle, desc.Width, desc.Height, desc.Format, GetCurrentProcessId()); fclose(f); }
            texture->Release();
            return S_OK;
        }
        texture->Release();
    }

    return g_originalCreateSharedHandle
        ? g_originalCreateSharedHandle(resource, attributes, access, name, sharedHandle)
        : E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateBuffer(ID3D11Device* device, const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* initialData, ID3D11Buffer** outBuffer) {
    if (!pDesc || !outBuffer) return E_INVALIDARG;
    D3D11_BUFFER_DESC desc = *pDesc;
    const UINT sharedFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    bool requestedShared = (desc.MiscFlags & sharedFlags) != 0;
    if (requestedShared) desc.MiscFlags &= ~sharedFlags;
    HRESULT hr = g_originalCreateBuffer(device, requestedShared ? &desc : pDesc, initialData, outBuffer);
    if (SUCCEEDED(hr) && requestedShared && *outBuffer) {
        HANDLE handle = reinterpret_cast<HANDLE>(kFakeSharedBufferTag | (desc.ByteWidth & 0xFFFFu));
        RegisterFakeSharedResource(*outBuffer, handle);
        FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
        if(f) { fprintf(f, "Created shared buffer compatibility resource %u\n", desc.ByteWidth); fclose(f); }
    } else if (FAILED(hr) && requestedShared) {
        FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
        if(f) { fprintf(f, "CreateBuffer failed for shared buffer %u: 0x%08X\n", desc.ByteWidth, hr); fclose(f); }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* initialData, ID3D11Texture2D** outTexture) {
    if (!pDesc || !outTexture) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc = *pDesc;
    const UINT sharedFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    bool requestedShared = (desc.MiscFlags & sharedFlags) != 0;
    if (requestedShared) desc.MiscFlags &= ~sharedFlags;
    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = g_originalCreateTexture2D(device, requestedShared ? &desc : pDesc, initialData, &texture);
    if (FAILED(hr) && desc.SampleDesc.Count > 1) {
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        hr = g_originalCreateTexture2D(device, &desc, initialData, &texture);
        FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
        if(f) { fprintf(f, "Texture2D %ux%u format=%u MSAA fallback to single-sample: 0x%08X\n", desc.Width, desc.Height, desc.Format, hr); fclose(f); }
    }
    if (FAILED(hr) || !texture) {
        *outTexture = nullptr;
        if (requestedShared) {
            FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
            if(f) { fprintf(f, "CreateTexture2D failed for shared texture %ux%u format=%u: 0x%08X\n", desc.Width, desc.Height, desc.Format, hr); fclose(f); }
        }
        return hr;
    }
    *outTexture = texture;
    if (requestedShared) {
        RegisterFakeSharedResource(texture, MakeFakeSharedTextureHandle(desc));
        FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
        if(f) { fprintf(f, "Created shared Texture2D compatibility resource %ux%u format=%u samples=%u\n", desc.Width, desc.Height, desc.Format, desc.SampleDesc.Count); fclose(f); }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_OpenSharedResource(ID3D11Device* device, HANDLE handle, REFIID requestedInterface, void** outResource) {
    uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    if (!outResource) return E_POINTER;
    HRESULT registeredResult = ReturnRegisteredResource(handle, requestedInterface, outResource);
    if (SUCCEEDED(registeredResult)) {
        FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
        if(f) { fprintf(f, "OpenSharedResource returned registered resource handle=%p process=%lu\n", handle, GetCurrentProcessId()); fclose(f); }
        return registeredResult;
    }
    if (!IsFakeSharedTextureHandle(value) && (value & 0xFFFF0000u) != kFakeSharedBufferTag) {
        HRESULT originalResult = g_originalOpenSharedResource(device, handle, requestedInterface, outResource);
        static LONG passthroughFailures = 0;
        if (FAILED(originalResult) && (InterlockedIncrement(&passthroughFailures) % 120) == 1) {
            FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
            if(f) { fprintf(f, "OpenSharedResource passthrough handle=%p result=0x%08X process=%lu\n", handle, originalResult, GetCurrentProcessId()); fclose(f); }
        }
        return originalResult;
    }
    HRESULT hr = ReturnRegisteredResource(handle, requestedInterface, outResource);
    if (SUCCEEDED(hr)) return hr;
    return CreateFakeSharedResource(device, handle, requestedInterface, outResource);
}

static HRESULT STDMETHODCALLTYPE Hooked_OpenSharedResource1(ID3D11Device1* device, HANDLE handle, REFIID requestedInterface, void** outResource) {
    uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    if (!outResource) return E_POINTER;
    HRESULT registeredResult = ReturnRegisteredResource(handle, requestedInterface, outResource);
    if (SUCCEEDED(registeredResult)) return registeredResult;
    if (!IsFakeSharedTextureHandle(value) && (value & 0xFFFF0000u) != kFakeSharedBufferTag)
        return g_originalOpenSharedResource1(device, handle, requestedInterface, outResource);
    HRESULT hr = ReturnRegisteredResource(handle, requestedInterface, outResource);
    if (SUCCEEDED(hr)) return hr;
    return CreateFakeSharedResource(static_cast<ID3D11Device*>(device), handle, requestedInterface, outResource);
}

static void SetupDeviceHooks(ID3D11Device* device) {
    if (!device) return;
    void** vtable = *(void***)device;
    ReplaceVTableEntry(vtable, 3, (void*)Hooked_CreateBuffer, (void**)&g_originalCreateBuffer);
    ReplaceVTableEntry(vtable, 5, (void*)Hooked_CreateTexture2D, (void**)&g_originalCreateTexture2D);
    ReplaceVTableEntry(vtable, 28, (void*)Hooked_OpenSharedResource, (void**)&g_originalOpenSharedResource);

    ID3D11Device1* device1 = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device1), (void**)&device1))) {
        void** device1Vtable = *(void***)device1;
        ReplaceVTableEntry(device1Vtable, 48, (void*)Hooked_OpenSharedResource1, (void**)&g_originalOpenSharedResource1);
        device1->Release();
    }

    if (g_originalCreateTexture2D) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = desc.Height = 1;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* texture = nullptr;
        if (SUCCEEDED(g_originalCreateTexture2D(device, &desc, nullptr, &texture)) && texture) {
            IDXGIResource* resource = nullptr;
            if (SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource), (void**)&resource))) {
                void** resourceVtable = *(void***)resource;
                ReplaceVTableEntry(resourceVtable, 8, (void*)Hooked_GetSharedHandle, (void**)&g_originalGetSharedHandle);
                resource->Release();
            }
            IDXGIResource1* resource1 = nullptr;
            if (SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource1), (void**)&resource1))) {
                void** resource1Vtable = *(void***)resource1;
                ReplaceVTableEntry(resource1Vtable, 13, (void*)Hooked_CreateSharedHandle, (void**)&g_originalCreateSharedHandle);
                resource1->Release();
            }
            texture->Release();
        }
    }
    StartProxyFrameCaptureWorker();
}

typedef HRESULT (WINAPI *D3D11CreateDevice_t)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

extern "C" __declspec(dllexport) HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
    
    FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
    if(f) { fprintf(f, "[Verantyx] D3D11CreateDevice PROXY CALLED!\n"); fclose(f); }
    
    EnsureD3D11Loaded();
    D3D11CreateDevice_t pOrig = (D3D11CreateDevice_t)GetProcAddress(g_hOriginalD3D11, "D3D11CreateDevice");
    if (!pOrig) {
        if(f) { f = fopen("C:\\d3d11_proxy_log.txt", "a"); if(f){fprintf(f, "[Verantyx] ERROR: pOrig is NULL! g_hOriginalD3D11=%p\n", g_hOriginalD3D11); fclose(f);} }
        return E_FAIL;
    }

    if(f) { f = fopen("C:\\d3d11_proxy_log.txt", "a"); if(f){fprintf(f, "[Verantyx] D3D11CreateDevice overriding adapter to NULL and DriverType to HARDWARE\n"); fclose(f);} }
    // pAdapter = NULL;
    // DriverType = D3D_DRIVER_TYPE_HARDWARE;
    Flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    Flags &= ~D3D11_CREATE_DEVICE_SINGLETHREADED;
    
    if(f) { f = fopen("C:\\d3d11_proxy_log.txt", "a"); if(f){
        fprintf(f, "[Verantyx] D3D11CreateDevice requested %d feature levels. SDKVersion=%u\n", FeatureLevels, SDKVersion);
        for(UINT i=0; i<FeatureLevels; i++) {
            fprintf(f, "  FeatureLevel[%d] = %04X\n", i, pFeatureLevels[i]);
        }
        fclose(f);
    } }
    
    ID3D11Device* pOrigDevice = NULL;
    HRESULT hr = pOrig(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, &pOrigDevice, pFeatureLevel, ppImmediateContext);
    
    if (FAILED(hr)) {
        if(f) { f = fopen("C:\\d3d11_proxy_log.txt", "a"); if(f){fprintf(f, "[Verantyx] D3D11CreateDevice still failed with hr=%08X\n", hr); fclose(f);} }
    }
    if (SUCCEEDED(hr) && pOrigDevice) {
        SetupDeviceHooks(pOrigDevice);
        if (ppDevice) {
            *ppDevice = pOrigDevice;
            if(f) { f = fopen("C:\\d3d11_proxy_log.txt", "a"); if(f){fprintf(f, "[Verantyx] Returned native ID3D11Device with resource hooks installed.\n"); fclose(f);} }
        } else {
            pOrigDevice->Release();
        }
    } else if (ppDevice) {
        *ppDevice = pOrigDevice;
    } else if (pOrigDevice) {
        pOrigDevice->Release();
    }
    return hr;
}

typedef HRESULT (WINAPI *D3D11CreateDeviceAndSwapChain_t)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*,
    IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

extern "C" __declspec(dllexport) HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
    
    FILE* f = fopen("C:\\d3d11_proxy_log.txt", "a");
    if(f) { fprintf(f, "[Verantyx] D3D11CreateDeviceAndSwapChain PROXY CALLED!\n"); fclose(f); }

    EnsureD3D11Loaded();
    D3D11CreateDeviceAndSwapChain_t pOrig = (D3D11CreateDeviceAndSwapChain_t)GetProcAddress(g_hOriginalD3D11, "D3D11CreateDeviceAndSwapChain");
    if (!pOrig) return E_FAIL;

    if(f) { f = fopen("C:\\d3d11_proxy_log.txt", "a"); if(f){fprintf(f, "[Verantyx] D3D11CreateDeviceAndSwapChain overriding adapter to NULL and DriverType to HARDWARE\n"); fclose(f);} }
    // pAdapter = NULL;
    // DriverType = D3D_DRIVER_TYPE_HARDWARE;
    Flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    Flags &= ~D3D11_CREATE_DEVICE_SINGLETHREADED;

    if(f) { f = fopen("C:\\d3d11_proxy_log.txt", "a"); if(f){
        fprintf(f, "[Verantyx] SwapChain requested %d feature levels. SDKVersion=%u\n", FeatureLevels, SDKVersion);
        for(UINT i=0; i<FeatureLevels; i++) {
            fprintf(f, "  FeatureLevel[%d] = %04X\n", i, pFeatureLevels[i]);
        }
        fclose(f);
    } }

    ID3D11Device* pOrigDevice = NULL;
    HRESULT hr = pOrig(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, &pOrigDevice, pFeatureLevel, ppImmediateContext);
    
    if (FAILED(hr)) {
        if(f) { f = fopen("C:\\d3d11_proxy_log.txt", "a"); if(f){fprintf(f, "[Verantyx] SwapChain device creation still failed with hr=%08X\n", hr); fclose(f);} }
    }
    if (SUCCEEDED(hr) && pOrigDevice) {
        SetupDeviceHooks(pOrigDevice);
        if (ppDevice) {
            *ppDevice = pOrigDevice;
            if(f) { f = fopen("C:\\d3d11_proxy_log.txt", "a"); if(f){fprintf(f, "[Verantyx] Returned native ID3D11Device from SwapChain with resource hooks installed.\n"); fclose(f);} }
        } else {
            pOrigDevice->Release();
        }
    } else if (ppDevice) {
        *ppDevice = pOrigDevice;
    } else if (pOrigDevice) {
        pOrigDevice->Release();
    }
    return hr;
}
