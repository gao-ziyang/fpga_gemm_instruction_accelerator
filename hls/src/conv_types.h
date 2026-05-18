#ifndef GZY_CONV_TYPES_H
#define GZY_CONV_TYPES_H

#include "gemm_types.h"

static const int CONV_CIN = 3;
static const int CONV_IN_H = 6;
static const int CONV_IN_W = 6;
static const int CONV_KH = 3;
static const int CONV_KW = 3;
static const int CONV_COUT = 4;
static const int CONV_STRIDE = 1;
static const int CONV_OUT_H = 4;
static const int CONV_OUT_W = 4;

static const int CONV_INPUT_H_STRIDE = CONV_IN_W;
static const int CONV_INPUT_C_STRIDE = CONV_IN_H * CONV_IN_W;
static const int CONV_INPUT_SIZE = CONV_CIN * CONV_INPUT_C_STRIDE;

static const int CONV_WEIGHT_KW_STRIDE = 1;
static const int CONV_WEIGHT_KH_STRIDE = CONV_KW;
static const int CONV_WEIGHT_CI_STRIDE = CONV_KH * CONV_KW;
static const int CONV_WEIGHT_CO_STRIDE = CONV_CIN * CONV_WEIGHT_CI_STRIDE;
static const int CONV_WEIGHT_SIZE = CONV_COUT * CONV_WEIGHT_CO_STRIDE;

static const int CONV_OUTPUT_H_STRIDE = CONV_OUT_W;
static const int CONV_OUTPUT_C_STRIDE = CONV_OUT_H * CONV_OUT_W;
static const int CONV_OUTPUT_SIZE = CONV_COUT * CONV_OUTPUT_C_STRIDE;

static const int CONV_GEMM_N = CONV_OUT_H * CONV_OUT_W;
static const int CONV_GEMM_K = CONV_CIN * CONV_KH * CONV_KW;
static const int CONV_GEMM_M = CONV_COUT;

#endif
