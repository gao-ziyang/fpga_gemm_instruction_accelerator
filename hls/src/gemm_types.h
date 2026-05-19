#ifndef GZY_GEMM_TYPES_H
#define GZY_GEMM_TYPES_H

#include <ap_int.h>

static const int GEMM_MAX_N = 16;
static const int GEMM_MAX_K = 96;
static const int GEMM_MAX_M = 96;

#ifndef GZY_GEMM_TILE
#define GZY_GEMM_TILE 4
#endif

#ifndef GZY_GEMM_BLOCK_M
#define GZY_GEMM_BLOCK_M 8
#endif

// Local MAC-array tile. Tcl benchmark scripts can override this with -D macros.
static const int GEMM_TILE = GZY_GEMM_TILE;
static const int GEMM_BLOCK_M = GZY_GEMM_BLOCK_M;

typedef ap_int<8> gemm_data_t;
typedef ap_int<32> gemm_acc_t;

#endif
