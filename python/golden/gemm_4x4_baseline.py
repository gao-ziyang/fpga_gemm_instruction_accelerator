#!/usr/bin/env python3

GEMM_MAX_N = 8
GEMM_MAX_K = 8
GEMM_MAX_M = 8
GEMM_OUT_SHIFT = 8

N = 7
K = 6
M = 5


def gen_a(i, k):
    return ((i * 37 + k * 19 + 11) % 128) - 64


def gen_b(k, j):
    return ((k * 23 + j * 29 + 7) % 128) - 64


def build_inputs():
    a = [[0 for _ in range(GEMM_MAX_K)] for _ in range(GEMM_MAX_N)]
    b = [[0 for _ in range(GEMM_MAX_M)] for _ in range(GEMM_MAX_K)]

    for i in range(N):
        for k in range(K):
            a[i][k] = gen_a(i, k)

    for k in range(K):
        for j in range(M):
            b[k][j] = gen_b(k, j)

    return a, b


def matmul_ref(a, b):
    c = [[0 for _ in range(GEMM_MAX_M)] for _ in range(GEMM_MAX_N)]
    for i in range(N):
        for j in range(M):
            acc = 0
            for k in range(K):
                acc += a[i][k] * b[k][j]
            c[i][j] = acc >> GEMM_OUT_SHIFT
    return c


def checksum(mat):
    total = 0
    for i in range(N):
        for j in range(M):
            total += mat[i][j] * (i * M + j + 1)
    return total


def print_matrix(name, mat, rows, cols):
    print(name)
    for i in range(rows):
        print(" ".join(f"{mat[i][j]:8d}" for j in range(cols)))


def main():
    a, b = build_inputs()
    c = matmul_ref(a, b)
    print_matrix("[PY] A:", a, N, K)
    print_matrix("[PY] B:", b, K, M)
    print_matrix("[PY] C = (A x B) >> 8:", c, N, M)
    print(f"[PY] checksum={checksum(c)}")


if __name__ == "__main__":
    main()
