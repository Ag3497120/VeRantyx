// Per-worker caches contain the pipelines after Klepton's SPIR-V rewrites.
// Each worker owns its cache exclusively; do not share it across compiles.
typedef struct { VkDevice dev; VkPipelineCache cache; PFN_vkGetPipelineCacheData data; char path[4096]; unsigned compiled; } kva_disk_cache;
static _Thread_local kva_disk_cache kva_disk;
static _Atomic unsigned kva_disk_next;
static VkPipelineCache kva_disk_get(VkDevice dev) {
    const char *dir = getenv("KL_VK_ASYNC_CACHE_DIR");
    if (!dir || !*dir || !real_gdpa) return VK_NULL_HANDLE;
    if (kva_disk.cache) return kva_disk.dev == dev ? kva_disk.cache : VK_NULL_HANDLE;
    PFN_vkCreatePipelineCache create = (PFN_vkCreatePipelineCache)real_gdpa(dev,"vkCreatePipelineCache");
    PFN_vkMergePipelineCaches merge = (PFN_vkMergePipelineCaches)real_gdpa(dev,"vkMergePipelineCaches");
    PFN_vkDestroyPipelineCache destroy = (PFN_vkDestroyPipelineCache)real_gdpa(dev,"vkDestroyPipelineCache");
    kva_disk.data = (PFN_vkGetPipelineCacheData)real_gdpa(dev,"vkGetPipelineCacheData");
    if (!create || !merge || !destroy || !kva_disk.data) return VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo ci={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    if (create(dev,&ci,NULL,&kva_disk.cache)!=VK_SUCCESS) return VK_NULL_HANDLE;
    kva_disk.dev=dev; unsigned id=atomic_fetch_add(&kva_disk_next,1);
    snprintf(kva_disk.path,sizeof kva_disk.path,"%s/worker-%u.bin",dir,id);
    size_t loaded=0;
    for(unsigned j=0;j<9;j++) {
        char path[4096];if(j==8)snprintf(path,sizeof path,"%s/prewarm.bin",dir);else snprintf(path,sizeof path,"%s/worker-%u.bin",dir,j);
        FILE *f=fopen(path,"rb");if(!f)continue;
        fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
        if(n<=0||n>128*1024*1024){fclose(f);continue;}
        void *bytes=malloc(n);if(!bytes){fclose(f);continue;}
        size_t got=fread(bytes,1,n,f);fclose(f);
        VkPipelineCache old=VK_NULL_HANDLE;
        ci.initialDataSize=n;ci.pInitialData=bytes;
        if(got==(size_t)n&&create(dev,&ci,NULL,&old)==VK_SUCCESS){
            if(merge(dev,kva_disk.cache,1,&old)==VK_SUCCESS)loaded+=n;
            destroy(dev,old,NULL);
        }
        free(bytes);
    }
    VKI("[async-cache] worker %u loaded %.1f MiB of cached shader data\n",id,loaded/1048576.0);
    return kva_disk.cache;
}
static void kva_disk_save(int force) {
    if(!kva_disk.cache || !kva_disk.data)return;
    kva_disk.compiled++;
    if(!force&&kva_disk.compiled%8)return;
    size_t n=0;if(kva_disk.data(kva_disk.dev,kva_disk.cache,&n,NULL)!=VK_SUCCESS||!n)return;
    void *bytes=malloc(n);if(!bytes)return;
    if(kva_disk.data(kva_disk.dev,kva_disk.cache,&n,bytes)==VK_SUCCESS){
        char tmp[4200];snprintf(tmp,sizeof tmp,"%s.tmp",kva_disk.path);
        FILE *f=fopen(tmp,"wb");if(f){size_t written=fwrite(bytes,1,n,f);int closed=fclose(f);if(written==n&&!closed)rename(tmp,kva_disk.path);}
    }
    free(bytes);
}
