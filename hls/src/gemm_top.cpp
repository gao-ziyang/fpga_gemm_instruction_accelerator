#include "gemm_core.h"

extern "C" {
void gemm_top(
    gemm_data_t A[GEMM_MAX_N][GEMM_MAX_K],
    gemm_data_t B[GEMM_MAX_K][GEMM_MAX_M],
    gemm_acc_t C[GEMM_MAX_N][GEMM_MAX_M],
    int N,
    int K,
    int M,
    bool update_A
) {
#pragma HLS INTERFACE ap_memory port=A//ABC都是数组矩阵必须明确告诉HLS数组从哪里读怎么读
#pragma HLS INTERFACE ap_memory port=B//NKM，update_A是标量，HLS自动推断成普通输入端口因为不是存储器不需要memory接口如ce，we，address
#pragma HLS INTERFACE ap_memory port=C
#pragma HLS INTERFACE ap_ctrl_hs port=return//控制接口，hs表示handshakes，函数调用时握手信号有效，函数执行完成后握手信号再次有效，适合单次调用的函数

    gemm_tiled(A, B, C, N, K, M, update_A);
}
}
