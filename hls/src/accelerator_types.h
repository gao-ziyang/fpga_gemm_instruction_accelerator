#ifndef GZY_ACCELERATOR_TYPES_H
#define GZY_ACCELERATOR_TYPES_H

#include <ap_int.h>

#include "gemm_types.h"

#ifndef GZY_ACCEL_MAX_N
#define GZY_ACCEL_MAX_N 1024
#endif

#ifndef GZY_ACCEL_MAX_K
#define GZY_ACCEL_MAX_K 1024
#endif

#ifndef GZY_ACCEL_MAX_M
#define GZY_ACCEL_MAX_M 1024
#endif

#ifndef GZY_ACCEL_BLOCK_N
#define GZY_ACCEL_BLOCK_N (GZY_GEMM_TILE * 8)
#endif

#ifndef GZY_ACCEL_BLOCK_K
#define GZY_ACCEL_BLOCK_K (GZY_GEMM_TILE * 8)
#endif

#ifndef GZY_ACCEL_BLOCK_M
#define GZY_ACCEL_BLOCK_M (GZY_GEMM_TILE * 8)
#endif

#ifndef GZY_ACCEL_MAX_INSTR
#define GZY_ACCEL_MAX_INSTR 64
#endif

static const int ACCEL_MAX_N = GZY_ACCEL_MAX_N;
static const int ACCEL_MAX_K = GZY_ACCEL_MAX_K;
static const int ACCEL_MAX_M = GZY_ACCEL_MAX_M;

static const int ACCEL_BLOCK_N = GZY_ACCEL_BLOCK_N;
static const int ACCEL_BLOCK_K = GZY_ACCEL_BLOCK_K;
static const int ACCEL_BLOCK_M = GZY_ACCEL_BLOCK_M;

static const int ACCEL_A_ELEMS = ACCEL_MAX_N * ACCEL_MAX_K;
static const int ACCEL_B_ELEMS = ACCEL_MAX_K * ACCEL_MAX_M;
static const int ACCEL_C_ELEMS = ACCEL_MAX_N * ACCEL_MAX_M;
static const int ACCEL_MAX_INSTR = GZY_ACCEL_MAX_INSTR;

typedef ap_uint<64> accel_instr_word_t;

static const ap_uint<8> ACCEL_OP_END = 0;
static const ap_uint<8> ACCEL_OP_GEMM = 1;
static const ap_uint<8> ACCEL_OP_QKV = 2;
static const ap_uint<8> ACCEL_OP_ATTN_SCORE = 3;
static const ap_uint<8> ACCEL_OP_CONV_GEMM = 4;
static const ap_uint<8> ACCEL_OP_CONV2D = 5;
static const ap_uint<8> ACCEL_OP_QKV_DDR = 6;
static const ap_uint<8> ACCEL_OP_ATTN_SCORE_DDR = 7;
static const ap_uint<8> ACCEL_OP_ATTN_NORM = 8;
static const ap_uint<8> ACCEL_OP_ATTN_VALUE = 9;
static const int ACCEL_BASE_UNIT = 4096;
static const int ACCEL_QKV_B_STRIDE = ACCEL_BASE_UNIT * 4;
static const int ACCEL_QKV_C_STRIDE = ACCEL_BASE_UNIT;

#ifndef GZY_ACCEL_BENCH_N
#define GZY_ACCEL_BENCH_N 1024
#endif

#ifndef GZY_ACCEL_BENCH_K
#define GZY_ACCEL_BENCH_K 1024
#endif

#ifndef GZY_ACCEL_BENCH_M
#define GZY_ACCEL_BENCH_M 1024
#endif

static const int ACCEL_BENCH_N = GZY_ACCEL_BENCH_N;
static const int ACCEL_BENCH_K = GZY_ACCEL_BENCH_K;
static const int ACCEL_BENCH_M = GZY_ACCEL_BENCH_M;

#endif
