// libvxrt_vm.dylib (the ARM64 runtime links it): every release or replacement of this process's memory is told
// to the runtime first, with malloc's own included (dyld applies a linked library's __interpose tuples to the
// calls of every image). With the program in a vCPU (translator/vcpu.c) the guest must stop seeing memory the host
// gave back: a page put in the VM keeps its old contents for the guest after the host maps something new there.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <sys/mman.h>

static void (*g_hook)(uint64_t addr, uint64_t len);
void vxvm_set_hook(void (*hook)(uint64_t addr, uint64_t len)) { g_hook = hook; }
static void tell(uint64_t addr, uint64_t len) { if (g_hook && len) g_hook(addr, len); }

static int my_munmap(void *a, size_t n) { tell((uint64_t)(uintptr_t)a, n); return munmap(a, n); }
static void *my_mmap(void *a, size_t n, int prot, int flags, int fd, off_t off) {
    if (flags & MAP_FIXED) tell((uint64_t)(uintptr_t)a, n);
    return mmap(a, n, prot, flags, fd, off);
}
static kern_return_t my_mach_vm_deallocate(vm_map_t t, mach_vm_address_t a, mach_vm_size_t n) {
    if (t == mach_task_self()) tell(a, n);
    return mach_vm_deallocate(t, a, n);
}
static kern_return_t my_vm_deallocate(vm_map_t t, vm_address_t a, vm_size_t n) {
    if (t == mach_task_self()) tell(a, n);
    return vm_deallocate(t, a, n);
}
static kern_return_t my_mach_vm_allocate(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t n, int flags) {
    if (t == mach_task_self() && (flags & VM_FLAGS_OVERWRITE)) tell(*a, n);
    return mach_vm_allocate(t, a, n, flags);
}
static kern_return_t my_mach_vm_map(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t n, mach_vm_offset_t mask, int flags,
                                    mem_entry_name_port_t obj, memory_object_offset_t off, boolean_t copy, vm_prot_t cur,
                                    vm_prot_t max, vm_inherit_t inh) {
    if (t == mach_task_self() && (flags & VM_FLAGS_OVERWRITE)) tell(*a, n);
    return mach_vm_map(t, a, n, mask, flags, obj, off, copy, cur, max, inh);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } g_tuples[] = {
    { (const void *)my_munmap, (const void *)munmap }, { (const void *)my_mmap, (const void *)mmap },
    { (const void *)my_mach_vm_deallocate, (const void *)mach_vm_deallocate }, { (const void *)my_vm_deallocate, (const void *)vm_deallocate },
    { (const void *)my_mach_vm_allocate, (const void *)mach_vm_allocate }, { (const void *)my_mach_vm_map, (const void *)mach_vm_map },
};
