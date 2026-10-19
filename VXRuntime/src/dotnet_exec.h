// Interpreter for the deterministic, architecture-neutral CIL integer subset.
#ifndef VX_DOTNET_EXEC_H
#define VX_DOTNET_EXEC_H
#include "dotnet_metadata.h"
#include <stddef.h>
#include <stdint.h>

typedef void *(*vx_dotnet_export_resolver)(const char *dll, const char *name);
// Supplies the active VXRuntime API export table to the managed framework host.
// Standalone interpreter users may leave it unset; APIs that need native handles
// then fail with a framework-call diagnostic instead of guessing their behavior.
void vx_dotnet_set_export_resolver(vx_dotnet_export_resolver resolver);

// Executes a zero-argument static MethodDef. Unsupported runtime features fail
// with a diagnostic; they are never replaced with guessed return values.
int vx_dotnet_execute_static(const vx_dotnet_metadata *md, uint32_t method_token,
                             int64_t *return_value, char *error, size_t error_capacity);

// Executes a managed entry point with bounded object, field, local, and method
// call support. Unsupported framework services fail with their resolved name.
int vx_dotnet_execute_entry(const vx_dotnet_metadata *md, uint32_t method_token,
                            int64_t *return_value, char *error, size_t error_capacity);
// Like execute_entry, resolving referenced managed assemblies from this directory.
int vx_dotnet_execute_entry_from_directory(const vx_dotnet_metadata *md, uint32_t method_token,
                                           const char *assembly_directory,
                                           int64_t *return_value, char *error, size_t error_capacity);
// Like execute_entry_from_directory, also providing the exact path of the entry image.
// Reflection APIs such as Assembly.Location use this path for the entry assembly.
int vx_dotnet_execute_entry_from_file(const vx_dotnet_metadata *md, uint32_t method_token,
                                     const char *assembly_directory, const char *entry_path,
                                     int64_t *return_value, char *error, size_t error_capacity);

#endif
