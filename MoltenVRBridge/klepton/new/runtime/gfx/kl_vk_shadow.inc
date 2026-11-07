// Bounded render-attachment diagnostics for the native shadow investigation.
static pthread_once_t g_klvs_once = PTHREAD_ONCE_INIT;
static int g_klvs_on;
static double g_klvs_start, g_klvs_after;
static _Atomic unsigned g_klvs_lines;
static void klvs_init(void) {
    g_klvs_on = kl_env_on("KL_VK_SHADOW_TRACE",0);
    g_klvs_start = kl_pipe_ms();
    const char *after = getenv("KL_VK_SHADOW_TRACE_AFTER");
    g_klvs_after = after ? atof(after)*1000.0 : 0;
}
static void klvs_trace_rendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    pthread_once(&g_klvs_once,klvs_init);
    if (!g_klvs_on || !ri || !ri->pDepthAttachment || !ri->pDepthAttachment->imageView) return;
    double elapsed = kl_pipe_ms()-g_klvs_start;
    if (elapsed<g_klvs_after || atomic_fetch_add(&g_klvs_lines,1)>=500) return;
    const VkRenderingAttachmentInfo *d=ri->pDepthAttachment;
    VKI("[shadow] t=%.3f cb=%p depthView=%#llx image=%#llx layout=%d load=%d store=%d clear=%.8g area=%d,%d+%ux%u layers=%u views=%#x colours=%u flags=%#x\n",
        elapsed/1000.0,(void *)cb,(unsigned long long)(uintptr_t)d->imageView,
        (unsigned long long)(uintptr_t)klvk_view_image(d->imageView),(int)d->imageLayout,
        (int)d->loadOp,(int)d->storeOp,d->clearValue.depthStencil.depth,
        ri->renderArea.offset.x,ri->renderArea.offset.y,ri->renderArea.extent.width,ri->renderArea.extent.height,
        ri->layerCount,ri->viewMask,ri->colorAttachmentCount,ri->flags);
}
