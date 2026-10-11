#pragma once
#include "klepton.h"
// Print the engine's tier0 console output to stderr. Install before loading
// the engine; bind once libtier0.so is loaded so the real routines still run.
void kl_enginelog_install(void);
void kl_enginelog_bind(kl_image *tier0);
unsigned kl_enginelog_lines(void);
