#include <cstdio>

#include "gemm_core.h"

static void print_matrix(const char* name, gemm_acc_t mat[GEMM_DIM][GEMM_DIM]) {
    std::printf("%s\n", name);
    for (int i = 0; i < GEMM_DIM; i++) {
        for (int j = 0; j < GEMM_DIM; j++) {
            std::printf("%8d", (int)mat[i][j]);
        }
        std::printf("\n");
    }
}

int main() {
    gemm_data_t A[GEMM_DIM][GEMM_DIM] = {
        {  1, -2,  3,  4 },
        {  5,  6, -7,  8 },
        { -1,  2, -3,  4 },
        {  9, -8,  7, -6 }
    };

    gemm_data_t B[GEMM_DIM][GEMM_DIM] = {
        {   1,   2,  -3,   4 },
        {  -5,   6,   7,  -8 },
        {   9, -10,  11,  12 },
        { -13,  14, -15,  16 }
    };

    gemm_acc_t C[GEMM_DIM][GEMM_DIM] = {0};
    gemm_acc_t golden[GEMM_DIM][GEMM_DIM] = {0};

    gemm_top(A, B, C);

    for (int i = 0; i < GEMM_DIM; i++) {
        for (int j = 0; j < GEMM_DIM; j++) {
            gemm_acc_t sum = 0;
            for (int k = 0; k < GEMM_DIM; k++) {
                sum += (gemm_acc_t)A[i][k] * (gemm_acc_t)B[k][j];
            }
            golden[i][j] = sum;
        }
    }

    int errors = 0;
    for (int i = 0; i < GEMM_DIM; i++) {
        for (int j = 0; j < GEMM_DIM; j++) {
            if (C[i][j] != golden[i][j]) {
                std::printf(
                    "[ERR] C[%d][%d] got %d, expected %d\n",
                    i, j, (int)C[i][j], (int)golden[i][j]
                );
                errors++;
            }
        }
    }

    print_matrix("[TB] C from HLS:", C);
    print_matrix("[TB] Golden:", golden);

    if (errors == 0) {
        std::printf("[TB] PASS\n");
        return 0;
    }

    std::printf("[TB] FAIL, errors=%d\n", errors);
    return 1;
}
