#pragma once
// Routes the Linux steam_api to the macOS Steam client (kl_steambridge.c).
void kl_steambridge_install(void);
// Redirect only the guest's Linux Steam service files, preserving the real Mac Steam account.
const char *kl_steambridge_path(const char *path);
