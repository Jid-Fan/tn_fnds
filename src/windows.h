#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <xmmintrin.h>
#include <pmmintrin.h>
using std::min;
using std::max;
typedef uint32_t DWORD;
#include <malloc.h>
#if !defined(__MINGW32__)
#define _aligned_malloc(a,b) memalign(b,a)
#define _aligned_free(a) free(a)
#endif
