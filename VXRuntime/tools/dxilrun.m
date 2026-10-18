// Runs translated Shader Model 6 compute shaders on the GPU and checks what they wrote: the translation's meaning,
// not only that Metal compiles it (tools/dxil2msl). Resources are bound as the D3D12 runtime binds them: VxRoot at
// buffer 0 with a pointer per resource range to its descriptors (VxDesc, src/dxil_msl.h).
//   build/dxilrun tests/shaders12
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/dxil_msl.h"

typedef struct { uint64_t va, res; uint32_t size, stride, fmt, flags; } VxDesc;

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;

static int translate(const char *path, dxil_msl *out, id<MTLComputePipelineState> *pso) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(size);
    fread(data, 1, size, f);
    fclose(f);
    dxil_module m;
    int rc = dxil_read(data, size, &m) ? (printf("%s: %s\n", path, m.error), -1) : dxil_to_msl(&m, out) ? (printf("%s: %s\n", path, out->error), -1) : 0;
    if (!rc) dxil_free(&m);
    free(data);
    if (rc) return -1;
    MTLCompileOptions *opt = [MTLCompileOptions new];
    opt.languageVersion = MTLLanguageVersion3_1;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(out->msl) options:opt error:&err];
    if (lib) *pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"vx_main"] error:&err];
    if (!lib || !*pso) { printf("%s: Metal: %s\n", path, err.localizedDescription.UTF8String); return -1; }
    return 0;
}

// sum_cs: a groupshared reduction of 64 words read from a ByteAddressBuffer, an InterlockedAdd, a switch writing a
// RWStructuredBuffer, WaveActiveSum and WaveGetLaneIndex written to a RWTexture2D.
static int run_sum(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/sum_cs.dxil", dir);
    dxil_msl out;
    id<MTLComputePipelineState> pso = nil;
    if (translate(path, &out, &pso)) return 1;
    uint32_t in[64], expect_sum = 0;
    for (int i = 0; i < 64; i++) { in[i] = (uint32_t)(i * 7 + 3); expect_sum += in[i]; }
    id<MTLBuffer> inb = [dev newBufferWithBytes:in length:sizeof in options:MTLResourceStorageModeShared];
    id<MTLBuffer> outb = [dev newBufferWithLength:66 * 4 options:MTLResourceStorageModeShared];
    memset(outb.contents, 0, 66 * 4);
    ((uint32_t *)outb.contents)[0] = 1000;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:64 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> img = [dev newTextureWithDescriptor:td];
    // descriptors: one per range, in the shader's slot order
    id<MTLBuffer> descs = [dev newBufferWithLength:sizeof(VxDesc) * out.nslots options:MTLResourceStorageModeShared];
    VxDesc *d = descs.contents;
    for (uint32_t i = 0; i < out.nslots; i++) {
        const dxil_msl_slot *s = &out.slots[i];
        memset(&d[i], 0, sizeof d[i]);
        if (s->cls == DXIL_SRV && s->lower == 0) { d[i].va = inb.gpuAddress; d[i].size = sizeof in; }
        else if (s->cls == DXIL_UAV && s->lower == 0) { d[i].va = outb.gpuAddress; d[i].size = 66 * 4; d[i].stride = 4; }
        else if (s->cls == DXIL_UAV && s->lower == 1) { MTLResourceID id = img.gpuResourceID; memcpy(&d[i].res, &id, 8); }
    }
    uint64_t root[64 + 2] = { 0 };
    for (uint32_t i = 0; i < out.nslots; i++) root[i] = descs.gpuAddress + i * sizeof(VxDesc);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBytes:root length:sizeof(uint64_t) * (out.nslots + 2) atIndex:0];
    [enc useResource:inb usage:MTLResourceUsageRead];
    [enc useResource:outb usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [enc useResource:img usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [enc useResource:descs usage:MTLResourceUsageRead];
    [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(out.threads[0], out.threads[1], out.threads[2])];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    int bad = 0;
    uint32_t *o = outb.contents;
    if (o[0] != 1000 + expect_sum) { printf("sum_cs: InterlockedAdd gave %u, want %u\n", o[0], 1000 + expect_sum); bad++; }
    for (uint32_t gi = 0; gi < 64; gi++) {
        uint32_t want = (gi & 3) == 0 ? 1 : (gi & 3) == 1 ? ({ float s = sqrtf((float)gi); uint32_t u; memcpy(&u, &s, 4); u; }) : (uint32_t)__builtin_popcount(gi);
        int ulp1 = (gi & 3) == 1 && (o[gi + 1] == want + 1 || o[gi + 1] + 1 == want);   // (sqrt: D3D allows 1 ULP; Metal's is within it)
        if (o[gi + 1] != want && !ulp1) { if (bad++ < 5) printf("sum_cs: outBuf[%u] = %#x, want %#x\n", gi + 1, o[gi + 1], want); }
    }
    float px[64][4];
    [img getBytes:px bytesPerRow:sizeof px fromRegion:MTLRegionMake2D(0, 0, 64, 1) mipmapLevel:0];
    uint32_t w = (uint32_t)pso.threadExecutionWidth;
    for (uint32_t x = 0; x < 64; x++) {
        uint32_t base = x / w * w, sum = 0;
        for (uint32_t k = base; k < base + w; k++) sum += k;
        if (px[x][0] != (float)sum || px[x][1] != (float)(x % w) || px[x][3] != 1) { if (bad++ < 8) printf("sum_cs: img[%u] = (%g, %g, %g, %g)\n", x, px[x][0], px[x][1], px[x][2], px[x][3]); }
    }
    printf("sum_cs: groupshared reduction, InterlockedAdd, switch, wave sum over %u lanes: %s\n", w, bad ? "WRONG" : "ok");
    dxil_msl_free(&out);
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/shaders12";
    dev = MTLCreateSystemDefaultDevice();
    queue = [dev newCommandQueue];
    int bad = run_sum(dir);
    printf("dxilrun: %s\n", bad ? "FAILED" : "ok");
    return bad;
}
