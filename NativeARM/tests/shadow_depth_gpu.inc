// Included by pipeline_cache_gpu.c. A depth-only pass with fragment shader and
// ignored colour-blend state must write its first draw into the depth image.
static void test_shadow_depth(VkPhysicalDevice physical, uint32_t family, const VkGraphicsPipelineCreateInfo *base) {
    DEVICE_FN(CreateShaderModule); DEVICE_FN(DestroyShaderModule); DEVICE_FN(CreateRenderPass); DEVICE_FN(DestroyRenderPass);
    DEVICE_FN(CreateImage); DEVICE_FN(DestroyImage); DEVICE_FN(GetImageMemoryRequirements); DEVICE_FN(BindImageMemory);
    DEVICE_FN(AllocateMemory); DEVICE_FN(FreeMemory); DEVICE_FN(CreateImageView); DEVICE_FN(DestroyImageView);
    DEVICE_FN(CreateFramebuffer); DEVICE_FN(DestroyFramebuffer); DEVICE_FN(CreateBuffer); DEVICE_FN(DestroyBuffer);
    DEVICE_FN(GetBufferMemoryRequirements); DEVICE_FN(BindBufferMemory); DEVICE_FN(MapMemory); DEVICE_FN(UnmapMemory);
    DEVICE_FN(CreateCommandPool); DEVICE_FN(DestroyCommandPool); DEVICE_FN(AllocateCommandBuffers);
    DEVICE_FN(BeginCommandBuffer); DEVICE_FN(EndCommandBuffer); DEVICE_FN(CmdBeginRenderPass); DEVICE_FN(CmdEndRenderPass);
    DEVICE_FN(CmdBindPipeline); DEVICE_FN(CmdDraw); DEVICE_FN(CmdPipelineBarrier); DEVICE_FN(CmdCopyImageToBuffer);
    DEVICE_FN(GetDeviceQueue); DEVICE_FN(QueueSubmit); DEVICE_FN(CreateFence); DEVICE_FN(DestroyFence); DEVICE_FN(WaitForFences);
    DEVICE_FN(CreateGraphicsPipelines);
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties = (PFN_vkGetPhysicalDeviceMemoryProperties)dlsym(g_gpu_library, "vkGetPhysicalDeviceMemoryProperties");
    assert(memory_properties);
    VkPhysicalDeviceMemoryProperties memory; memory_properties(physical, &memory);
    // SPIR-V: a fragment entry point that returns without a colour output.
    const uint32_t fragment[] = { 0x07230203, 0x00010000, 0, 5, 0,
        0x00020011, 1, 0x0003000e, 0, 1, 0x0005000f, 4, 3, 0x6e69616d, 0,
        0x00030010, 3, 7, 0x00020013, 1, 0x00030021, 2, 1,
        0x00050036, 1, 3, 0, 2, 0x000200f8, 4, 0x000100fd, 0x00010038 };
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main" }
    };
    VkShaderModuleCreateInfo sm = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof spv_fullscreen_vert, .pCode = spv_fullscreen_vert };
    assert(pCreateShaderModule(device.dev, &sm, NULL, &stages[0].module) == VK_SUCCESS);
    sm.codeSize = sizeof fragment; sm.pCode = fragment;
    assert(pCreateShaderModule(device.dev, &sm, NULL, &stages[1].module) == VK_SUCCESS);
    VkAttachmentDescription attachment = { .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference depth = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .pDepthStencilAttachment = &depth };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass };
    VkRenderPass pass; assert(pCreateRenderPass(device.dev, &rpci, NULL, &pass) == VK_SUCCESS);
    kva_record_pass(pass, &rpci, NULL);
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_ALWAYS, .maxDepthBounds = 1 };
    VkGraphicsPipelineCreateInfo ci = *base;
    ci.pStages = stages; ci.renderPass = pass; ci.pDepthStencilState = &ds;
    // base has a colour-write mask of 15, which is ignored for this pass.
    assert(kva_depth_only(&ci));
    VkPipelineCacheCreateInfo cci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    VkPipelineCache cache; assert(klvk_CreatePipelineCache(device.dev, &cci, NULL, &cache) == VK_SUCCESS);
    VkPipeline pipeline; assert(kva_create(device.dev, cache, 1, &ci, &pipeline, pCreateGraphicsPipelines));
    // This test verifies GPU depth after the background material is available.
    // The nonblocking recording path is covered by the stalled-compiler test.
    pthread_mutex_lock(&g_kva_mu);
    while (g_kva_pending || g_kva_finishing) pthread_cond_wait(&g_kva_ready, &g_kva_mu);
    pthread_mutex_unlock(&g_kva_mu);
    kl_vulkan_async_begin_frame();
    VkPipeline real = (VkPipeline)kva_bind(&ci, VK_PIPELINE_BIND_POINT_GRAPHICS, (uint64_t)pipeline); assert(real);

    VkImageCreateInfo image_ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT, .extent = {4,4,1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    VkImage image; assert(pCreateImage(device.dev, &image_ci, NULL, &image) == VK_SUCCESS);
    VkMemoryRequirements requirement; pGetImageMemoryRequirements(device.dev, image, &requirement);
    uint32_t type = 0; while (type < memory.memoryTypeCount && !(requirement.memoryTypeBits & (1u << type))) type++;
    assert(type < memory.memoryTypeCount);
    VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirement.size, .memoryTypeIndex = type };
    VkDeviceMemory image_memory; assert(pAllocateMemory(device.dev, &allocation, NULL, &image_memory) == VK_SUCCESS);
    assert(pBindImageMemory(device.dev, image, image_memory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo view_ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT, .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
    VkImageView view; assert(pCreateImageView(device.dev, &view_ci, NULL, &view) == VK_SUCCESS);
    VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
        .attachmentCount = 1, .pAttachments = &view, .width = 4, .height = 4, .layers = 1 };
    VkFramebuffer framebuffer; assert(pCreateFramebuffer(device.dev, &fbci, NULL, &framebuffer) == VK_SUCCESS);
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buffer; assert(pCreateBuffer(device.dev, &bci, NULL, &buffer) == VK_SUCCESS);
    pGetBufferMemoryRequirements(device.dev, buffer, &requirement);
    type = 0;
    while (type < memory.memoryTypeCount && (!(requirement.memoryTypeBits & (1u << type)) ||
        (memory.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) type++;
    assert(type < memory.memoryTypeCount);
    allocation.allocationSize = requirement.size; allocation.memoryTypeIndex = type;
    VkDeviceMemory buffer_memory; assert(pAllocateMemory(device.dev, &allocation, NULL, &buffer_memory) == VK_SUCCESS);
    assert(pBindBufferMemory(device.dev, buffer, buffer_memory, 0) == VK_SUCCESS);
    VkCommandPoolCreateInfo pool_ci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family };
    VkCommandPool pool; assert(pCreateCommandPool(device.dev, &pool_ci, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo commands = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer command; assert(pAllocateCommandBuffers(device.dev, &commands, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    assert(pBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    VkClearValue clear = { .depthStencil = { 1, 0 } };
    VkRenderPassBeginInfo render = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
        .framebuffer = framebuffer, .renderArea = { {0,0}, {4,4} }, .clearValueCount = 1, .pClearValues = &clear };
    pCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
    pCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, real); pCmdDraw(command, 3, 1, 0, 0); pCmdEndRenderPass(command);
    VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
    pCmdPipelineBarrier(command, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageExtent = {4,4,1} };
    pCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    VkMemoryBarrier host = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    pCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    assert(pEndCommandBuffer(command) == VK_SUCCESS);
    VkQueue queue; pGetDeviceQueue(device.dev, family, 0, &queue);
    VkFenceCreateInfo fence_ci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; assert(pCreateFence(device.dev, &fence_ci, NULL, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command };
    assert(pQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    assert(pWaitForFences(device.dev, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS);
    float *pixels; assert(pMapMemory(device.dev, buffer_memory, 0, 64, 0, (void **)&pixels) == VK_SUCCESS);
    for (int i = 0; i < 16; i++) assert(pixels[i] == 0.0f);
    pUnmapMemory(device.dev, buffer_memory);
    puts("shadow depth: ignored blend state, ready-frame draw retained, 16 GPU depth pixels verified");
    pDestroyFence(device.dev, fence, NULL); pDestroyCommandPool(device.dev, pool, NULL);
    pDestroyFramebuffer(device.dev, framebuffer, NULL); pDestroyImageView(device.dev, view, NULL);
    pDestroyImage(device.dev, image, NULL); pFreeMemory(device.dev, image_memory, NULL);
    pDestroyBuffer(device.dev, buffer, NULL); pFreeMemory(device.dev, buffer_memory, NULL);
    assert(kva_destroy_pipeline(device.dev, (uint64_t)pipeline, (PFN_vkDestroyPipeline)real_gdpa(device.dev, "vkDestroyPipeline")));
    klvk_DestroyPipelineCache(device.dev, (uint64_t)cache, NULL);
    kva_forget_pass(pass); pDestroyRenderPass(device.dev, pass, NULL);
    for (int i = 0; i < 2; i++) pDestroyShaderModule(device.dev, stages[i].module, NULL);
}
