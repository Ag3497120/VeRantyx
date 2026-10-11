#!/usr/bin/env python3
"""Apply VeRantyx's Klepton changes to a Klepton checkout (idempotent).

    NativeARM/klepton-patches/apply.py <klepton dir>

New files (runtime/kl_tls.*) are copied in; the edits to existing files are
anchored string insertions so they survive small upstream drift and can be
re-run. Each edit asserts its anchor exists, and is skipped if already applied.
"""
import os
import re
import shutil
import sys

ROOT = sys.argv[1]
HERE = os.path.dirname(os.path.abspath(__file__))


def patch(relpath, edits):
    path = os.path.join(ROOT, relpath)
    text = open(path).read()
    orig = text
    for anchor, insert, where, marker in edits:
        if marker in text:
            continue
        idx = text.find(anchor)
        assert idx >= 0, f"{relpath}: anchor not found: {anchor[:60]!r}"
        if where == "after":
            text = text[: idx + len(anchor)] + insert + text[idx + len(anchor):]
        else:
            text = text[:idx] + insert + text[idx:]
    if text != orig:
        open(path, "w").write(text)
        print(f"patched {relpath}")
    else:
        print(f"already  {relpath}")


for name in ("kl_tls.h", "kl_tls.c", "kl_tls.S"):
    src = os.path.join(HERE, "runtime", name)
    dst = os.path.join(ROOT, "runtime", name)
    if not os.path.exists(dst) or open(src, "rb").read() != open(dst, "rb").read():
        shutil.copyfile(src, dst)
        print(f"copied   runtime/{name}")

# ---- Makefile: build the TLS runtime with everything else
patch("Makefile", [(
    "RUNTIME_SHIP := runtime/kl_env.c runtime/kl_image.c",
    " runtime/kl_tls.c runtime/kl_tls.S", "after", "runtime/kl_tls.c")])

# ---- klepton.h: stats, TLS symbol lookups
patch("runtime/klepton.h", [
    ('#include "kl_env.h"', '\n#include "kl_tls.h"', "after", '#include "kl_tls.h"'),
    ("    unsigned ctr_sites, ctr_patched, ctr_refused;",
     "\n    unsigned tls_desc, tls_tprel;                    // ELF TLS relocations (glibc guests)",
     "after", "tls_desc, tls_tprel"),
    ("void     *kl_guest_sym_global(const char *name);",
     "\n// ELF TLS symbols: a defined STT_TLS symbol's offset inside its module's block.\n"
     "int       kl_sym_tls(kl_image *img, const char *name, uint64_t *off);\n"
     "kl_tls_module *kl_image_tls_module(kl_image *img);\n"
     "int       kl_guest_tls_sym_global(const char *name, kl_tls_module **m, uint64_t *off);",
     "after", "kl_guest_tls_sym_global"),
])

# ---- kl_image.c: PT_TLS registration, TLSDESC / TPREL64 relocations
patch("runtime/kl_image.c", [
    ("#define STB_WEAK 2\n",
     "#define PT_TLS 7\n#define R_AARCH64_TLS_TPREL64 1030\n#define R_AARCH64_TLSDESC 1031\n#define STT_TLS 6\n",
     "after", "R_AARCH64_TLSDESC 1031"),
    ("    void       *dl_handle;     // set when the image arrived as a translated",
     "    kl_tls_module *tls;        // PT_TLS, registered with kl_tls.c (glibc guests)\n",
     "before", "kl_tls_module *tls;"),
    ("        default:\n            err(\"unhandled relocation type %u at %#llx\"",
     """        case R_AARCH64_TLS_TPREL64:
        case R_AARCH64_TLSDESC: {
            // ELF TLS (glibc guests). The symbol, if any, is an STT_TLS whose
            // value is an offset inside its module's TLS block — this image's
            // when defined here, another loaded image's when imported.
            kl_tls_module *m = img->tls;
            uint64_t off = 0;
            if (sidx) {
                const Elf64_Sym *s = &img->symtab[sidx];
                nm = img->strtab + s->st_name;
                if (s->st_shndx != 0) off = s->st_value;
                else if (kl_guest_tls_sym_global(nm, &m, &off) != 0) m = NULL;
            }
            if (!m) {
                img->stats.imports_missing++;
                record_missing(img, nm ? nm : "(TLS without PT_TLS)");
                if (type == R_AARCH64_TLSDESC) { slot[0] = (uint64_t)(uintptr_t)unresolved_stub(nm ? nm : "tlsdesc"); slot[1] = 0; }
                else *slot = 0;
                break;
            }
            off += (uint64_t)r->r_addend;
            if (type == R_AARCH64_TLS_TPREL64) {
                uint64_t v;
                if (kl_tls_tprel(m, off, &v) != 0) { err("%s: %s", img->path, kl_tls_error()); return -1; }
                *slot = v;
                img->stats.tls_tprel++;
            } else {
                kl_tls_desc(m, off, slot);
                img->stats.tls_desc++;
            }
            break;
        }
""", "before", "case R_AARCH64_TLSDESC: {"),
    ("    // Order is independent — RELA, RELR and APS2 relocate disjoint slots",
     """    // PT_TLS: register the module's thread-local block before any TLS
    // relocation asks for an offset into it. A module with TPREL64 relocations
    // needs TP-relative placement; everything else goes in the per-thread block.
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_TLS) continue;
        int fixed = 0;
        const Elf64_Rela *tabs[2] = { rela, jmprel };
        size_t cnts[2] = { relasz / sizeof(Elf64_Rela), pltsz / sizeof(Elf64_Rela) };
        for (int t = 0; t < 2 && !fixed; t++)
            for (size_t k = 0; tabs[t] && k < cnts[t]; k++)
                if (ELF64_R_TYPE(tabs[t][k].r_info) == R_AARCH64_TLS_TPREL64) { fixed = 1; break; }
        img->tls = kl_tls_register(img->path, img->base + ph[i].p_vaddr - lo,
                                   (size_t)ph[i].p_filesz, (size_t)ph[i].p_memsz,
                                   (size_t)ph[i].p_align, fixed);
        if (!img->tls) { err("%s: %s", img->path, kl_tls_error()); return -1; }
        break;
    }

""", "before", "PT_TLS: register the module's thread-local block"),
    ("        if (s->st_shndx == 0 || !s->st_name) continue;\n        const char *nm = img->strtab + s->st_name;\n        if (nm < lo_p || nm >= hi_p) continue;               // not a real name\n        if (strcmp(nm, name) == 0)\n            return img->base + s->st_value;",
     "        if ((s->st_info & 0xf) == STT_TLS) continue;         // an offset, not an address: kl_sym_tls\n",
     "before", "an offset, not an address: kl_sym_tls"),
    ("void kl_unload(kl_image *img) {",
     """int kl_sym_tls(kl_image *img, const char *name, uint64_t *off) {
    if (!img || !img->tls) return -1;
    size_t n = img->nsyms;
    if (!n) n = ((const char *)img->strtab - (const char *)img->symtab) / sizeof(Elf64_Sym);
    const char *lo_p = (const char *)img->base, *hi_p = lo_p + img->span;
    for (size_t i = 0; i < n; i++) {
        const Elf64_Sym *s = &img->symtab[i];
        if ((const char *)(s + 1) > hi_p) break;
        if (s->st_shndx == 0 || !s->st_name || (s->st_info & 0xf) != STT_TLS) continue;
        const char *nm = img->strtab + s->st_name;
        if (nm < lo_p || nm >= hi_p) continue;
        if (strcmp(nm, name) == 0) { if (off) *off = s->st_value; return 0; }
    }
    return -1;
}

kl_tls_module *kl_image_tls_module(kl_image *img) { return img ? img->tls : NULL; }

""", "before", "int kl_sym_tls(kl_image *img"),
])

# ---- rewrite_tls: KL_TLS_REWRITE_FORCE=1 skips the data heuristic. glibc
# libraries keep strings next to functions, and the heuristic refused a real
# `mrs x2, tpidr_el0` in libtier0's thread-local accessor for that reason.
_p = os.path.join(ROOT, "runtime/kl_image.c"); _t = open(_p).read()
_t2 = _t.replace("        if ((w[i] & 0xffffffe0u) == 0xd53bd040u) {\n            if (kl_x18_is_data(w, n, i)) {",
                 "        if ((w[i] & 0xffffffe0u) == 0xd53bd040u) {\n            if (!kl_env_on(\"KL_TLS_REWRITE_FORCE\", 0) && kl_x18_is_data(w, n, i)) {")
if _t2 != _t: open(_p, "w").write(_t2); print("patched runtime/kl_image.c (rewrite force)")
_p = os.path.join(ROOT, "tools/klepton_ld.c"); _t = open(_p).read()
_t2 = _t.replace("        if (kl_x18_is_data(w, n, i)) {\n            (*refused)++;",
                 "        if (!getenv(\"KL_TLS_REWRITE_FORCE\") && kl_x18_is_data(w, n, i)) {\n            (*refused)++;")
if _t2 != _t: open(_p, "w").write(_t2); print("patched tools/klepton_ld.c (rewrite force)")

# ---- kl_libc.c: fcntl file locking (Source 2 takes a lock file at startup)
_p = os.path.join(ROOT, "runtime/libc/kl_libc.c"); _t = open(_p).read()
_old = """    case LX_F_GETLK: case LX_F_SETLK: case LX_F_SETLKW:
        warn_once("fcntl file locking (struct flock layout differs) — refused");
        errno = EINVAL;
        return -1;"""
_new = """    case LX_F_GETLK: case LX_F_SETLK: case LX_F_SETLKW: {
        // Linux/aarch64 struct flock leads with l_type/l_whence, Darwin's with
        // l_start/l_len, and the F_RDLCK/F_WRLCK/F_UNLCK values differ (0/1/2 vs
        // 1/3/2), so the record is translated both ways.
        struct lx_flock { int16_t l_type, l_whence; int32_t pad; int64_t l_start, l_len; int32_t l_pid, pad2; };
        struct lx_flock *lx = (struct lx_flock *)arg;
        struct flock d; memset(&d, 0, sizeof d);
        d.l_type = lx->l_type == 0 ? F_RDLCK : lx->l_type == 1 ? F_WRLCK : F_UNLCK;
        d.l_whence = lx->l_whence; d.l_start = lx->l_start; d.l_len = lx->l_len; d.l_pid = lx->l_pid;
        int dcmd = cmd == LX_F_GETLK ? F_GETLK : cmd == LX_F_SETLK ? F_SETLK : F_SETLKW;
        int r = fcntl(fd, dcmd, &d);
        if (r == 0 && cmd == LX_F_GETLK) {
            lx->l_type = d.l_type == F_RDLCK ? 0 : d.l_type == F_WRLCK ? 1 : 2;
            lx->l_whence = d.l_whence; lx->l_start = d.l_start; lx->l_len = d.l_len; lx->l_pid = d.l_pid;
        }
        return r;
    }"""
if _old in _t: open(_p, "w").write(_t.replace(_old, _new)); print("patched runtime/libc/kl_libc.c (fcntl locks)")
elif "struct lx_flock" not in _t: raise SystemExit("kl_libc.c: fcntl lock anchor not found")

# ---- kl_dl.c: host modules — a guest dlopen of a path with a registered suffix
# returns a handle whose dlsym is answered by host code (VeRantyx's OpenVR
# runtime stands in for vrclient.so this way).
patch("runtime/kl_dl.c", [
    ("void *klb_dlopen(const char *path, int flags) {",
     """// ---- host modules ----
// A library the HOST implements for the guest, keyed by a path suffix
// ("/vrclient.so"): dlopen of such a path returns a handle whose dlsym asks the
// registered lookup. Up to 8; the handle is the registry entry's address.
struct kl_host_module { const char *suffix; void *(*lookup)(const char *); };
static struct kl_host_module g_host_mods[8];
static unsigned g_host_mods_n;
void kl_dl_add_host_module(const char *suffix, void *(*lookup)(const char *)) {
    if (g_host_mods_n < 8) { g_host_mods[g_host_mods_n].suffix = suffix; g_host_mods[g_host_mods_n].lookup = lookup; g_host_mods_n++; }
}
static void *kl_host_module_dlopen(const char *path) {
    size_t n = strlen(path);
    for (unsigned i = 0; i < g_host_mods_n; i++) {
        size_t m = strlen(g_host_mods[i].suffix);
        if (n >= m && !strcmp(path + n - m, g_host_mods[i].suffix)) {
            fprintf(stderr, "  [klepton] guest dlopen(%s) -> host module\\n", path);
            return &g_host_mods[i];
        }
    }
    return NULL;
}
static struct kl_host_module *kl_host_module_handle(void *h) {
    for (unsigned i = 0; i < g_host_mods_n; i++) if (h == (void *)&g_host_mods[i]) return &g_host_mods[i];
    return NULL;
}

""", "before", "struct kl_host_module {"),
    ("    if (kl_core_shim_claims(path)) {\n        fprintf(stderr, \"  [klepton] guest dlopen(\\\"%s\\\") -> shim-served handle\\n\", path);",
     "    { void *hm = kl_host_module_dlopen(path); if (hm) return hm; }\n", "before", "kl_host_module_dlopen(path); if (hm)"),
    ("void *klb_dlsym(void *handle, const char *name) {\n",
     "    { struct kl_host_module *hm = kl_host_module_handle(handle); if (hm) return hm->lookup(name); }\n",
     "after", "kl_host_module_handle(handle); if (hm)"),
])
patch("runtime/klepton.h", [
    ("int       kl_guest_tls_sym_global(const char *name, kl_tls_module **m, uint64_t *off);",
     "\n// A host-implemented library for the guest, by dlopen path suffix (\"/vrclient.so\").\nvoid      kl_dl_add_host_module(const char *suffix, void *(*lookup)(const char *));",
     "after", "kl_dl_add_host_module"),
])

# ---- kl_vulkan.c: Linux WSI. Source 2's render system insists on one of
# VK_KHR_xcb_surface / xlib / wayland at instance creation even with -nowindow;
# serve them exactly like VK_KHR_android_surface (the synthetic swapchain).
_p = os.path.join(ROOT, "runtime/gfx/kl_vulkan.c"); _t = open(_p).read(); _orig = _t
if "klvk_CreateLinuxSurfaceKHR" not in _t:
    _t = _t.replace("    VkExtensionProperties *tmp = calloc(n + 2, sizeof *tmp);",
                    "    VkExtensionProperties *tmp = calloc(n + 8, sizeof *tmp);")
    _t = _t.replace("""        tmp[n].specVersion = 6;
        n++;
    }
""", """        tmp[n].specVersion = 6;
        n++;
    }
    // ...and the Linux window-system surfaces, for a Linux guest (Source 2 asks
    // for xcb/xlib/wayland and fails Vulkan init without one). Same synthetic
    // surface behind all of them.
    if (!layer) {
        static const char *linux_wsi[] = { "VK_KHR_xcb_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface" };
        for (unsigned k = 0; k < 3; k++)
            if (!ext_supported(tmp, n, linux_wsi[k])) {
                snprintf(tmp[n].extensionName, sizeof tmp[n].extensionName, "%s", linux_wsi[k]);
                tmp[n].specVersion = 6;
                n++;
            }
    }
""", 1)
    _m = re.search(r'        if \(!strcmp\(e, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME\)\) \{\n(.*?)\n            continue;\n        \}', _t, re.S)
    if _m:
        _t = (_t[:_m.start()] + '        if (!strcmp(e, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME) || !strcmp(e, "VK_KHR_xcb_surface") ||\n'
              '            !strcmp(e, "VK_KHR_xlib_surface") || !strcmp(e, "VK_KHR_wayland_surface")) {\n' + _m.group(1) +
              '\n            continue;\n        }' + _t[_m.end():])
    _t = _t.replace("""static klvk_surface *surf_of(VkSurfaceKHR h) {""", """// The Linux WSI entry points: the create-info is window-system specific and
// ignored; the surface is the same synthetic one.
static VkResult klvk_CreateLinuxSurfaceKHR(VkInstance inst, const void *ci,
        const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    (void)ci;
    return klvk_CreateAndroidSurfaceKHR(inst, NULL, alloc, out);
}
static VkBool32 klvk_GetPhysicalDeviceLinuxPresentationSupportKHR(VkPhysicalDevice p, uint32_t fam, void *a, void *b) {
    (void)p; (void)fam; (void)a; (void)b;
    return VK_TRUE;
}

static klvk_surface *surf_of(VkSurfaceKHR h) {""", 1)
    _t = _t.replace("""    E(CreateAndroidSurfaceKHR), E(DestroySurfaceKHR),""", """    E(CreateAndroidSurfaceKHR), E(DestroySurfaceKHR),
    { "vkCreateXcbSurfaceKHR", (PFN_vkVoidFunction)klvk_CreateLinuxSurfaceKHR },
    { "vkCreateXlibSurfaceKHR", (PFN_vkVoidFunction)klvk_CreateLinuxSurfaceKHR },
    { "vkCreateWaylandSurfaceKHR", (PFN_vkVoidFunction)klvk_CreateLinuxSurfaceKHR },
    { "vkGetPhysicalDeviceXcbPresentationSupportKHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceLinuxPresentationSupportKHR },
    { "vkGetPhysicalDeviceXlibPresentationSupportKHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceLinuxPresentationSupportKHR },
    { "vkGetPhysicalDeviceWaylandPresentationSupportKHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceLinuxPresentationSupportKHR },""", 1)
# ---- kl_vulkan.c: device features the guest asks for but MoltenVK lacks are
# masked off instead of failing vkCreateDevice with VK_ERROR_FEATURE_NOT_PRESENT
# (Source 2 asks for a superset and copes with what it gets).
if "klvk_clamp_features" not in _t:
    _t = _t.replace("""static VkResult klvk_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,""",
"""// Every feature struct is { sType, pNext, VkBool32... }; knowing a struct's size
// is enough to query what the device supports (vkGetPhysicalDeviceFeatures2 with
// a zeroed copy chained) and AND the request against it.
static const struct { VkStructureType st; size_t size; const char *name; } klvk_feature_structs[] = {
#define FS(T, ST) { ST, sizeof(T), #T }
    FS(VkPhysicalDeviceVulkan11Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES),
    FS(VkPhysicalDeviceVulkan12Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES),
    FS(VkPhysicalDeviceVulkan13Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES),
    FS(VkPhysicalDeviceMultiviewFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES),
    FS(VkPhysicalDevice16BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES),
    FS(VkPhysicalDevice8BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES),
    FS(VkPhysicalDeviceShaderFloat16Int8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES),
    FS(VkPhysicalDeviceDynamicRenderingFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES),
    FS(VkPhysicalDeviceDynamicRenderingLocalReadFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES),
    FS(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT),
    FS(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT),
    FS(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT),
    FS(VkPhysicalDeviceHostImageCopyFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES),
    FS(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES),
    FS(VkPhysicalDeviceImagelessFramebufferFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES),
    FS(VkPhysicalDeviceDescriptorIndexingFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES),
    FS(VkPhysicalDeviceBufferDeviceAddressFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES),
    FS(VkPhysicalDeviceTimelineSemaphoreFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES),
    FS(VkPhysicalDeviceShaderDrawParametersFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES),
    FS(VkPhysicalDeviceSamplerYcbcrConversionFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES),
    FS(VkPhysicalDeviceVariablePointersFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES),
    FS(VkPhysicalDeviceUniformBufferStandardLayoutFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES),
    FS(VkPhysicalDeviceScalarBlockLayoutFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES),
    FS(VkPhysicalDeviceHostQueryResetFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES),
    FS(VkPhysicalDeviceVulkanMemoryModelFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES),
    FS(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES),
    FS(VkPhysicalDeviceShaderAtomicInt64Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES),
    FS(VkPhysicalDeviceFragmentDensityMapFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT),
    FS(VkPhysicalDeviceMemoryPriorityFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT),
    FS(VkPhysicalDeviceRobustness2FeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT),
    FS(VkPhysicalDeviceSynchronization2Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES),
    FS(VkPhysicalDeviceMaintenance4Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES),
    FS(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES),
    FS(VkPhysicalDeviceImageRobustnessFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES),
    FS(VkPhysicalDeviceInlineUniformBlockFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES),
    FS(VkPhysicalDevicePipelineCreationCacheControlFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES),
    FS(VkPhysicalDevicePrivateDataFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES),
    FS(VkPhysicalDeviceShaderIntegerDotProductFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES),
    FS(VkPhysicalDeviceShaderTerminateInvocationFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES),
    FS(VkPhysicalDeviceSubgroupSizeControlFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES),
    FS(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES),
    FS(VkPhysicalDeviceCustomBorderColorFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT),
    FS(VkPhysicalDeviceIndexTypeUint8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES),
    FS(VkPhysicalDeviceLineRasterizationFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES),
    FS(VkPhysicalDeviceVertexAttributeDivisorFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES),
    FS(VkPhysicalDeviceDepthClipEnableFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT),
    FS(VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT),
    FS(VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT),
    FS(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT),
    FS(VkPhysicalDeviceTextureCompressionASTCHDRFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES),
    FS(VkPhysicalDeviceMultiDrawFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT),
    FS(VkPhysicalDeviceDepthClampZeroOneFeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_ZERO_ONE_FEATURES_KHR),
    FS(VkPhysicalDeviceShaderClockFeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR),
    FS(VkPhysicalDeviceConditionalRenderingFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT),
    FS(VkPhysicalDeviceTransformFeedbackFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT),
    FS(VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT),
    FS(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT),
    FS(VkPhysicalDeviceMaintenance5Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES),
    FS(VkPhysicalDeviceMaintenance6Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES),
#undef FS
};
static size_t klvk_feature_struct_size(VkStructureType st, const char **name) {
    for (size_t i = 0; i < sizeof klvk_feature_structs / sizeof *klvk_feature_structs; i++)
        if (klvk_feature_structs[i].st == st) { if (name) *name = klvk_feature_structs[i].name; return klvk_feature_structs[i].size; }
    return 0;
}
// AND the requested features (pEnabledFeatures and every known feature struct in
// the pNext chain, which is modified in place: it is the guest's memory, but a
// bit the device lacks could never have been honoured) against what MoltenVK
// reports. Returns the number of features turned off.
static unsigned klvk_clamp_features(VkPhysicalDevice phys, VkDeviceCreateInfo *ci) {
    PFN_vkGetPhysicalDeviceFeatures gf = (PFN_vkGetPhysicalDeviceFeatures)real_gipa(g_instance, "vkGetPhysicalDeviceFeatures");
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = (PFN_vkGetPhysicalDeviceFeatures2)real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2");
    if (!gf2) gf2 = (PFN_vkGetPhysicalDeviceFeatures2)real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2KHR");
    unsigned off = 0;
    VkPhysicalDeviceFeatures have; memset(&have, 0, sizeof have);
    if (gf) gf(phys, &have);
    VkBool32 *hb = (VkBool32 *)&have;
    if (ci->pEnabledFeatures && gf) {
        VkBool32 *rb = (VkBool32 *)ci->pEnabledFeatures;
        for (size_t i = 0; i < sizeof have / sizeof(VkBool32); i++)
            if (rb[i] && !hb[i]) { rb[i] = VK_FALSE; off++; VKI("device feature #%zu (VkPhysicalDeviceFeatures) not supported — turned off\\n", i); }
    }
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)ci->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            VkBool32 *rb = (VkBool32 *)&((VkPhysicalDeviceFeatures2 *)p)->features;
            for (size_t i = 0; gf && i < sizeof have / sizeof(VkBool32); i++)
                if (rb[i] && !hb[i]) { rb[i] = VK_FALSE; off++; VKI("device feature #%zu (VkPhysicalDeviceFeatures2) not supported — turned off\\n", i); }
            continue;
        }
        const char *name = NULL;
        size_t size = klvk_feature_struct_size(p->sType, &name);
        if (!size || !gf2) continue;
        uint8_t *probe = calloc(1, size);
        if (!probe) continue;
        ((VkBaseOutStructure *)probe)->sType = p->sType;
        VkPhysicalDeviceFeatures2 q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, probe };
        gf2(phys, &q);
        VkBool32 *rb = (VkBool32 *)((uint8_t *)p + 16), *sb = (VkBool32 *)(probe + 16);
        for (size_t i = 0; i < (size - 16) / sizeof(VkBool32); i++)
            if (rb[i] && !sb[i]) { rb[i] = VK_FALSE; off++; VKI("device feature %s field #%zu not supported — turned off\\n", name, i); }
        free(probe);
    }
    return off;
}

static VkResult klvk_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,""", 1)
    _t = _t.replace("""    VkResult r = real(phys, &mine, alloc, out);
    free(have);
    if (r != VK_SUCCESS) { VKI("vkCreateDevice FAILED %d\\n", r); return r; }""", """    VkResult r = real(phys, &mine, alloc, out);
    if (r == VK_ERROR_FEATURE_NOT_PRESENT) {
        // Source 2 asks for a superset of features and copes with what it
        // gets; a feature MoltenVK lacks is turned off and the create retried.
        unsigned off = klvk_clamp_features(phys, &mine);
        VKI("vkCreateDevice: VK_ERROR_FEATURE_NOT_PRESENT — %u feature(s) turned off, retrying\\n", off);
        r = real(phys, &mine, alloc, out);
    }
    free(have);
    if (r != VK_SUCCESS) { VKI("vkCreateDevice FAILED %d\\n", r); return r; }""", 1)
if _t != _orig:
    open(_p, "w").write(_t); print("patched runtime/gfx/kl_vulkan.c (Linux WSI / feature clamp)")
for _needle in ("VK_KHR_xcb_surface", "klvk_CreateLinuxSurfaceKHR", "vkCreateXcbSurfaceKHR", "klvk_clamp_features"):
    if _needle not in _t: raise SystemExit("kl_vulkan.c: patch incomplete: " + _needle)

# ---- interposers also answer guest dlsym (the engine takes SDL and Vulkan entry
# points by dlsym, not by import), and there is room for more than eight.
patch("runtime/kl_image.c", [
    ("static struct { const char *name; void *fn; } g_interpose[8];",
     "", "after", "g_interpose[64]"),
])
_p = os.path.join(ROOT, "runtime/kl_image.c"); _t = open(_p).read(); _orig = _t
_t = _t.replace("static struct { const char *name; void *fn; } g_interpose[8];", "static struct { const char *name; void *fn; } g_interpose[64];")
_t = _t.replace("    if (!name || !fn || g_interpose_n >= 8) return;", "    if (!name || !fn || g_interpose_n >= 64) return;")
if "void *kl_interpose_lookup(const char *name)" not in _t:
    _t = _t.replace("static uint64_t sym_value(kl_image *img, uint32_t idx, const char **name_out) {",
"""void *kl_interpose_lookup(const char *name) {
    for (unsigned i = 0; i < g_interpose_n; i++)
        if (!strcmp(name, g_interpose[i].name)) return g_interpose[i].fn;
    return NULL;
}

static uint64_t sym_value(kl_image *img, uint32_t idx, const char **name_out) {""", 1)
if _t != _orig: open(_p, "w").write(_t); print("patched runtime/kl_image.c (interpose table)")
patch("runtime/klepton.h", [
    ("void       kl_interpose(const char *name, void *fn);",
     "\nvoid      *kl_interpose_lookup(const char *name);   // an interposer by name, or NULL", "after", "kl_interpose_lookup"),
])
patch("runtime/kl_dl.c", [
    ("    void *v = kl_sym((kl_image *)handle, name);\n    if (!v) { snprintf(g_dlerr, sizeof g_dlerr, \"klepton: undefined symbol: %s\", name);\n              return klb_dlsym_null(name); }\n",
     "    { void *w = kl_interpose_lookup(name); if (w) return w; }\n", "after", "kl_interpose_lookup(name); if (w) return w;"),
])

# ---- dependency cycles (glibc guests: libfreetype <-> libharfbuzz). The loader
# recursed forever; now a library that is already being loaded is skipped, and
# once the whole tree is in, imports that missed are bound again.
_p = os.path.join(ROOT, "runtime/kl_image.c"); _t = open(_p).read(); _orig = _t
if "int kl_rebind_missing(kl_image *img)" not in _t:
    _t = _t.replace("void kl_unload(kl_image *img) {", r"""// Bind again the imports that were unresolved when this image was relocated:
// a dependency cycle loads one side before the other exists. Only the names in
// the missing list are touched; GOT pages that RELRO made read-only are opened.
int kl_rebind_missing(kl_image *img) {
    if (!img || !img->missing_n || !img->symtab || !img->strtab) return 0;
    unsigned phnum = 0;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)kl_phdrs(img, &phnum);
    if (!ph) return 0;
    uint64_t lo = UINT64_MAX;
    for (unsigned i = 0; i < phnum; i++) if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
    if (lo == UINT64_MAX) lo = 0;
    const Elf64_Rela *tabs[2] = { NULL, NULL }; size_t cnts[2] = { 0, 0 };
    for (unsigned i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_DYNAMIC) continue;
        for (const Elf64_Dyn *dp = (const Elf64_Dyn *)(img->base + ph[i].p_vaddr - lo); dp->d_tag != DT_NULL; dp++) {
            if (dp->d_tag == DT_RELA) tabs[0] = (const Elf64_Rela *)(img->base + dp->d_val);
            else if (dp->d_tag == DT_RELASZ) cnts[0] = dp->d_val / sizeof(Elf64_Rela);
            else if (dp->d_tag == DT_JMPREL) tabs[1] = (const Elf64_Rela *)(img->base + dp->d_val);
            else if (dp->d_tag == DT_PLTRELSZ) cnts[1] = dp->d_val / sizeof(Elf64_Rela);
        }
    }
    int fixed = 0;
    const uintptr_t pg = (uintptr_t)getpagesize();
    for (int t = 0; t < 2; t++) {
        for (size_t k = 0; tabs[t] && k < cnts[t]; k++) {
            const Elf64_Rela *r = &tabs[t][k];
            uint32_t type = ELF64_R_TYPE(r->r_info), sidx = ELF64_R_SYM(r->r_info);
            if (!sidx || (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT)) continue;
            const Elf64_Sym *s = &img->symtab[sidx];
            if (s->st_shndx != 0) continue;
            const char *nm = img->strtab + s->st_name;
            unsigned m;
            for (m = 0; m < img->missing_n; m++) if (!strcmp(img->missing[m], nm)) break;
            if (m == img->missing_n) continue;
            void *v = kl_shim_lookup(nm);
            if (!v) v = kl_guest_sym_global(nm);
            if (!v) continue;
            uint64_t *slot = (uint64_t *)(img->base + r->r_offset);
            mprotect((void *)((uintptr_t)slot & ~(pg - 1)), pg, PROT_READ | PROT_WRITE);
            *slot = (uint64_t)(uintptr_t)v + (uint64_t)r->r_addend;
            fixed++;
        }
    }
    if (fixed) {
        // Drop the names that now resolve.
        unsigned w = 0;
        for (unsigned m = 0; m < img->missing_n; m++) {
            const char *nm = img->missing[m];
            if (kl_shim_lookup(nm) || kl_guest_sym_global(nm)) continue;
            img->missing[w++] = nm;
        }
        img->missing_n = w;
        img->stats.imports_bound += (unsigned)fixed;
        if (kl_env_on("KL_TRACE_NEEDED", 1))
            fprintf(stderr, "  [dl] %s: %d import(s) bound late (dependency cycle)\n", img->path, fixed);
    }
    return fixed;
}

void kl_unload(kl_image *img) {""", 1)
if _t != _orig: open(_p, "w").write(_t); print("patched runtime/kl_image.c (late binding)")
patch("runtime/klepton.h", [
    ("void       kl_unload(kl_image *img);",
     "\nint        kl_rebind_missing(kl_image *img);   // bind imports that missed (dependency cycles)", "after", "kl_rebind_missing"),
])
_p = os.path.join(ROOT, "runtime/kl_dl.c"); _t = open(_p).read(); _orig = _t
if "g_loading_names" not in _t:
    _t = _t.replace("""kl_image *kl_load_recursive(const char *path) {
    kl_image *have = kl_find_image(path);
    if (have) return have;                       // cycle guard / already loaded
""", """// Libraries whose DT_NEEDED chain is being walked right now. A dependency that
// is already on this list is skipped (its symbols bind late, see
// kl_rebind_missing), and when the outermost load finishes every image's
// missing imports are tried again.
static char g_loading_names[64][128];
static int g_loading_n;
static int g_loading_sentinel_storage;
static int loading_has(const char *b) { for (int i = 0; i < g_loading_n; i++) if (!strcmp(g_loading_names[i], b)) return 1; return 0; }

kl_image *kl_load_recursive(const char *path) {
    kl_image *have = kl_find_image(path);
    if (have) return have;                       // already loaded
    const char *bname = strrchr(path, '/'); bname = bname ? bname + 1 : path;
    if (loading_has(bname)) {
        if (kl_env_on("KL_TRACE_NEEDED", 1)) fprintf(stderr, "  [dl] %s is already being loaded (dependency cycle) — bound late\\n", bname);
        return (kl_image *)&g_loading_sentinel_storage;      // non-NULL: the dependent carries on
    }
    if (g_loading_n < 64) snprintf(g_loading_names[g_loading_n++], 128, "%s", bname);
    kl_image *result = kl_load_recursive_inner(path);
    if (g_loading_n > 0) g_loading_n--;
    if (g_loading_n == 0) {
        pthread_mutex_lock(&g_lock);
        int n = g_nimgs;
        pthread_mutex_unlock(&g_lock);
        for (int i = 0; i < n; i++) kl_rebind_missing(g_imgs[i].img);
    }
    return result;
}

static kl_image *kl_load_recursive_inner(const char *path) {
""", 1)
    _t = _t.replace("""kl_image *kl_load_recursive(const char *path);""", """kl_image *kl_load_recursive(const char *path);
static kl_image *kl_load_recursive_inner(const char *path);""", 1)
    if "static kl_image *kl_load_recursive_inner(const char *path);" not in _t:
        _t = _t.replace("static char g_loading_names[64][128];", "static kl_image *kl_load_recursive_inner(const char *path);\nstatic char g_loading_names[64][128];", 1)
if _t != _orig: open(_p, "w").write(_t); print("patched runtime/kl_dl.c (dependency cycles)")

# ---- kl_x18.c: x18 is an ordinary register for a glibc guest, so shapes an
# Android guest never produces show up: adr x18, blr x18, and push/pop pairs
# naming x18 (stp ... [sp, #-16]! / ldp ... [sp], #16). Each gets its own veneer.
_p = os.path.join(ROOT, "runtime/kl_x18.h"); _t = open(_p).read(); _orig = _t
_t = re.sub(r"#define KLX_VEN_MAX_INSN\s+(\d+)", lambda m: "#define KLX_VEN_MAX_INSN 12" if int(m.group(1)) < 12 else m.group(0), _t)
if _t != _orig: open(_p, "w").write(_t); print("patched runtime/kl_x18.h (veneer length)")
_p = os.path.join(ROOT, "runtime/kl_x18.c"); _t = open(_p).read(); _orig = _t
if "glibc guest shapes" not in _t:
    _t = _t.replace("""        if (in.hazard || in.pcrel == KLX_PC_ADR || in.pcrel == KLX_PC_LITERAL ||
            out + VEN_MAX_INSN > pool_end) { note_refusal(word); st->refused++; continue; }""",
r"""        // ---- glibc guest shapes (x18 is a general register on Linux) ----
        if (out + VEN_MAX_INSN <= pool_end) {
            uint32_t body[VEN_MAX_INSN], patch;
            unsigned k = 0;
            int ok = 1, served = 0;
            if (word == 0xD63F0240u) {
                // blr x18: call through the shadow via x16 (IP0, which every
                // call may clobber), never through the real x18: the kernel can
                // zero x18 between loading it and the blr, and the race
                // recovery then resumed the callee with the wrong return
                // address (libmpg123's dct36 calls: corrupted state, crash).
                // Return lands in the veneer, which branches back behind the site.
                body[k++] = enc_mrs_tpidrro(16);
                body[k++] = enc_ldr(16, 16, (unsigned)KLX_TSD_SLOT * 8);
                body[k++] = 0xD63F0200u;                 // blr x16
                ok &= enc_b(vpc + k * 4, spc + 4, &body[k]); k++;
                served = 1;
            } else if (in.pcrel == KLX_PC_ADR && (word & 0x9F000000u) == 0x10000000u && (word & 31u) == 18) {
                // adr x18, label: the address is a constant, materialised into the shadow.
                int64_t imm = (int64_t)(((word >> 5) & 0x7FFFFu) << 2 | ((word >> 29) & 3u));
                if (imm & (1LL << 20)) imm -= (1LL << 21);
                uint64_t target = spc + (uint64_t)imm;
                unsigned a, b;
                pick_scratch(in.gp_used, &a, &b);
                body[k++] = enc_stp(a, b, -16);
                body[k++] = enc_mrs_tpidrro(a);
                k += enc_mov_imm64(b, target, body + k);
                body[k++] = enc_str(b, a, (unsigned)KLX_TSD_SLOT * 8);
                body[k++] = enc_ldp(a, b, -16);
                ok &= enc_b(vpc + k * 4, spc + 4, &body[k]); k++;
                served = 1;
            } else if (in.hazard == KLX_HZ_SPWRITE && (word & 0xFFC00000u) == 0xA9800000u && ((word >> 5) & 31u) == 31u) {
                // stp Xt, Xt2, [sp, #imm]!  — scratch spilled below the pair's
                // destination, the pair written (x18 read from the shadow) with
                // the original writeback, scratch restored from the new sp.
                int imm = (int)((word >> 15) & 0x7Fu); if (imm & 0x40) imm -= 0x80; imm *= 8;
                if (imm - 16 >= -512 && imm <= 0) {
                    unsigned a, b;
                    pick_scratch(in.gp_used, &a, &b);
                    body[k++] = enc_stp(a, b, imm - 16);
                    body[k++] = enc_mrs_tpidrro(a);
                    body[k++] = enc_ldr(b, a, (unsigned)KLX_TSD_SLOT * 8);
                    body[k++] = klx_substitute(word, &in, b);
                    body[k++] = enc_ldp(a, b, -16);
                    ok &= enc_b(vpc + k * 4, spc + 4, &body[k]); k++;
                    served = 1;
                }
            } else if (in.hazard == KLX_HZ_SPWRITE && (word & 0xFFC00000u) == 0xA8C00000u && ((word >> 5) & 31u) == 31u) {
                // ldp Xt, Xt2, [sp], #imm — the pair is loaded (x18's value into
                // scratch b) with the original writeback, b goes to the shadow,
                // scratch is restored from where it was spilled (old sp - 16).
                int imm = (int)((word >> 15) & 0x7Fu); if (imm & 0x40) imm -= 0x80; imm *= 8;
                if (imm >= 0 && -16 - imm >= -512) {
                    unsigned a, b;
                    pick_scratch(in.gp_used, &a, &b);
                    body[k++] = enc_stp(a, b, -16);
                    body[k++] = klx_substitute(word, &in, b);
                    body[k++] = enc_mrs_tpidrro(a);
                    body[k++] = enc_str(b, a, (unsigned)KLX_TSD_SLOT * 8);
                    body[k++] = enc_ldp(a, b, -16 - imm);
                    ok &= enc_b(vpc + k * 4, spc + 4, &body[k]); k++;
                    served = 1;
                }
            }
            if (served) {
                ok &= (k <= VEN_MAX_INSN) && enc_b(spc, vpc, &patch);
                if (!ok) {
                    note_refusal(word); st->refused++;
                    if (out_of_reach(spc, vpc)) st->far_refused++;
                    continue;
                }
                memcpy(out, body, k * 4);
                out += k;
                w[i] = patch;
                st->patched++;
                continue;
            }
        }

        if (in.hazard || in.pcrel == KLX_PC_ADR || in.pcrel == KLX_PC_LITERAL ||
            out + VEN_MAX_INSN > pool_end) { note_refusal(word); st->refused++; continue; }""", 1)
if _t != _orig: open(_p, "w").write(_t); print("patched runtime/kl_x18.c (glibc x18 shapes)")
if "glibc guest shapes" not in _t: raise SystemExit("kl_x18.c: anchor for the glibc x18 shapes not found")

# ---- kl_dl.c: one image per FILE. A glibc guest reaches the same library under
# two names (DT_NEEDED "libpango-1.0.so.0", dlopen "libpango-1.0.so"); loading it
# twice registers every GObject type twice. The registry remembers the real path.
_p = os.path.join(ROOT, "runtime/kl_dl.c"); _t = open(_p).read(); _orig = _t
if "char real[1024];" not in _t:
    _t = re.sub(r"(typedef struct \{[^}]*?)(\} entry;)", lambda m: m.group(1) + "    char real[1024];       // realpath of the file, so two names for one file are one image\n" + m.group(2), _t, count=1, flags=re.S)
    _t = _t.replace("""        snprintf(g_imgs[g_nimgs].soname, sizeof g_imgs[0].soname, "%s", base);
        g_imgs[g_nimgs].img = img;""", """        snprintf(g_imgs[g_nimgs].soname, sizeof g_imgs[0].soname, "%s", base);
        if (!realpath(soname, g_imgs[g_nimgs].real)) snprintf(g_imgs[g_nimgs].real, sizeof g_imgs[0].real, "%s", soname);
        g_imgs[g_nimgs].img = img;""", 1)
    _t = _t.replace("""kl_image *kl_find_image(const char *soname) {
    const char *b = basename_of(soname);
    for (int i = 0; i < g_nimgs; i++)
        if (strcmp(g_imgs[i].soname, b) == 0) return g_imgs[i].img;
    return NULL;
}""", """kl_image *kl_find_image(const char *soname) {
    const char *b = basename_of(soname);
    char rp[1024];
    const char *real = strchr(soname, '/') && realpath(soname, rp) ? rp : NULL;
    for (int i = 0; i < g_nimgs; i++)
        if (strcmp(g_imgs[i].soname, b) == 0 || (real && strcmp(g_imgs[i].real, real) == 0)) return g_imgs[i].img;
    return NULL;
}""", 1)
if _t != _orig: open(_p, "w").write(_t); print("patched runtime/kl_dl.c (one image per file)")
if "char real[1024];" not in _t: raise SystemExit("kl_dl.c: entry struct anchor not found")

# ---- kl_image.c: KL_TRACE_INIT=1 names each constructor before it runs.
_p = os.path.join(ROOT, "runtime/kl_image.c"); _t = open(_p).read(); _orig = _t
if "KL_TRACE_INIT" not in _t:
    _t = _t.replace("""void kl_run_init(kl_image *img) {
    for (size_t i = 0; i < img->init_count; i++)
        if (img->init_array[i]) img->init_array[i]();
}""", """void kl_run_init(kl_image *img) {
    int trace = kl_env_on("KL_TRACE_INIT", 0);
    for (size_t i = 0; i < img->init_count; i++) {
        if (!img->init_array[i]) continue;
        if (trace) fprintf(stderr, "  [init] %s [%zu/%zu] -> +%#llx\\n", img->path, i, img->init_count,
                           (unsigned long long)((const uint8_t *)img->init_array[i] - img->base));
        // KL_INIT_SLEEP=<basename>:<index>:<ms> pauses before that constructor; KL_INIT_SKIP=<basename>:<index> skips it.
        { const char *sl = getenv("KL_INIT_SLEEP"); const char *sk = getenv("KL_INIT_SKIP");
          const char *b = strrchr(img->path, '/'); b = b ? b + 1 : img->path;
          if (sl) { const char *c1 = strchr(sl, ':'); const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
                    if (c1 && c2 && (size_t)(c1 - sl) == strlen(b) && !strncmp(sl, b, strlen(b)) && (size_t)atoi(c1 + 1) == i) usleep((useconds_t)atoi(c2 + 1) * 1000); }
          if (sk) { const char *c1 = strrchr(sk, ':');
                    if (c1 && (size_t)(c1 - sk) == strlen(b) && !strncmp(sk, b, strlen(b)) && (size_t)atoi(c1 + 1) == i) { fprintf(stderr, "  [init] skipping %s [%zu]\\n", b, i); continue; } } }
        // KL_INIT_BREAK=<basename>:<index> traps into the debugger just before that constructor.
        { const char *brk = getenv("KL_INIT_BREAK");
          if (brk) { const char *colon = strrchr(brk, ':'); const char *b = strrchr(img->path, '/'); b = b ? b + 1 : img->path;
                     if (colon && (size_t)(colon - brk) == strlen(b) && !strncmp(brk, b, strlen(b)) && (size_t)atoi(colon + 1) == i) __builtin_debugtrap(); } }
        img->init_array[i]();
    }
}""", 1)
if _t != _orig: open(_p, "w").write(_t); print("patched runtime/kl_image.c (init trace)")

# ---- kl_dl.c: TLS symbol lookup across images; glibc sonames are shim-served
patch("runtime/kl_dl.c", [
    ("// Load an .so together with its DT_NEEDED chain, dependencies first",
     """int kl_guest_tls_sym_global(const char *name, kl_tls_module **m, uint64_t *off) {
    int rc = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nimgs && rc != 0; i++)
        if (kl_sym_tls(g_imgs[i].img, name, off) == 0) { *m = kl_image_tls_module(g_imgs[i].img); rc = 0; }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

""", "before", "int kl_guest_tls_sym_global"),
    ('        "libopenxr_loader.so", "libOpenMAXAL.so",\n',
     """        // glibc guests: the C runtime is served by the host shim, never loaded
        // as a guest image; windowing and device libraries are stubbed by name.
        "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
        "libutil.so.1", "libresolv.so.2", "ld-linux-aarch64.so.1",
        "libX11.so.6", "libXext.so.6", "libxcb.so.1", "libwayland-client.so.0",
        "libudev.so.1", "libdrm.so.2", "libmimalloc.so",
""", "after", '"libc.so.6", "libm.so.6", "libdl.so.2"'),
])

# ---- kl_shim.c: every guest thread initialises its fixed TLS slots
patch("runtime/libc/kl_shim.c", [
    ("    ((uint64_t *)tp)[TLS_SLOT_STACK_GUARD] = g_canary;\n",
     "    kl_tls_thread_init();\n", "after", "kl_tls_thread_init();"),
])
# ---- kl_image.c: bare `svc #0` in a glibc guest becomes a nop
# On Darwin `svc` takes its syscall number from x16, not x8, so a Linux inline
# syscall runs whatever Darwin call x16 happens to name — exit(1) when x16 is 1,
# which is how libvideo.so's 1703 debugger-break `svc #0` sites (none sets x8)
# ended the process with status 1 and no trace. A site with no write to x8 in
# the six words before it is not a syscall the guest means; it becomes a nop.
# A site that does set x8 is left alone and counted, as before.
patch("runtime/kl_image.c", [
    ("static void rewrite_tls(uint8_t *p, size_t n, uint64_t va, const char *path,\n",
     """// A `svc #0` with no x8 write in the six words before it (any instruction
// whose destination register is x8/w8).
static int kl_svc_is_bare(const uint32_t *w, size_t i) {
    for (size_t k = 1; k <= 6 && k <= i; k++)
        if ((w[i - k] & 0x1fu) == 8u) return 0;
    return 1;
}

""", "before", "kl_svc_is_bare(const uint32_t"),
    ("        if ((w[i] & 0xffffffe0u) == 0xd53bd040u) {\n",
     """        if (w[i] == 0xd4000001u && kl_svc_is_bare(w, i)) {
            static int said;
            if (!said++) fprintf(stderr, "  [klepton] %s: bare svc #0 (no syscall number) at +0x%llx and "
                                         "others -> nop (Darwin would run syscall x16)\\n",
                                 path, (unsigned long long)(va + i * 4));
            w[i] = 0xd503201fu;
            st->svc_sites++;
            continue;
        }
""", "before", "kl_svc_is_bare(w, i)"),
])
# ---- kl_dl.c: room for a Source 2 game's images
# Klepton's image table held 64. Half-Life: Alyx loads ~110 (game, hlvr, the
# Steam Runtime and Debian libraries); every image past the 64th was invisible
# to dlsym, dladdr, dl_iterate_phdr and the one-image-per-file dedupe — the
# engine's dlsym(libclient, "CreateInterface") then fell through to the global
# search and got libvstdlib's, and "Unable to create interface
# Source2ClientConfig001" ended the run. A full table now says so instead of
# dropping the image silently.
_p = os.path.join(ROOT, "runtime/kl_dl.c"); _t = open(_p).read()
if "#define KL_MAX_IMAGES 64\n" in _t:
    open(_p, "w").write(_t.replace("#define KL_MAX_IMAGES 64\n",
        "#define KL_MAX_IMAGES 1024   // VeRantyx: was 64; Alyx loads ~110 images\n"))
patch("runtime/kl_dl.c", [
    ("    if (g_nimgs < KL_MAX_IMAGES) {\n        snprintf(g_imgs[g_nimgs].soname",
     "    if (g_nimgs >= KL_MAX_IMAGES)\n        fprintf(stderr, \"  [klepton] image table full (%d): %s is not visible to dlsym\\n\", KL_MAX_IMAGES, soname);\n",
     "before", "image table full"),
])
# ---- image-loaded hook: lets the host repair a guest image right after load
patch("runtime/klepton.h", [
    ("void       kl_interpose(const char *name, void *fn);\n",
     "// Called once per image right after it is registered (before guest code\n"
     "// of a later dlopen can run it); the host uses it for byte-checked fixes.\n"
     "extern void (*kl_image_loaded_hook)(const char *path, kl_image *img);\n",
     "after", "kl_image_loaded_hook"),
])
patch("runtime/kl_dl.c", [
    ("void kl_register_image(const char *soname, kl_image *img) {\n",
     "void (*kl_image_loaded_hook)(const char *path, kl_image *img);\n\n",
     "before", "void (*kl_image_loaded_hook)"),
    ("    if (!strcmp(base, \"libgl4es.so\")) kl_shim_set_guest_gl(img);\n",
     "    if (kl_image_loaded_hook) kl_image_loaded_hook(soname, img);\n",
     "after", "if (kl_image_loaded_hook) kl_image_loaded_hook"),
])
# ---- kl_libc.c: CPU topology in the synthetic /sys
# Source 2's tier0 counts physical cores from cpuN/topology/{core_id,
# physical_package_id} (and reads cpu_capacity to find big cores, as on the
# Steam Frame's Snapdragon). Klepton served neither topology node, so Alyx
# found no cores and ran with "Thread pool has 0 threads": every job on one
# thread, hitches whenever characters stream in. Now each online core has a
# core_id, package 0 and sibling lists, and cpu_capacity marks Apple
# silicon's efficiency cores (hw.perflevel1) as little cores.
patch("runtime/libc/kl_libc.c", [
    ("    // Some guests size their core loop from kernel_max (highest possible CPU\n",
     """    {
        int perf = 0; size_t sz = sizeof perf;
        if (sysctlbyname("hw.perflevel0.logicalcpu", &perf, &sz, NULL, 0) != 0 || perf <= 0 || perf > ncpu) perf = (int)ncpu;
        char sib[64];
        if (ncpu > 1) snprintf(sib, sizeof sib, "0-%ld\\n", ncpu - 1); else snprintf(sib, sizeof sib, "0\\n");
        for (long i = 0; i < ncpu; i++) {
            char rel[256], val[32];
            snprintf(rel, sizeof rel, "/sys/devices/system/cpu/cpu%ld/topology/core_id", i);
            snprintf(val, sizeof val, "%ld\\n", i); proc_put(rel, val);
            snprintf(rel, sizeof rel, "/sys/devices/system/cpu/cpu%ld/topology/physical_package_id", i);
            proc_put(rel, "0\\n");
            snprintf(rel, sizeof rel, "/sys/devices/system/cpu/cpu%ld/topology/thread_siblings_list", i);
            proc_put(rel, val);
            snprintf(rel, sizeof rel, "/sys/devices/system/cpu/cpu%ld/topology/core_siblings_list", i);
            proc_put(rel, sib);
            snprintf(rel, sizeof rel, "/sys/devices/system/cpu/cpu%ld/topology/package_cpus_list", i);
            proc_put(rel, sib);
            snprintf(rel, sizeof rel, "/sys/devices/system/cpu/cpu%ld/cpu_capacity", i);
            proc_put(rel, i < perf ? "1024\\n" : "400\\n");          // performance cores first
        }
    }
""", "before", "topology/core_id"),
])
print("done")
