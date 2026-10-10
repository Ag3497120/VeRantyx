// The single-file decoder is compiled separately (zstd/zstddeclib.c); this header only
// declares the two entry points steam_support.c uses.
#include <stddef.h>
size_t ZSTD_decompress(void *dst, size_t dstCapacity, const void *src, size_t compressedSize);
unsigned ZSTD_isError(size_t code);
