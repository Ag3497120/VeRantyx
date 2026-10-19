#ifndef VX_DOTNET_HOST_H
#define VX_DOTNET_HOST_H

void vx_dotnet_init(void);
// Returns -1 for native PE images, otherwise the managed process exit code.
int vx_dotnet_run_file(const char *path);

#endif
