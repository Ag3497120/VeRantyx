#pragma once
// Lets libvscript's LuaJIT arena live above 4 GB (see kl_luafix.c).
void kl_luafix_install(void);
