// Finds the DXBC containers embedded in files (DLLs, executables, shader caches), translates each distinct one and
// compiles the result with Metal, and reports what fails, grouped by message: a robustness check of the translator
// against real shaders. Nothing is written unless -d is given (failing shaders are then saved there, for study).
//   build/dxbcscan [-v] [-12] [-d dir] [-s N] files...     (-s N: compile every Nth with Metal; -t N: translate every Nth;
//                                                          -12: as Direct3D 12 pipelines take them, vertex/pixel/geometry/compute)
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/dxbc.h"

typedef struct { const uint8_t *data; size_t size; char origin[256]; int type, major; } blob;
static blob *g_blobs; static size_t g_count, g_cap;
static NSMutableSet *g_seen;

static uint32_t rd(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
// The program type and version of a container's SHDR/SHEX chunk (-1: none, -2: DXIL).
static void program_type(const uint8_t *c, size_t size, int *type, int *major) {
    *type = -1; *major = 0;
    uint32_t n = rd(c + 28);
    for (uint32_t i = 0; i < n && 32 + 4 * i + 4 <= size; i++) {
        uint32_t off = rd(c + 32 + 4 * i);
        if (off + 12 > size) continue;
        if (!memcmp(c + off, "SHDR", 4) || !memcmp(c + off, "SHEX", 4)) {
            uint32_t v = rd(c + off + 8);
            *type = (int)(v >> 16 & 0xffff); *major = (int)(v >> 4 & 0xf);
            return;
        }
        if (!memcmp(c + off, "DXIL", 4)) *type = -2;
    }
}
static void scan(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 32) { close(fd); return; }
    const uint8_t *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return;
    size_t size = (size_t)st.st_size;
    for (const uint8_t *p = m; (p = memmem(p, size - (size_t)(p - m), "DXBC", 4)); p += 4) {
        size_t left = size - (size_t)(p - m);
        if (left < 32) break;
        uint32_t one = rd(p + 20), total = rd(p + 24), chunks = rd(p + 28);
        if (one != 1 || total < 32 || total > left || chunks == 0 || chunks > 64 || 32 + 4 * chunks > total) continue;
        unsigned char digest[32];
        CC_SHA256(p, total, digest);
        NSData *key = [NSData dataWithBytes:digest length:16];
        if ([g_seen containsObject:key]) continue;
        [g_seen addObject:key];
        if (g_count == g_cap) g_blobs = realloc(g_blobs, (g_cap = g_cap ? g_cap * 2 : 1024) * sizeof *g_blobs);
        blob *b = &g_blobs[g_count++];
        b->data = p; b->size = total;
        const char *slash = strrchr(path, '/');
        snprintf(b->origin, sizeof b->origin, "%s+%#zx", slash ? slash + 1 : path, (size_t)(p - m));
        program_type(p, total, &b->type, &b->major);
    }
}

// A failure message with its numbers taken out, so failures of one kind group together.
static NSString *kind(const char *msg) {
    NSMutableString *s = [NSMutableString string];
    for (const char *c = msg; *c && s.length < 160; c++) {
        if (*c >= '0' && *c <= '9') { if (![s hasSuffix:@"#"]) [s appendString:@"#"]; continue; }
        [s appendFormat:@"%c", *c];
    }
    return s;
}
// The first error line of Metal's log.
static NSString *first_error(NSString *log) {
    for (NSString *line in [log componentsSeparatedByString:@"\n"]) {
        NSRange r = [line rangeOfString:@"error: "];
        if (r.location != NSNotFound) return [line substringFromIndex:r.location + r.length];
    }
    return [log substringToIndex:MIN(log.length, (NSUInteger)160)];
}

int main(int argc, char **argv) {
    @autoreleasepool {
        int verbose = 0;
        __block int d3d12 = 0;
        __block size_t stride = 1, tstride = 1;
        const char *dump = NULL;
        int i = 1;
        for (; i < argc && argv[i][0] == '-'; i++) {
            if (!strcmp(argv[i], "-v")) verbose = 1;
            else if (!strcmp(argv[i], "-12")) d3d12 = 1;
            else if (!strcmp(argv[i], "-d") && i + 1 < argc) dump = argv[++i];
            else if (!strcmp(argv[i], "-s") && i + 1 < argc) stride = (size_t)atol(argv[++i]) ?: 1;
            else if (!strcmp(argv[i], "-t") && i + 1 < argc) tstride = (size_t)atol(argv[++i]) ?: 1;
        }
        g_seen = [NSMutableSet set];
        for (; i < argc; i++) scan(argv[i]);
        static const char *names[] = { "pixel", "vertex", "geometry", "hull", "domain", "compute" };
        int by_type[8] = { 0 }, dxil = 0, none = 0;
        static int ok_type[8];
        NSMutableDictionary<NSString *, NSMutableArray *> *failures = [NSMutableDictionary dictionary];
        NSLock *lock = [NSLock new];
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        __block int ok = 0, failed = 0;
        dispatch_apply(g_count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t k) {
            @autoreleasepool {
                blob *b = &g_blobs[k];
                if (b->type < 0 || k % tstride || (d3d12 && b->type >= 3 && b->type <= 4)) return;
                dxbc_shader sh;
                NSString *why = nil;
                if ((d3d12 ? dxbc_translate_d3d12 : dxbc_translate)(b->data, b->size, &sh)) why = [@"translate: " stringByAppendingString:kind(sh.error)];
                else if (k % stride) dxbc_free(&sh);
                else {
                    NSError *err = nil;
                    id<MTLLibrary> lib = [dev newLibraryWithSource:@(sh.msl) options:nil error:&err];
                    if (!lib) why = [@"metal: " stringByAppendingString:kind(first_error(err.localizedDescription).UTF8String)];
                    dxbc_free(&sh);
                }
                [lock lock];
                if (why) {
                    failed++;
                    NSMutableArray *list = failures[why] ?: (failures[why] = [NSMutableArray array]);
                    [list addObject:@(k)];
                } else { ok++; ok_type[b->type & 7]++; }
                [lock unlock];
            }
        });
        for (size_t k = 0; k < g_count; k++) {
            if (g_blobs[k].type == -2) dxil++;
            else if (g_blobs[k].type < 0) none++;
            else if (!(k % tstride) && !(d3d12 && g_blobs[k].type >= 3 && g_blobs[k].type <= 4)) by_type[g_blobs[k].type & 7]++;
        }
        printf("%zu distinct containers: %d DXIL (shader model 6), %d without code\n", g_count, dxil, none);
        for (int t = 0; t < 6; t++) if (by_type[t]) printf("  %-8s %5d, %5d translate and compile\n", names[t], by_type[t], ok_type[t]);
        NSArray *keys = [failures keysSortedByValueUsingComparator:^NSComparisonResult(NSArray *a, NSArray *b) { return a.count < b.count ? NSOrderedDescending : a.count > b.count ? NSOrderedAscending : NSOrderedSame; }];
        int tried = 0;
        for (int t = 0; t < 8; t++) tried += by_type[t];
        printf("%d of %d tried pass%s; failures:\n", ok, tried, stride > 1 ? " (Metal compiles a sample)" : "");
        for (NSString *k in keys) {
            NSArray *list = failures[k];
            blob *b = &g_blobs[[list[0] unsignedLongValue]];
            printf("%5lu  %s   (e.g. %s %s %d)\n", (unsigned long)list.count, k.UTF8String, b->origin, b->type >= 0 && b->type < 6 ? names[b->type] : "?", b->major);
            if (dump) {
                for (NSUInteger j = 0; j < list.count && j < (verbose ? list.count : 3); j++) {
                    blob *f = &g_blobs[[list[j] unsignedLongValue]];
                    char path[1024];
                    snprintf(path, sizeof path, "%s/%s.dxbc", dump, f->origin);
                    FILE *o = fopen(path, "wb");
                    if (o) { fwrite(f->data, 1, f->size, o); fclose(o); }
                }
            }
        }
        return failed != 0;
    }
}
