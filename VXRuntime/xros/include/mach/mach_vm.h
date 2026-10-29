// visionOS (xros/README.md): the SDK refuses <mach/mach_vm.h>, but the same calls exist as vm_* (vm_map.h), whose
// address and size types are 64-bit on arm64. Only the build for xros puts this directory on the include path.
#pragma once
#include <mach/mach.h>
#include <mach/vm_map.h>
static inline kern_return_t mach_vm_allocate(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t n, int flags) {
    vm_address_t v = (vm_address_t)*a; kern_return_t r = vm_allocate(t, &v, (vm_size_t)n, flags); *a = v; return r;
}
static inline kern_return_t mach_vm_deallocate(vm_map_t t, mach_vm_address_t a, mach_vm_size_t n) {
    return vm_deallocate(t, (vm_address_t)a, (vm_size_t)n);
}
static inline kern_return_t mach_vm_protect(vm_map_t t, mach_vm_address_t a, mach_vm_size_t n, boolean_t set_max, vm_prot_t p) {
    return vm_protect(t, (vm_address_t)a, (vm_size_t)n, set_max, p);
}
static inline kern_return_t mach_vm_behavior_set(vm_map_t t, mach_vm_address_t a, mach_vm_size_t n, vm_behavior_t b) {
    return vm_behavior_set(t, (vm_address_t)a, (vm_size_t)n, b);
}
static inline kern_return_t mach_vm_read_overwrite(vm_map_read_t t, mach_vm_address_t a, mach_vm_size_t n, mach_vm_address_t data, mach_vm_size_t *out) {
    vm_size_t o = 0; kern_return_t r = vm_read_overwrite(t, (vm_address_t)a, (vm_size_t)n, (vm_address_t)data, &o); if (out) *out = o; return r;
}
static inline kern_return_t mach_vm_map(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t n, mach_vm_offset_t mask, int flags,
                                        mem_entry_name_port_t obj, memory_object_offset_t off, boolean_t copy,
                                        vm_prot_t cur, vm_prot_t max, vm_inherit_t inh) {
    vm_address_t v = (vm_address_t)*a;
    kern_return_t r = vm_map(t, &v, (vm_size_t)n, (vm_address_t)mask, flags, obj, (vm_offset_t)off, copy, cur, max, inh);
    *a = v; return r;
}
static inline kern_return_t mach_vm_region(vm_map_read_t t, mach_vm_address_t *a, mach_vm_size_t *n, vm_region_flavor_t f,
                                           vm_region_info_t info, mach_msg_type_number_t *cnt, mach_port_t *obj) {
    vm_address_t v = (vm_address_t)*a; vm_size_t s = 0;
    kern_return_t r = vm_region_64(t, &v, &s, f, info, cnt, obj); *a = v; *n = s; return r;
}
