#!/usr/bin/env python3

GEMM_DIM = 4

A = [
    [1, -2, 3, 4],
    [5, 6, -7, 8],
    [-1, 2, -3, 4],
    [9, -8, 7, -6],
]

B = [
    [1, 2, -3, 4],
    [-5, 6, 7, -8],
    [9, -10, 11, 12],
    [-13, 14, -15, 16],
]


def matmul_4x4(a, b):
    c = [[0 for _ in range(GEMM_DIM)] for _ in range(GEMM_DIM)]
    for i in range(GEMM_DIM):
        for j in range(GEMM_DIM):
            acc = 0
            for k in range(GEMM_DIM):
                acc += a[i][k] * b[k][j]
            c[i][j] = acc
    return c


def print_matrix(name, mat):
    print(name)
    for row in mat:
        print(" ".join(f"{x:8d}" for x in row))


def main():
    c = matmul_4x4(A, B)
    print_matrix("[PY] A:", A)
    print_matrix("[PY] B:", B)
    print_matrix("[PY] C = A x B:", c)


if __name__ == "__main__":
    main()
