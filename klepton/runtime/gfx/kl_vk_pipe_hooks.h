// Pipeline-object hooks the Vulkan layer (kl_vulkan.c) calls after each successful create,
// with the guest's create info, for an add-on pipeline recorder/replayer (the Alyx add-on's
// kl_fossilize.cpp). Include after the Vulkan headers.
//
// device_created hands over the layer's own vkGetDeviceProcAddr, so a replayer creates its
// objects through exactly the path the guest does: the same fix-ups, the same MoltenVK state,
// and so the same MSL, which is what the OS Metal compiler cache is keyed on.
#ifndef KL_VK_PIPE_HOOKS_H
#define KL_VK_PIPE_HOOKS_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct klvk_pipe_hooks {
    void (*device_created)(VkDevice dev, PFN_vkGetDeviceProcAddr layer_gdpa);
    void (*sampler)(VkSampler h, const VkSamplerCreateInfo *ci);
    void (*set_layout)(VkDescriptorSetLayout h, const VkDescriptorSetLayoutCreateInfo *ci);
    void (*pipeline_layout)(VkPipelineLayout h, const VkPipelineLayoutCreateInfo *ci);
    void (*shader_module)(VkShaderModule h, const VkShaderModuleCreateInfo *ci);
    void (*render_pass)(VkRenderPass h, const VkRenderPassCreateInfo *ci);
    void (*render_pass2)(VkRenderPass h, const VkRenderPassCreateInfo2 *ci);
    void (*graphics_pipeline)(VkPipeline h, const VkGraphicsPipelineCreateInfo *ci);
    void (*compute_pipeline)(VkPipeline h, const VkComputePipelineCreateInfo *ci);
    // how long the guest's create of this pipeline took (its batch's time over its size), after graphics_pipeline
    void (*pipeline_time)(VkPipeline h, double ms);
    // the guest's device create info, as it asked, just before the device is made (its features go into the recording)
    void (*device_info)(const VkDeviceCreateInfo *ci);
} klvk_pipe_hooks;

void klvk_set_pipe_hooks(const klvk_pipe_hooks *h);

// A replayer's own device (kl_foz_prewarm), when it is done with it: out of the layer's books and destroyed, with
// its instance, so the game's device is the layer's first.
void klvk_retire_device(VkInstance inst, VkDevice dev);

// On a replayer's own threads: their creates are neither handed to the hooks nor counted
// as guest compiles in the "pipeline warmup" report.
void klvk_set_replay_thread(int on);

#ifdef __cplusplus
}
#endif
#endif
