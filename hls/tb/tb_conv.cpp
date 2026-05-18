#include <cstdio>

#include "conv_top.h"

static void init_input(gemm_data_t input[CONV_CIN][CONV_IN_H][CONV_IN_W]) {
    for (int ci = 0; ci < CONV_CIN; ci++) {
        for (int h = 0; h < CONV_IN_H; h++) {
            for (int w = 0; w < CONV_IN_W; w++) {
                int v = ((ci * 41 + h * 17 + w * 29 + 11) % 96) - 48;
                input[ci][h][w] = (gemm_data_t)v;
            }
        }
    }
}

static void init_weight(gemm_data_t weight[CONV_COUT][CONV_CIN][CONV_KH][CONV_KW]) {
    for (int co = 0; co < CONV_COUT; co++) {
        for (int ci = 0; ci < CONV_CIN; ci++) {
            for (int kh = 0; kh < CONV_KH; kh++) {
                for (int kw = 0; kw < CONV_KW; kw++) {
                    int v = ((co * 37 + ci * 23 + kh * 19 + kw * 13 + 7) % 64) - 32;
                    if (((co + ci + kh + kw) & 1) != 0) {
                        v = -v;
                    }
                    weight[co][ci][kh][kw] = (gemm_data_t)v;
                }
            }
        }
    }
}

static void build_im2col_matrix(
    gemm_data_t input[CONV_CIN][CONV_IN_H][CONV_IN_W],
    gemm_data_t A[CONV_GEMM_N][CONV_GEMM_K]
) {
    for (int oh = 0; oh < CONV_OUT_H; oh++) {
        for (int ow = 0; ow < CONV_OUT_W; ow++) {
            const int row = oh * CONV_OUT_W + ow;
            int col = 0;
            for (int ci = 0; ci < CONV_CIN; ci++) {
                for (int kh = 0; kh < CONV_KH; kh++) {
                    for (int kw = 0; kw < CONV_KW; kw++) {
                        const int ih = oh * CONV_STRIDE + kh;
                        const int iw = ow * CONV_STRIDE + kw;
                        A[row][col] = input[ci][ih][iw];
                        col++;
                    }
                }
            }
        }
    }
}

static void build_weight_matrix(
    gemm_data_t weight[CONV_COUT][CONV_CIN][CONV_KH][CONV_KW],
    gemm_data_t B[CONV_GEMM_K][CONV_GEMM_M]
) {
    for (int co = 0; co < CONV_COUT; co++) {
        int row = 0;
        for (int ci = 0; ci < CONV_CIN; ci++) {
            for (int kh = 0; kh < CONV_KH; kh++) {
                for (int kw = 0; kw < CONV_KW; kw++) {
                    B[row][co] = weight[co][ci][kh][kw];
                    row++;
                }
            }
        }
    }
}

static void build_gemm_matrix_reference(
    gemm_data_t A[CONV_GEMM_N][CONV_GEMM_K],
    gemm_data_t B[CONV_GEMM_K][CONV_GEMM_M],
    gemm_acc_t C[CONV_GEMM_N][CONV_GEMM_M]
) {
    for (int i = 0; i < CONV_GEMM_N; i++) {
        for (int m = 0; m < CONV_GEMM_M; m++) {
            gemm_acc_t raw_sum = 0;
            for (int k = 0; k < CONV_GEMM_K; k++) {
                raw_sum += (gemm_acc_t)A[i][k] * (gemm_acc_t)B[k][m];
            }
            C[i][m] = raw_sum;
        }
    }
}

static void conv2d_reference(
    gemm_data_t input[CONV_CIN][CONV_IN_H][CONV_IN_W],
    gemm_data_t weight[CONV_COUT][CONV_CIN][CONV_KH][CONV_KW],
    gemm_acc_t expected[CONV_COUT][CONV_OUT_H][CONV_OUT_W]
) {
    for (int co = 0; co < CONV_COUT; co++) {
        for (int oh = 0; oh < CONV_OUT_H; oh++) {
            for (int ow = 0; ow < CONV_OUT_W; ow++) {
                gemm_acc_t raw_sum = 0;
                for (int ci = 0; ci < CONV_CIN; ci++) {
                    for (int kh = 0; kh < CONV_KH; kh++) {
                        for (int kw = 0; kw < CONV_KW; kw++) {
                            const int ih = oh * CONV_STRIDE + kh;
                            const int iw = ow * CONV_STRIDE + kw;
                            raw_sum += (gemm_acc_t)input[ci][ih][iw] *
                                       (gemm_acc_t)weight[co][ci][kh][kw];
                        }
                    }
                }
                expected[co][oh][ow] = raw_sum;
            }
        }
    }
}

static void print_input(gemm_data_t input[CONV_CIN][CONV_IN_H][CONV_IN_W]) {
    for (int ci = 0; ci < CONV_CIN; ci++) {
        std::printf("[TB] input[%d]:\n", ci);
        for (int h = 0; h < CONV_IN_H; h++) {
            for (int w = 0; w < CONV_IN_W; w++) {
                std::printf("%8d", (int)input[ci][h][w]);
            }
            std::printf("\n");
        }
    }
}

static void print_weight(gemm_data_t weight[CONV_COUT][CONV_CIN][CONV_KH][CONV_KW]) {
    for (int co = 0; co < CONV_COUT; co++) {
        for (int ci = 0; ci < CONV_CIN; ci++) {
            std::printf("[TB] weight[%d][%d]:\n", co, ci);
            for (int kh = 0; kh < CONV_KH; kh++) {
                for (int kw = 0; kw < CONV_KW; kw++) {
                    std::printf("%8d", (int)weight[co][ci][kh][kw]);
                }
                std::printf("\n");
            }
        }
    }
}

static void print_matrix_data(const char* name, gemm_data_t* data, int rows, int cols, int stride) {
    std::printf("%s\n", name);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            std::printf("%8d", (int)data[r * stride + c]);
        }
        std::printf("\n");
    }
}

static void print_matrix_acc(const char* name, gemm_acc_t* data, int rows, int cols, int stride) {
    std::printf("%s\n", name);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            std::printf("%8d", (int)data[r * stride + c]);
        }
        std::printf("\n");
    }
}

static void print_output(const char* name, gemm_acc_t data[CONV_COUT][CONV_OUT_H][CONV_OUT_W]) {
    std::printf("%s\n", name);
    for (int co = 0; co < CONV_COUT; co++) {
        std::printf("  channel %d:\n", co);
        for (int oh = 0; oh < CONV_OUT_H; oh++) {
            for (int ow = 0; ow < CONV_OUT_W; ow++) {
                std::printf("%8d", (int)data[co][oh][ow]);
            }
            std::printf("\n");
        }
    }
}

int main() {
    std::printf(
        "[TB] Conv shape: input[%d,%d,%d], weight[%d,%d,%d,%d], GEMM A[%d,%d] x B[%d,%d]\n",
        CONV_CIN,
        CONV_IN_H,
        CONV_IN_W,
        CONV_COUT,
        CONV_CIN,
        CONV_KH,
        CONV_KW,
        CONV_GEMM_N,
        CONV_GEMM_K,
        CONV_GEMM_K,
        CONV_GEMM_M
    );

    gemm_data_t input[CONV_CIN][CONV_IN_H][CONV_IN_W] = {0};
    gemm_data_t weight[CONV_COUT][CONV_CIN][CONV_KH][CONV_KW] = {0};
    gemm_acc_t output[CONV_COUT][CONV_OUT_H][CONV_OUT_W] = {0};
    gemm_acc_t expected[CONV_COUT][CONV_OUT_H][CONV_OUT_W] = {0};
    gemm_data_t A_ref[CONV_GEMM_N][CONV_GEMM_K] = {0};
    gemm_data_t B_ref[CONV_GEMM_K][CONV_GEMM_M] = {0};
    gemm_acc_t C_ref[CONV_GEMM_N][CONV_GEMM_M] = {0};

    init_input(input);
    init_weight(weight);
    build_im2col_matrix(input, A_ref);
    build_weight_matrix(weight, B_ref);
    build_gemm_matrix_reference(A_ref, B_ref, C_ref);

    conv_top(input, weight, output);
    conv2d_reference(input, weight, expected);

    print_input(input);
    print_weight(weight);
    print_matrix_data("[TB] im2col A matrix:", &A_ref[0][0], CONV_GEMM_N, CONV_GEMM_K, CONV_GEMM_K);
    print_matrix_data("[TB] flattened B matrix:", &B_ref[0][0], CONV_GEMM_K, CONV_GEMM_M, CONV_GEMM_M);
    print_matrix_acc("[TB] GEMM C matrix reference:", &C_ref[0][0], CONV_GEMM_N, CONV_GEMM_M, CONV_GEMM_M);
    print_output("[TB] output from HLS conv_top:", output);
    print_output("[TB] golden reference:", expected);

    int errors = 0;
    int max_abs_error = 0;
    long long checksum = 0;

    for (int co = 0; co < CONV_COUT; co++) {
        for (int oh = 0; oh < CONV_OUT_H; oh++) {
            for (int ow = 0; ow < CONV_OUT_W; ow++) {
                const int got = (int)output[co][oh][ow];
                const int ref = (int)expected[co][oh][ow];
                const int diff = got - ref;
                const int abs_diff = diff < 0 ? -diff : diff;
                if (abs_diff > max_abs_error) {
                    max_abs_error = abs_diff;
                }
                checksum += (long long)got * (long long)(co * CONV_OUT_H * CONV_OUT_W + oh * CONV_OUT_W + ow + 1);
                if (got != ref) {
                    std::printf(
                        "[ERR] output[%d][%d][%d] got %d, expected %d\n",
                        co, oh, ow, got, ref
                    );
                    errors++;
                }
            }
        }
    }

    std::printf("[TB] mismatch_count=%d\n", errors);
    std::printf("[TB] max_abs_error=%d\n", max_abs_error);
    std::printf("[TB] checksum=%lld\n", checksum);

    if (errors == 0) {
        std::printf("[PASS] Conv2D -> GEMM validation passed.\n");
        return 0;
    }

    std::printf("[FAIL] Conv2D -> GEMM validation failed, errors=%d\n", errors);
    return 1;
}
