#!/usr/bin/env python3

GEMM_MAX_N = 16
GEMM_MAX_K = 96
GEMM_MAX_M = 96

N = 16
D = 96


def gen_x(i, k):
    return ((i * 31 + k * 17 + 5) % 128) - 64


def gen_w(k, j, seed):
    return ((k * 13 + j * 29 + seed) % 128) - 64


def build_inputs():
    x = [[0 for _ in range(GEMM_MAX_K)] for _ in range(GEMM_MAX_N)]
    wq = [[0 for _ in range(GEMM_MAX_M)] for _ in range(GEMM_MAX_K)]
    wk = [[0 for _ in range(GEMM_MAX_M)] for _ in range(GEMM_MAX_K)]
    wv = [[0 for _ in range(GEMM_MAX_M)] for _ in range(GEMM_MAX_K)]

    for i in range(N):
        for k in range(D):
            x[i][k] = gen_x(i, k)

    for k in range(D):
        for j in range(D):
            wq[k][j] = gen_w(k, j, 7)
            wk[k][j] = gen_w(k, j, 19)
            wv[k][j] = gen_w(k, j, 43)

    return x, wq, wk, wv


def projection_ref(x, w):
    y = [[0 for _ in range(GEMM_MAX_M)] for _ in range(GEMM_MAX_N)]
    for i in range(N):
        for j in range(D):
            acc = 0
            for k in range(D):
                acc += x[i][k] * w[k][j]
            y[i][j] = acc
    return y


def checksum(q, k_mat, v):
    total = 0
    for i in range(N):
        for j in range(D):
            pos = i * D + j + 1
            total += q[i][j] * pos
            total += k_mat[i][j] * (1000 + pos)
            total += v[i][j] * (2000 + pos)
    return total


def print_matrix(name, mat):
    print(name)
    print_rows = min(N, 4)
    print_cols = min(D, 8)
    for i in range(print_rows):
        suffix = " ..." if print_cols < D else ""
        print(" ".join(f"{mat[i][j]:8d}" for j in range(print_cols)) + suffix)
    if print_rows < N:
        print("  ...")


def main():
    x, wq, wk, wv = build_inputs()
    q = projection_ref(x, wq)
    k_mat = projection_ref(x, wk)
    v = projection_ref(x, wv)

    print_matrix("[PY] Q = X x Wq:", q)
    print_matrix("[PY] K = X x Wk:", k_mat)
    print_matrix("[PY] V = X x Wv:", v)
    print(f"[PY] checksum={checksum(q, k_mat, v)}")


if __name__ == "__main__":
    main()
