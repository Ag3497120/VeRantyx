// Source 2's Deckard renderer selects all of its dynamic colour state from
// extension presence. MoltenVK exposes EDS3 with a subset of its optional
// features; in particular Metal cannot change the per-channel write mask.
// Removing a requested feature only at CreateDevice is too late: the engine
// can still emit masks that the driver cannot apply to colour/depth-only work.
// Let this engine choose its static pipeline variants before device creation.
static VkResult kvc_enumerate_extensions(PFN_vkEnumerateDeviceExtensionProperties enumerate,
        PFN_vkGetPhysicalDeviceFeatures2 features, VkPhysicalDevice pd, const char *layer,
        uint32_t *count, VkExtensionProperties *out) {
    if (!enumerate || !count) return VK_ERROR_INITIALIZATION_FAILED;
    if (!kl_env_on("KL_VK_ALYX_STATIC_COLOR_WRITE", 0) || layer || !features)
        return enumerate(pd, layer, count, out);
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT
    };
    VkPhysicalDeviceFeatures2 supported = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &dynamic };
    features(pd, &supported);
    if (dynamic.extendedDynamicState3ColorWriteMask) return enumerate(pd, layer, count, out);
    uint32_t n = 0;
    VkResult result = enumerate(pd, layer, &n, NULL);
    if (result != VK_SUCCESS) return result;
    VkExtensionProperties *all = n ? calloc(n, sizeof *all) : NULL;
    if (n && !all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = enumerate(pd, layer, &n, all);
    if (result != VK_SUCCESS) { free(all); return result; }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(all[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)) continue;
        all[kept++] = all[i];
    }
    if (out) {
        uint32_t copied = *count < kept ? *count : kept;
        if (copied) memcpy(out, all, copied*sizeof *all);
        *count = copied;
        result = copied < kept ? VK_INCOMPLETE : VK_SUCCESS;
    } else *count = kept;
    free(all);
    return result;
}
