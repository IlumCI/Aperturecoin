#define BLAKE3_NO_AVX512 1
#define BLAKE3_NO_AVX2 1
#define BLAKE3_NO_SSE41 1
#define BLAKE3_NO_SSE2 1
#include "../../src/crypto/blake3/blake3.c"
#include "../../src/crypto/blake3/blake3_dispatch.c"
#include "../../src/crypto/blake3/blake3_portable.c"
