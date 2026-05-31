#!/usr/bin/env python3
"""Variable GEMM design-space model, v2.

This script scans GEMM scheduler design points across workloads and hardware
knobs.  It keeps three model layers separate:

1. ideal lower bound;
2. current-HLS loop schedule model;
3. calibrated HLS/resource model.

The resource model deliberately gives the 224 generic-vs-full-only observation
its own terms: generic tail path, inner boundary checks, address muxing, and
runtime full/generic fallback.  This makes the large LUT/FF drop in the
compile-time full-only 224 case visible instead of hiding it in a single
opaque correction factor.
"""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[2]
EXPERIMENTS_CSV = ROOT / "python" / "analysis" / "roofline_experiments.csv"
REPORT_DIR = ROOT / "reports" / "variable_design_space_v2"
FIGURE_DIR = REPORT_DIR / "figures"

LUT_LIMIT = 53_200
DSP_LIMIT = 220
# ZYNQ-7020 has 140 physical BRAM36K blocks, equivalently 280 BRAM18K blocks.
BRAM36_LIMIT_ZYNQ7020 = 140
BRAM18_LIMIT_ZYNQ7020 = BRAM36_LIMIT_ZYNQ7020 * 2
BRAM18_BITS = 18_432 #表示单个BRAM18kbit容量 18 Kibit = 18 * 1024 = 18,432 bits
BRAM36_BITS = BRAM18_BITS * 2
DEFAULT_COMPACT_BLOCK_CANDIDATES = 3

TILE_CANDIDATES = [4, 6, 7, 8, 10, 11, 12, 13, 14, 16]
FIXED_BLOCK_CANDIDATES = [16, 32, 48, 56, 64, 96, 112, 128, 160, 192, 224, 256]
ROW_UNROLL_CANDIDATES = [1, 2, 4]
INPUT_BIT_CANDIDATES = [8, 16]
DDR_EFFICIENCIES = [0.4, 0.6, 0.8, 1.0]


@dataclass(frozen=True)
class Workload:
    name: str
    n: int
    k: int
    m: int
    kind: str
    note: str


@dataclass(frozen=True)
class Architecture:
    name: str
    external_ab_combined: bool = True
    local_ab_combined: bool = False
    double_buffer: bool = False
    dataflow_overlap: bool = False
    full_block_only_fast_path: bool = False
    generic_tail_path: bool = True
    boundary_check_mode: str = "inner_loop_guard"
    store_mode: str = "write_only"
    block_pingpong_ab: bool = False
    block_pingpong_c: bool = False
    current_o8_style_local_db: bool = False
    runtime_full_generic_fallback: bool = False
    note: str = ""

#对应一组完整测试点design point的设计参数，包含工作参数、架构选择以及内部的各个参数大小等
#即workload + architecture + （tile + block_n/k/m + row_unroll ）+ bitwidth + DDR 参数
@dataclass(frozen=True)
class Design:
    workload: Workload
    arch: Architecture
    tile: int
    block_n: int
    block_k: int
    block_m: int
    row_unroll: int
    #下面几个基本固定~~
    col_unroll: int
    input_bits: int
    acc_bits: int
    output_bits: int
    freq_mhz: float
    #~~
    ddr_mb_per_s: float
    ddr_efficiency: float
    axi_burst_bytes: int
    axi_burst_latency_cycles: int


#roofline_experiments.csv中记录的就是MeasuredPoint，包含设计参数和测量指标（cycle、资源等），以及备注信息
#表示“已经真实跑过 HLS 的一个实验点”。它不是模型预测点，而是实验记录点
#roofline_experiments.csv为此前O1~版本得到的真实结果，包含设计参数（n、k、m、tile、block_n/k/m、row_unroll）和测量结果（latency_cycles、bram18k、dsp、ff、lut）以及备注信息。通过分析这些MeasuredPoint，可以得到不同设计选择对性能和资源的影响，从而为模型校准提供依据。
#MeasuredPoint = 原始实验数据
@dataclass
class MeasuredPoint:
    name: str
    n: int
    k: int
    m: int
    tile: int
    block_n: int
    block_k: int
    block_m: int
    row_unroll: int
    latency_cycles: int
    bram18k: int
    dsp: int
    ff: int
    lut: int
    note: str

#Calibration 保存经验校准参数，包括行展开的gap和资源惩罚，以及一些特定架构特征的资源惩罚（如local AB combined、O8 local double buffer、runtime full/generic fallback等），以及一些备注信息。
#用真实 HLS 实验点校准出来的模型修正参数”。由build_calibration函数根据读取的MeasuredPoint计算得到，并在模型中使用这些参数来调整理想模型的预测，使其更接近实际测量结果。
#Calibration   = 从原始实验数据提炼出的模型参数
@dataclass
class Calibration:
    baseline_loop_gap: float = 1.13
    row_loop_gap: dict[int, float] | None = None
    row_lut_penalty: dict[int, float] | None = None
    row_ff_penalty: dict[int, float] | None = None
    local_ab_lut_penalty: float = 34_500.0
    local_ab_ff_penalty: float = 7_000.0
    o8_local_db_lut_penalty: float = 37_700.0
    o8_local_db_ff_penalty: float = 13_700.0
    runtime_fallback_lut_penalty: float = 20_100.0
    runtime_fallback_ff_penalty: float = 29_500.0
    generic_total_lut_penalty_224: float = 31_750.0
    generic_total_ff_penalty_224: float = 5_450.0
    notes: list[str] | None = None

#若干种工作参数加载WORKLOADS
WORKLOADS = [
    Workload("square_128", 128, 128, 128, "square", "Square GEMM 128"),
    Workload("square_224", 224, 224, 224, "square", "Square GEMM 224"),
    Workload("square_256", 256, 256, 256, "square", "Square GEMM 256"),
    Workload("qkv_16x96x96", 16, 96, 96, "transformer", "QKV projection"),
    Workload("qkt_16x96x16", 16, 96, 16, "transformer", "Q x K^T score"),
    Workload("sv_16x16x96", 16, 16, 96, "transformer", "Score x V"),
    Workload("ffn_up_16x96x384", 16, 96, 384, "transformer", "FFN up projection"),
    Workload("ffn_down_16x384x96", 16, 384, 96, "transformer", "FFN down projection"),
    Workload("conv_16x27x4", 16, 27, 4, "cnn_im2col", "Small im2col GEMM"),
    Workload("conv_64x27x16", 64, 27, 16, "cnn_im2col", "Medium im2col GEMM"),
    Workload("conv_196x27x32", 196, 27, 32, "cnn_im2col", "Larger im2col GEMM"),
]

#7条架构路线影响ARCHITECTURES
ARCHITECTURES = [
    Architecture(
        "generic_o1_like",
        note="O1-like generic path with inner-loop boundary guards.",
    ),
    Architecture(
        "full_only_static",
        full_block_only_fast_path=True,
        generic_tail_path=False,
        boundary_check_mode="none",
        note="Compile-time full-only. Legal only when N/K/M and blocks are tile/block aligned.",
    ),
    Architecture(
        "local_ab_helper",
        local_ab_combined=True,
        note="O4/O5-style helper. Kept as a negative exploration point.",
    ),
    Architecture(
        "o8_local_db_current",
        double_buffer=True,
        current_o8_style_local_db=True,
        note="O8-style local double buffer over helper/local arrays; known not deployable.",
    ),
    Architecture(
        "block_ab_pingpong_analysis",
        double_buffer=True,
        block_pingpong_ab=True,
        note="Route-D-lite: A/B block ping-pong for load/compute overlap only.",
    ),
    Architecture(
        "block_abc_dataflow_analysis",
        double_buffer=True,
        dataflow_overlap=True,
        block_pingpong_ab=True,
        block_pingpong_c=True,
        note="Route D full load/compute/store overlap analysis point.",
    ),
    Architecture(
        "runtime_full_generic_fastpath",
        full_block_only_fast_path=True,
        generic_tail_path=True,
        runtime_full_generic_fallback=True,
        boundary_check_mode="inner_loop_guard",
        note="O6-style runtime full/generic fallback; can duplicate compute datapath.",
    ),
]


def ceil_div(a: int | float, b: int | float) -> int:
    return int(math.ceil(float(a) / float(b))) if b else 0


def safe_div(a: float | int, b: float | int) -> float:
    return float(a) / float(b) if b else 0.0


def sanitize(name: str) -> str:
    return "".join(ch if ch.isalnum() or ch in {"_", "-"} else "_" for ch in name)


def as_bool(value: str) -> bool:
    return value.strip().lower() in {"1", "true", "yes", "y", "on"}


def read_measured_points(path: Path) -> dict[str, MeasuredPoint]:
    if not path.exists():
        return {}
    with path.open("r", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    points: dict[str, MeasuredPoint] = {}
    for row in rows:
        p = MeasuredPoint(
            name=row["name"],
            n=int(row["n"]),
            k=int(row["k"]),
            m=int(row["m"]),
            tile=int(row["tile"]),
            block_n=int(row["block_n"]),
            block_k=int(row["block_k"]),
            block_m=int(row["block_m"]),
            row_unroll=int(row["row_unroll"]),
            latency_cycles=int(row["latency_cycles"]),
            bram18k=int(row["bram18k"]),
            dsp=int(row["dsp"]),
            ff=int(row["ff"]),
            lut=int(row["lut"]),
            note=row.get("note", ""),
        )
        points[p.name] = p
    return points


def build_calibration(points: dict[str, MeasuredPoint]) -> Calibration:
    notes: list[str] = []

    row_loop_gap = {1: 1.13, 2: 1.16, 4: 1.35}
    row_lut_penalty = {1: 0.0, 2: 18_500.0, 4: 33_700.0}
    row_ff_penalty = {1: 0.0, 2: 1_050.0, 4: 9_200.0}

    if "O1" in points:
        notes.append("O1 is used as the deployable baseline calibration point.")
    if "O2" in points and "O1" in points:
        row_lut_penalty[2] = max(0.0, points["O2"].lut - points["O1"].lut)
        row_ff_penalty[2] = max(0.0, points["O2"].ff - points["O1"].ff)
        notes.append(
            f"row_unroll=2 LUT penalty calibrated from O2-O1: {row_lut_penalty[2]:.0f}."
        )
    if "O7a" in points and "O1" in points:
        row_lut_penalty[4] = max(0.0, points["O7a"].lut - points["O1"].lut)
        row_ff_penalty[4] = max(0.0, points["O7a"].ff - points["O1"].ff)
        notes.append(
            f"row_unroll=4 LUT penalty calibrated from O7a-O1: {row_lut_penalty[4]:.0f}."
        )

    local_ab_lut_penalty = 34_500.0
    local_ab_ff_penalty = 7_000.0
    if "O4" in points and "O1" in points:
        local_ab_lut_penalty = max(0.0, points["O4"].lut - points["O1"].lut)
        local_ab_ff_penalty = max(0.0, points["O4"].ff - points["O1"].ff)
        notes.append("local A/B helper penalty calibrated from O4-O1.")

    o8_lut = 37_700.0
    o8_ff = 13_700.0
    if "O8a" in points and "O1" in points:
        o8_lut = max(0.0, points["O8a"].lut - points["O1"].lut)
        o8_ff = max(0.0, points["O8a"].ff - points["O1"].ff)
        notes.append("O8 local double-buffer penalty calibrated from O8a-O1.")

    runtime_lut = 20_100.0
    runtime_ff = 29_500.0
    if "O6a" in points and "O1" in points:
        runtime_lut = max(0.0, points["O6a"].lut - points["O1"].lut)
        runtime_ff = max(0.0, points["O6a"].ff - points["O1"].ff)
        notes.append("runtime full/generic fallback penalty calibrated from O6a-O1.")

    generic_lut = 31_750.0
    generic_ff = 5_450.0
    if "O1_224_generic" in points and "O6c_fullonly_224" in points:
        generic_lut = max(0.0, points["O1_224_generic"].lut - points["O6c_fullonly_224"].lut)
        generic_ff = max(0.0, points["O1_224_generic"].ff - points["O6c_fullonly_224"].ff)
        lut_pct = safe_div(generic_lut, points["O1_224_generic"].lut)
        ff_pct = safe_div(generic_ff, points["O1_224_generic"].ff)
        notes.append(
            "224 full-only calibration: generic tail/boundary/address path "
            f"accounts for about {lut_pct:.1%} LUT and {ff_pct:.1%} FF of O1_224_generic."
        )

    return Calibration(
        row_loop_gap=row_loop_gap,
        row_lut_penalty=row_lut_penalty,
        row_ff_penalty=row_ff_penalty,
        local_ab_lut_penalty=local_ab_lut_penalty,
        local_ab_ff_penalty=local_ab_ff_penalty,
        o8_local_db_lut_penalty=o8_lut,
        o8_local_db_ff_penalty=o8_ff,
        runtime_fallback_lut_penalty=runtime_lut,
        runtime_fallback_ff_penalty=runtime_ff,
        generic_total_lut_penalty_224=generic_lut,
        generic_total_ff_penalty_224=generic_ff,
        notes=notes,
    )


def full_only_legal(d: Design) -> bool:
    w = d.workload
    return (
        w.n % d.block_n == 0
        and w.k % d.block_k == 0
        and w.m % d.block_m == 0
        and d.block_n % d.tile == 0
        and d.block_k % d.tile == 0
        and d.block_m % d.tile == 0
    )


def block_candidates(
    dim: int,
    tile: int,
    compact: bool = True,
    compact_limit: int = DEFAULT_COMPACT_BLOCK_CANDIDATES,
) -> list[int]:
    values = {v for v in FIXED_BLOCK_CANDIDATES if v <= dim}
    values.add(dim)
    values.add(max(1, min(dim, tile)))
    values.add(max(1, min(dim, tile * 4)))
    values.add(max(1, min(dim, tile * 8)))
    values = {v for v in values if v >= 1 and v <= dim}
    if not compact:
        return sorted(values)

    def score(v: int) -> tuple[float, int]:
        tail = 0 if dim % v == 0 else 1
        tile_align = 0 if v % tile == 0 else 1
        preferred = min(abs(v - tile * 8), abs(v - dim), abs(v - 64), abs(v - 112))
        return (tail * 4 + tile_align * 2 + preferred / max(dim, 1), -v)

    selected = sorted(values, key=score)[: max(1, compact_limit)]
    return sorted(selected)


def generate_designs(
    workloads: Iterable[Workload],
    bram18_limit: int,
    compact_blocks: bool = True,
    compact_block_candidates: int = DEFAULT_COMPACT_BLOCK_CANDIDATES,
) -> Iterable[Design]:
    for workload in workloads:
        for tile in TILE_CANDIDATES:
            if tile > max(workload.n, workload.k, workload.m):
                continue
            bn_values = block_candidates(workload.n, tile, compact_blocks, compact_block_candidates)
            bk_values = block_candidates(workload.k, tile, compact_blocks, compact_block_candidates)
            bm_values = block_candidates(workload.m, tile, compact_blocks, compact_block_candidates)
            for block_n in bn_values:
                for block_k in bk_values:
                    for block_m in bm_values:
                        for row_unroll in ROW_UNROLL_CANDIDATES:
                            if row_unroll > tile:
                                continue
                            for input_bits in INPUT_BIT_CANDIDATES:
                                for ddr_eff in DDR_EFFICIENCIES:
                                    for arch in ARCHITECTURES:
                                        design = Design(
                                            workload=workload,
                                            arch=arch,
                                            tile=tile,
                                            block_n=block_n,
                                            block_k=block_k,
                                            block_m=block_m,
                                            row_unroll=row_unroll,
                                            col_unroll=tile,#目前默认按照tile大小列展开，方便mac调用。
                                            input_bits=input_bits,
                                            acc_bits=32,
                                            output_bits=32,
                                            freq_mhz=100.0,
                                            ddr_mb_per_s=800.0,
                                            ddr_efficiency=ddr_eff,
                                            axi_burst_bytes=64,
                                            axi_burst_latency_cycles=0,
                                        )
                                        if arch.name == "full_only_static" and not full_only_legal(design):
                                            continue
                                        yield design


def c_bytes(d: Design) -> int:
    base = ceil_div(d.workload.n * d.workload.m * d.output_bits, 8)
    mode = d.arch.store_mode
    if mode == "write_only":
        return base
    if mode == "read_write":
        return 2 * base
    if mode == "per_k_block_read_write":
        return 2 * ceil_div(d.workload.k, d.block_k) * base
    raise ValueError(f"unknown store_mode {mode}")


def external_traffic(d: Design) -> dict[str, float]:
    w = d.workload
    input_bytes = d.input_bits / 8.0
    a_bytes = ceil_div(w.m, d.block_m) * w.n * w.k * input_bytes
    b_bytes = ceil_div(w.n, d.block_n) * w.k * w.m * input_bytes
    c = c_bytes(d)
    return {
        "a_bytes": a_bytes,
        "b_bytes": b_bytes,
        "c_bytes": c,
        "external_bytes": a_bytes + b_bytes + c,
    }


def block_counts(d: Design) -> dict[str, int]:
    w = d.workload
    return {
        "n_blk": ceil_div(w.n, d.block_n),
        "k_blk": ceil_div(w.k, d.block_k),
        "m_blk": ceil_div(w.m, d.block_m),
        "tile_n": ceil_div(d.block_n, d.tile),
        "tile_k": ceil_div(d.block_k, d.tile),
        "tile_m": ceil_div(d.block_m, d.tile),
    }


def tail_counts(d: Design) -> dict[str, float | int]:
    w = d.workload
    counts = block_counts(d)
    total_blocks = counts["n_blk"] * counts["m_blk"] * counts["k_blk"]
    full_blocks = 0
    tail_blocks = 0
    for n0 in range(0, w.n, d.block_n):
        cur_n = min(d.block_n, w.n - n0)
        for m0 in range(0, w.m, d.block_m):
            cur_m = min(d.block_m, w.m - m0)
            for k0 in range(0, w.k, d.block_k):
                cur_k = min(d.block_k, w.k - k0)
                if cur_n == d.block_n and cur_m == d.block_m and cur_k == d.block_k:
                    full_blocks += 1
                else:
                    tail_blocks += 1
    output_tiles_per_block = counts["tile_n"] * counts["tile_m"]
    return {
        "full_block_count": full_blocks,
        "tail_block_count": tail_blocks,
        "tail_ratio": safe_div(tail_blocks, total_blocks),
        "full_tile_count": full_blocks * output_tiles_per_block,
        "tail_tile_count": tail_blocks * output_tiles_per_block,
    }


def ideal_lower_bound(d: Design) -> dict[str, float]:
    w = d.workload
    traffic = external_traffic(d)
    ddr_bytes_per_cycle = (d.ddr_mb_per_s / d.freq_mhz) * d.ddr_efficiency
    macs = w.n * w.k * w.m
    compute_roof_mac_per_cycle = d.tile * d.tile
    compute_cycles_ideal = safe_div(macs, compute_roof_mac_per_cycle)
    external_cycles_ideal = safe_div(traffic["external_bytes"], ddr_bytes_per_cycle)

    counts = block_counts(d)
    ideal_compute_per_k_tile = d.tile
    ideal_internal_tile_cycles = 1 + counts["tile_k"] * (1 + 1 + ideal_compute_per_k_tile) + 1
    ideal_internal_cycles = (
        counts["n_blk"]
        * counts["m_blk"]
        * counts["k_blk"]
        * counts["tile_n"]
        * counts["tile_m"]
        * ideal_internal_tile_cycles
    )
    ideal_ext_load_cycles = safe_div(traffic["a_bytes"] + traffic["b_bytes"], ddr_bytes_per_cycle)
    ideal_ext_store_cycles = safe_div(traffic["c_bytes"], ddr_bytes_per_cycle)
    return {
        "ddr_bytes_per_cycle": ddr_bytes_per_cycle,
        "compute_roof_mac_per_cycle": compute_roof_mac_per_cycle,
        "compute_cycles_ideal": compute_cycles_ideal,
        "external_cycles_ideal": external_cycles_ideal,
        "ideal_roofline_cycles": max(compute_cycles_ideal, external_cycles_ideal),
        "ideal_no_overlap_cycles": ideal_ext_load_cycles + ideal_internal_cycles + ideal_ext_store_cycles,
        "ideal_internal_cycles": ideal_internal_cycles,
        "ideal_ext_load_cycles": ideal_ext_load_cycles,
        "ideal_ext_store_cycles": ideal_ext_store_cycles,
    }


def local_iis(d: Design) -> tuple[int, int, int]:
    if d.arch.current_o8_style_local_db:
        return 7, 7, 1
    if d.arch.local_ab_combined:
        return 1, 1, 7
    return 1, 1, 1


def hls_loop_schedule(d: Design, cal: Calibration) -> dict[str, float | int]:
    counts = block_counts(d)
    tails = tail_counts(d)
    n_blk = counts["n_blk"]
    k_blk = counts["k_blk"]
    m_blk = counts["m_blk"]
    tile_n = counts["tile_n"]
    tile_m = counts["tile_m"]
    tile_k = counts["tile_k"]
#外部 load AB 的周期数估计：如果外部 AB combined，则每个 block 需要 load block_k * max(block_n, block_m) 的数据；否则需要 load block_n * block_k + block_k * block_m 的数据。总的 load AB 周期数为 n_blk * m_blk * k_blk * 每个 block 的 load 周期数。
    if d.arch.external_ab_combined:
        t_load_per_block = d.block_k * max(d.block_n, d.block_m)
    else:
        t_load_per_block = d.block_n * d.block_k + d.block_k * d.block_m
    t_load_ab_block = n_blk * m_blk * k_blk * t_load_per_block
#内部 local feeding + MAC compute 的周期数估计：每个输出 tile 需要 local read C、tile_k 次 local load A/B（或 AB combined）和 MAC compute，以及 local write C。每个 block 内的输出 tile 数为 tile_n * tile_m，每个 block 的数量为 n_blk * m_blk * k_blk。
    local_a_ii, local_b_ii, local_ab_ii = local_iis(d)
    rows = ceil_div(d.tile, d.row_unroll)
    t_local_c_read = rows
    t_local_a_load = rows * local_a_ii
    t_local_b_load = rows * local_b_ii
    t_local_ab_load = d.tile * local_ab_ii if d.arch.local_ab_combined else 0
    t_mac_compute = d.tile
    t_local_c_write = rows

    if d.arch.local_ab_combined:
        t_one_output_tile = t_local_c_read + tile_k * (t_local_ab_load + t_mac_compute) + t_local_c_write
        local_load_store_cycles = t_local_c_read + tile_k * t_local_ab_load + t_local_c_write
    else:
        t_one_output_tile = (
            t_local_c_read
            + tile_k * (t_local_a_load + t_local_b_load + t_mac_compute)
            + t_local_c_write
        )
        local_load_store_cycles = (
            t_local_c_read + tile_k * (t_local_a_load + t_local_b_load) + t_local_c_write
        )

    if d.arch.current_o8_style_local_db:
        t_preload = t_local_a_load + t_local_b_load
        t_step = max(t_local_a_load + t_local_b_load, t_mac_compute)
        t_one_output_tile_with_db_ideal = (
            t_local_c_read + t_preload + max(tile_k - 1, 0) * t_step + t_mac_compute + t_local_c_write
        )
    else:
        t_one_output_tile_with_db_ideal = t_one_output_tile

    t_compute_block_internal = n_blk * m_blk * k_blk * tile_n * tile_m * t_one_output_tile
    t_store_c_block = n_blk * m_blk * d.block_n * d.block_m
    t_total_no_overlap_base = t_load_ab_block + t_compute_block_internal + t_store_c_block
#tail penalty 和 boundary check penalty的周期数估计：tail penalty 主要来自于 tail block 中的输出 tile 需要额外的周期数来处理不满 tile 的情况，估计为 tail_tile_count * tile_k * rows * boundary_mode_weight，其中 boundary_mode_weight 根据边界检查模式调整；boundary check penalty 来自于每个 block 内的输出 tile 都需要进行边界检查，估计为 (full_tile_count + tail_tile_count) * tile_k * 0.25 * boundary_mode_weight。
    boundary_mode_weight = {
        "none": 0.0,
        "outer_block_only": 0.4,
        "inner_loop_guard": 1.0,
    }.get(d.arch.boundary_check_mode, 1.0)
    tail_penalty_cycles = tails["tail_tile_count"] * tile_k * rows * boundary_mode_weight
    boundary_check_penalty_cycles = (
        (tails["full_tile_count"] + tails["tail_tile_count"]) * tile_k * 0.25 * boundary_mode_weight
    )

    gap = cal.row_loop_gap.get(d.row_unroll, cal.baseline_loop_gap) if cal.row_loop_gap else cal.baseline_loop_gap
    if d.arch.local_ab_combined:
        gap = max(gap, 1.16)
    if d.arch.current_o8_style_local_db:
        gap = 0.70
    if d.arch.dataflow_overlap:
        gap = max(1.08, gap - 0.05)

    t_control_est = max(0.0, t_total_no_overlap_base * (gap - 1.0))
    if d.arch.current_o8_style_local_db:
        t_control_est = t_total_no_overlap_base * (gap - 1.0)
    t_total_no_overlap = t_total_no_overlap_base + t_control_est + tail_penalty_cycles + boundary_check_penalty_cycles

    compute_per_k_block = tile_n * tile_m * t_one_output_tile
    store_per_output_block = d.block_n * d.block_m
    if d.arch.block_pingpong_ab and not d.arch.block_pingpong_c:
        per_output_block = k_blk * max(t_load_per_block, compute_per_k_block) + store_per_output_block
        t_total_with_overlap = n_blk * m_blk * per_output_block + t_control_est
    elif d.arch.dataflow_overlap and d.arch.block_pingpong_c:
        per_output_block = max(k_blk * t_load_per_block, k_blk * compute_per_k_block, store_per_output_block)
        fill_drain = k_blk * t_load_per_block + store_per_output_block
        t_total_with_overlap = n_blk * m_blk * per_output_block + fill_drain + t_control_est
    else:
        t_total_with_overlap = t_total_no_overlap

    compute_cycles_per_output_tile = tile_k * t_mac_compute
    return {
        **counts,
        **tails,
        "T_load_per_block": t_load_per_block,
        "T_load_AB_block": t_load_ab_block,
        "T_compute_block_internal": t_compute_block_internal,
        "T_store_C_block": t_store_c_block,
        "T_control_est": t_control_est,
        "T_total_no_overlap": t_total_no_overlap,
        "T_total_with_overlap": t_total_with_overlap,
        "T_one_output_tile": t_one_output_tile,
        "T_one_output_tile_db_ideal": t_one_output_tile_with_db_ideal,
        "T_local_C_read": t_local_c_read,
        "T_local_A_load": t_local_a_load,
        "T_local_B_load": t_local_b_load,
        "T_local_AB_load": t_local_ab_load,
        "T_mac_compute": t_mac_compute,
        "T_local_C_write": t_local_c_write,
        "local_load_store_share": safe_div(local_load_store_cycles, t_one_output_tile),
        "compute_share": safe_div(compute_cycles_per_output_tile, t_one_output_tile),
        "tail_penalty_cycles": tail_penalty_cycles,
        "boundary_check_penalty_cycles": boundary_check_penalty_cycles,
    }


def brams_for_bank(words: int, bits: int) -> int:
    return max(1, ceil_div(words * bits, BRAM18_BITS))

#BRAM 模型
def bram_model(d: Design) -> dict[str, float | int]:
    row_factor = max(1, d.row_unroll)
    a_col_banks = max(1, min(d.tile, d.block_k))#abc分别消耗的bank数，已经考虑到了列的bank factor=14了，所以ab均*14倍，c乘以28倍
    b_col_banks = max(1, min(d.tile, d.block_m))
    c_col_banks = max(1, min(d.tile, d.block_m))

    a_banks = a_col_banks * row_factor
    b_banks = b_col_banks * row_factor
    c_banks = c_col_banks * row_factor

    a_words = ceil_div(d.block_n, row_factor) * ceil_div(d.block_k, a_col_banks)
    b_words = ceil_div(d.block_k, row_factor) * ceil_div(d.block_m, b_col_banks)
    c_words = ceil_div(d.block_n, row_factor) * ceil_div(d.block_m, c_col_banks)

    a_per_bank = brams_for_bank(a_words, d.input_bits)
    b_per_bank = brams_for_bank(b_words, d.input_bits)
    c_per_bank = brams_for_bank(c_words, d.acc_bits)
    if d.row_unroll >= 4 and d.acc_bits >= 32:
        c_per_bank = max(c_per_bank, 2)

    a_bram = a_banks * a_per_bank
    b_bram = b_banks * b_per_bank
    c_bram = c_banks * c_per_bank

    raw_bits_a = d.block_n * d.block_k * d.input_bits
    raw_bits_b = d.block_k * d.block_m * d.input_bits
    raw_bits_c = d.block_n * d.block_m * d.acc_bits

    if d.arch.block_pingpong_ab:
        a_bram *= 2
        b_bram *= 2
        raw_bits_a *= 2
        raw_bits_b *= 2
    if d.arch.block_pingpong_c:
        c_bram *= 2
        raw_bits_c *= 2

    bram18_est = a_bram + b_bram + c_bram
    raw_bits = raw_bits_a + raw_bits_b + raw_bits_c
    raw_bram18 = safe_div(raw_bits, BRAM18_BITS)
    return {
        "banking_factor_A": a_banks,
        "banking_factor_B": b_banks,
        "banking_factor_C": c_banks,
        "bram18_A": a_bram,
        "bram18_B": b_bram,
        "bram18_C": c_bram,
        "bram18_est": bram18_est,
        "bram_raw_bits": raw_bits,
        "bram_raw18_est": raw_bram18,
        "bram_packing_efficiency": safe_div(raw_bits, bram18_est * BRAM18_BITS),
    }


def split_generic_resource_penalty(d: Design, cal: Calibration) -> dict[str, float]:
    if d.arch.boundary_check_mode == "none" and not d.arch.generic_tail_path:
        total_lut = 0.0
        total_ff = 0.0
    else:
        scale = (d.tile / 14.0) ** 1.15
        mode_weight = {
            "none": 0.0,
            "outer_block_only": 0.45,
            "inner_loop_guard": 1.0,
        }.get(d.arch.boundary_check_mode, 1.0)
        total_lut = cal.generic_total_lut_penalty_224 * scale * mode_weight
        total_ff = cal.generic_total_ff_penalty_224 * scale * mode_weight

    boundary_lut = total_lut * 0.28
    address_lut = total_lut * 0.32
    generic_tail_lut = total_lut * 0.22 if d.arch.generic_tail_path else 0.0
    fallback_path_lut = total_lut * 0.18 if d.arch.generic_tail_path else 0.0

    boundary_ff = total_ff * 0.30
    address_ff = total_ff * 0.25
    generic_tail_ff = total_ff * 0.25 if d.arch.generic_tail_path else 0.0
    fallback_path_ff = total_ff * 0.20 if d.arch.generic_tail_path else 0.0
    return {
        "generic_resource_lut_penalty": total_lut,
        "boundary_check_lut_penalty": boundary_lut,
        "address_mux_lut_penalty": address_lut,
        "generic_tail_lut_penalty": generic_tail_lut,
        "fallback_path_lut_penalty": fallback_path_lut,
        "generic_resource_ff_penalty": total_ff,
        "boundary_check_ff_penalty": boundary_ff,
        "address_mux_ff_penalty": address_ff,
        "generic_tail_ff_penalty": generic_tail_ff,
        "fallback_path_ff_penalty": fallback_path_ff,
    }


def resource_model(d: Design, cal: Calibration, bram18_limit: int) -> dict[str, float | int | str]:
    bram = bram_model(d)
    macs_parallel = d.tile * d.tile
    dsp_est = macs_parallel#DSP 模型
    if d.input_bits > 8:
        dsp_est = macs_parallel
    if d.arch.runtime_full_generic_fallback:
        dsp_est *= 2

    scale = (d.tile / 14.0) ** 2
    full_only_base_lut = 1_800.0 + 90.5 * d.tile * d.tile
    full_only_base_ff = 9_000.0 + 106.0 * d.tile * d.tile
    bit_lut_scale = 1.0 if d.input_bits == 8 else 1.25
    bit_ff_scale = 1.0 if d.input_bits == 8 else 1.12

    row_lut = cal.row_lut_penalty.get(d.row_unroll, 0.0) * (d.tile / 14.0) if cal.row_lut_penalty else 0.0
    row_ff = cal.row_ff_penalty.get(d.row_unroll, 0.0) * (d.tile / 14.0) if cal.row_ff_penalty else 0.0

    generic = split_generic_resource_penalty(d, cal)
    local_ab_lut = cal.local_ab_lut_penalty * scale if d.arch.local_ab_combined else 0.0
    local_ab_ff = cal.local_ab_ff_penalty * scale if d.arch.local_ab_combined else 0.0
    o8_lut = cal.o8_local_db_lut_penalty * scale if d.arch.current_o8_style_local_db else 0.0
    o8_ff = cal.o8_local_db_ff_penalty * scale if d.arch.current_o8_style_local_db else 0.0
    runtime_lut = cal.runtime_fallback_lut_penalty * scale if d.arch.runtime_full_generic_fallback else 0.0
    runtime_ff = cal.runtime_fallback_ff_penalty * scale if d.arch.runtime_full_generic_fallback else 0.0

    pingpong_lut = 0.0
    pingpong_ff = 0.0
    if d.arch.block_pingpong_ab:
        pingpong_lut += 2_500.0 + 18.0 * (bram["banking_factor_A"] + bram["banking_factor_B"])
        pingpong_ff += 1_200.0 + 12.0 * (bram["banking_factor_A"] + bram["banking_factor_B"])
    if d.arch.block_pingpong_c:
        pingpong_lut += 4_000.0 + 24.0 * bram["banking_factor_C"]
        pingpong_ff += 2_000.0 + 18.0 * bram["banking_factor_C"]
    if d.arch.dataflow_overlap:
        pingpong_lut += 8_000.0
        pingpong_ff += 6_000.0

    lut_est = (
        full_only_base_lut * bit_lut_scale
        + row_lut
        + generic["generic_resource_lut_penalty"]
        + local_ab_lut
        + o8_lut
        + runtime_lut
        + pingpong_lut
    )
    ff_est = (
        full_only_base_ff * bit_ff_scale
        + row_ff
        + generic["generic_resource_ff_penalty"]
        + local_ab_ff
        + o8_ff
        + runtime_ff
        + pingpong_ff
    )

    if d.arch.name == "full_only_static" and not full_only_legal(d):
        lut_est += 1_000_000
        ff_est += 1_000_000

    bram18_est = bram["bram18_est"]
    bram36_equiv_est = safe_div(bram18_est, 2.0)
    bram36_feasible = bram36_equiv_est <= BRAM36_LIMIT_ZYNQ7020
    resource_feasible = (
        lut_est <= LUT_LIMIT
        and dsp_est <= DSP_LIMIT
        and bram18_est <= bram18_limit
    )
    return {
        **bram,
        **generic,
        "bram36_equiv_est": bram36_equiv_est,
        "bram36_limit": BRAM36_LIMIT_ZYNQ7020,
        "bram36_feasible": int(bram36_feasible),
        "dsp_est": dsp_est,
        "lut_est": lut_est,
        "ff_est": ff_est,
        "row_unroll_lut_penalty": row_lut,
        "row_unroll_ff_penalty": row_ff,
        "local_ab_lut_penalty": local_ab_lut,
        "local_ab_ff_penalty": local_ab_ff,
        "o8_local_db_lut_penalty": o8_lut,
        "o8_local_db_ff_penalty": o8_ff,
        "runtime_fallback_lut_penalty": runtime_lut,
        "runtime_fallback_ff_penalty": runtime_ff,
        "dataflow_pingpong_lut_penalty": pingpong_lut,
        "dataflow_pingpong_ff_penalty": pingpong_ff,
        "resource_feasible": int(resource_feasible),
        "resource_limit_reason": resource_limit_reason(lut_est, dsp_est, bram18_est, bram18_limit),
    }


def resource_limit_reason(lut: float, dsp: float, bram: float, bram_limit: int) -> str:
    reasons = []
    if lut > LUT_LIMIT:
        reasons.append("LUT")
    if dsp > DSP_LIMIT:
        reasons.append("DSP")
    if bram > bram_limit:
        reasons.append("BRAM")
    return "ok" if not reasons else "+".join(reasons)


def recommended_reason(row: dict[str, float | int | str]) -> str:
    reasons = []
    if int(row["resource_feasible"]):
        reasons.append("fits resource limits")
    else:
        reasons.append(f"not feasible: {row['resource_limit_reason']}")
    if row["arch"] == "full_only_static":
        reasons.append("removes generic tail/boundary/address mux penalties")
    if float(row["tail_ratio"]) > 0:
        reasons.append("has tail blocks")
    if float(row["local_load_store_share"]) > 0.6:
        reasons.append("local feeding dominated")
    if row["arch"] in {"o8_local_db_current", "block_abc_dataflow_analysis"}:
        reasons.append("exploration only")
    return "; ".join(reasons)


def evaluate_design(d: Design, cal: Calibration, bram18_limit: int) -> dict[str, float | int | str]:
    w = d.workload
    traffic = external_traffic(d)
    ideal = ideal_lower_bound(d)
    hls = hls_loop_schedule(d, cal)
    resource = resource_model(d, cal, bram18_limit)#LUT/FF 模型

    macs = w.n * w.k * w.m
    latency_est = hls["T_total_with_overlap"] if d.arch.dataflow_overlap or d.arch.block_pingpong_ab else hls["T_total_no_overlap"]
    if resource["resource_limit_reason"] == "LUT" and d.arch.dataflow_overlap:
        latency_est *= 1.08
    actual_like_mac_per_cycle = safe_div(macs, latency_est)
    actual_like_gops = actual_like_mac_per_cycle * d.freq_mhz / 1000.0 * 2
    external_ctc_mac_per_byte = safe_div(macs, traffic["external_bytes"])
    mem_roof_mac_per_cycle = external_ctc_mac_per_byte * ideal["ddr_bytes_per_cycle"]
    compute_roof_mac_per_cycle = ideal["compute_roof_mac_per_cycle"]
    attainable = min(mem_roof_mac_per_cycle, compute_roof_mac_per_cycle)
    roofline_util = safe_div(actual_like_mac_per_cycle, attainable)
    compute_util = safe_div(actual_like_mac_per_cycle, compute_roof_mac_per_cycle)

    row = {
        "workload": w.name,
        "workload_kind": w.kind,
        "workload_note": w.note,
        "N": w.n,
        "K": w.k,
        "M": w.m,
        "arch": d.arch.name,
        "arch_note": d.arch.note,
        "tile": d.tile,
        "block_n": d.block_n,
        "block_k": d.block_k,
        "block_m": d.block_m,
        "row_unroll": d.row_unroll,
        "col_unroll": d.col_unroll,
        "input_bits": d.input_bits,
        "acc_bits": d.acc_bits,
        "output_bits": d.output_bits,
        "ddr_efficiency": d.ddr_efficiency,
        "external_ab_combined": int(d.arch.external_ab_combined),
        "local_ab_combined": int(d.arch.local_ab_combined),
        "double_buffer": int(d.arch.double_buffer),
        "dataflow_overlap": int(d.arch.dataflow_overlap),
        "full_block_only_fast_path": int(d.arch.full_block_only_fast_path),
        "generic_tail_path": int(d.arch.generic_tail_path),
        "boundary_check_mode": d.arch.boundary_check_mode,
        "store_mode": d.arch.store_mode,
        "full_only_legal": int(full_only_legal(d)),
        "macs": macs,
        **traffic,
        **ideal,
        **hls,
        **resource,
        "latency_cycles_est": latency_est,
        "actual_like_mac_per_cycle_est": actual_like_mac_per_cycle,
        "actual_like_gops_at_freq_est": actual_like_gops,
        "external_ctc_mac_per_byte": external_ctc_mac_per_byte,
        "roofline_util_est": roofline_util,
        "compute_util_est": compute_util,
        "gops_per_dsp": safe_div(actual_like_gops, resource["dsp_est"]),
        "gops_per_klut": safe_div(actual_like_gops, resource["lut_est"] / 1000.0),
        "gops_per_bram18": safe_div(actual_like_gops, resource["bram18_est"]),
    }
    row["recommended_reason"] = recommended_reason(row)
    return row


def write_csv(path: Path, rows: list[dict[str, float | int | str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fields = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def min_by(rows: list[dict[str, float | int | str]], key: str) -> dict[str, float | int | str] | None:
    return min(rows, key=lambda r: float(r[key])) if rows else None


def max_by(rows: list[dict[str, float | int | str]], key: str) -> dict[str, float | int | str] | None:
    return max(rows, key=lambda r: float(r[key])) if rows else None


def add_candidate(
    out: list[dict[str, float | int | str]],
    category: str,
    row: dict[str, float | int | str] | None,
) -> None:
    if row is None:
        return
    item = dict(row)
    item["candidate_category"] = category
    out.append(item)


def select_top_candidates(all_rows: list[dict[str, float | int | str]]) -> list[dict[str, float | int | str]]:
    out: list[dict[str, float | int | str]] = []
    for workload in sorted({str(r["workload"]) for r in all_rows}):
        rows = [r for r in all_rows if r["workload"] == workload]
        feasible = [r for r in rows if int(r["resource_feasible"])]
        add_candidate(out, "best_latency_feasible", min_by(feasible, "latency_cycles_est"))
        add_candidate(out, "best_latency_all", min_by(rows, "latency_cycles_est"))
        add_candidate(out, "best_efficiency", max_by(feasible or rows, "gops_per_klut"))
        balanced_rows = feasible or rows
        max_lat = max(float(r["latency_cycles_est"]) for r in balanced_rows)
        max_lut = max(float(r["lut_est"]) for r in balanced_rows)
        max_bram = max(float(r["bram18_est"]) for r in balanced_rows)
        max_dsp = max(float(r["dsp_est"]) for r in balanced_rows)
        best_balanced = min(
            balanced_rows,
            key=lambda r: (
                safe_div(float(r["latency_cycles_est"]), max_lat)
                + 0.6 * safe_div(float(r["lut_est"]), max_lut)
                + 0.3 * safe_div(float(r["bram18_est"]), max_bram)
                + 0.2 * safe_div(float(r["dsp_est"]), max_dsp)
            ),
        )
        add_candidate(out, "best_balanced", best_balanced)

        baseline_like = min(
            rows,
            key=lambda r: (
                abs(int(r["tile"]) - 14)
                + abs(int(r["block_n"]) - min(112, int(r["N"]))) / 16
                + abs(int(r["block_k"]) - min(112, int(r["K"]))) / 16
                + abs(int(r["block_m"]) - min(112, int(r["M"]))) / 16
                + (0 if r["arch"] == "generic_o1_like" else 5)
            ),
        )
        add_candidate(out, "baseline_like", baseline_like)

        small_rows = [
            r
            for r in (feasible or rows)
            if int(r["tile"]) in {4, 8, 16} and float(r["tail_ratio"]) <= 0.5
        ]
        add_candidate(out, "small_shape_friendly", min_by(small_rows or feasible or rows, "latency_cycles_est"))
    return out


def pareto_frontier(rows: list[dict[str, float | int | str]]) -> list[dict[str, float | int | str]]:
    out: list[dict[str, float | int | str]] = []
    for workload in sorted({str(r["workload"]) for r in rows}):
        wr = [r for r in rows if r["workload"] == workload]
        wr = sorted(wr, key=lambda r: (float(r["lut_est"]), float(r["latency_cycles_est"])))
        best_latency = math.inf
        for row in wr:
            latency = float(row["latency_cycles_est"])
            if latency < best_latency:
                item = dict(row)
                item["pareto_axis"] = "latency_vs_lut"
                out.append(item)
                best_latency = latency
    return out


def workload_summary(top_rows: list[dict[str, float | int | str]]) -> list[dict[str, float | int | str]]:
    rows = []
    for workload in sorted({str(r["workload"]) for r in top_rows}):
        cats = [r for r in top_rows if r["workload"] == workload]
        best = next((r for r in cats if r["candidate_category"] == "best_latency_feasible"), None)
        if best is None:
            best = next((r for r in cats if r["candidate_category"] == "best_latency_all"), None)
        if best is None:
            continue
        rows.append(
            {
                "workload": workload,
                "recommended_arch": best["arch"],
                "tile": best["tile"],
                "block_n": best["block_n"],
                "block_k": best["block_k"],
                "block_m": best["block_m"],
                "row_unroll": best["row_unroll"],
                "input_bits": best["input_bits"],
                "latency_cycles_est": best["latency_cycles_est"],
                "lut_est": best["lut_est"],
                "ff_est": best["ff_est"],
                "dsp_est": best["dsp_est"],
                "bram18_est": best["bram18_est"],
                "tail_ratio": best["tail_ratio"],
                "local_load_store_share": best["local_load_store_share"],
                "resource_feasible": best["resource_feasible"],
                "reason": best["recommended_reason"],
            }
        )
    return rows


def assumptions_md(
    cal: Calibration,
    bram18_limit: int,
    compact: bool,
    compact_block_candidates: int,
) -> str:
    notes = "\n".join(f"- {n}" for n in (cal.notes or []))
    return f"""# Variable Design Space V2 Model Assumptions

## Scope

This model is a design-space estimator, not an HLS replacement.  It separates
ideal lower-bound cycles, current HLS loop-schedule cycles, and calibrated
resource/latency estimates.

## Workloads

The built-in workload list includes square GEMM, Transformer-like QKV/QK^T/SV/FFN
shapes, and CNN im2col-like shapes.  Workloads are represented by a `Workload`
dataclass so new shapes can be added in one list.

## Latency models

- Ideal roofline: `max(N*K*M/(tile*tile), external_bytes/ddr_bytes_per_cycle)`.
- Ideal no-overlap: external load + idealized local/internal + external store.
- HLS loop schedule: current scheduler-style `tripcount x II`, using fixed
  `block_n/block_k/block_m` loop bounds instead of shortening tail loops.
- Calibrated latency: HLS loop total plus calibrated control/gap terms.  For
  DATAFLOW analysis variants, the model also reports an optimistic overlap
  estimate, but these variants are marked exploration unless resources fit.

## Resource models

BRAM is modeled with both raw bits and banked BRAM18 estimates.  ZYNQ-7020 has
`140` physical BRAM36K blocks, which is equivalent to `280` BRAM18K blocks.
The script therefore compares `bram18_est` against a BRAM18K limit, and also
reports `bram36_equiv_est = bram18_est / 2`.

`BRAM18_BITS = 18_432` means one Xilinx BRAM18 stores 18 Kibit:

```text
18 * 1024 = 18,432 bits
```

The banked estimate accounts for tile column banking and row_unroll banking.
For O1-like `tile=14, block=112, row_unroll=1`, the model reproduces:

```text
A_buf = 14 BRAM18K
B_buf = 14 BRAM18K
C_buf = 28 BRAM18K
total = 56 BRAM18K
```

This is `28` BRAM36K-equivalent blocks.  The `TILE=14` column banking is already
included: A has 14 banks, B has 14 banks, and C has 14 banks with 2 BRAM18K per
bank because C stores INT32 accumulators.

BRAM18K limit used for `resource_feasible`: `{bram18_limit}`.
Equivalent BRAM36K physical limit: `{BRAM36_LIMIT_ZYNQ7020}`.
Compact block scan: `{compact}`.
Compact block candidates per dimension: `{compact_block_candidates}` when
compact scan is enabled.  Use `--full-block-scan` for the full fixed block list.

## 224 full-block resource calibration

The model explicitly separates the resource penalty of generic runtime paths:

- `boundary_check_lut_penalty`
- `address_mux_lut_penalty`
- `generic_tail_lut_penalty`
- `fallback_path_lut_penalty`
- corresponding FF terms

This is calibrated from `O1_224_generic` vs `O6c_fullonly_224`.  The observed
drop is not treated as a plotting artifact; it is modeled as a real generic
tail/boundary/address/fallback hardware cost.  Calibration notes:

{notes}

## Known limitations

- LUT/FF are approximate and intended for filtering and ranking, not signoff.
- DATAFLOW overlap points are analysis points.  The main TILE=14/BLOCK=112 line
  should not be changed directly without a small prototype report.
- The model is conservative about O8-style helper/DATAFLOW over local arrays,
  because experiments showed heavy FIFO/control costs.
- INT16 support is structural; it uses conservative scaling rather than direct
  HLS calibration.
"""


def summary_md(
    all_rows: list[dict[str, float | int | str]],
    top_rows: list[dict[str, float | int | str]],
    pareto_rows: list[dict[str, float | int | str]],
    figure_status: str,
) -> str:
    feasible_count = sum(1 for r in all_rows if int(r["resource_feasible"]))
    total_count = len(all_rows)
    lines = [
        "# Variable GEMM Design Space V2 Summary",
        "",
        f"Total scanned points: `{total_count}`",
        f"Resource-feasible points: `{feasible_count}`",
        f"Pareto points: `{len(pareto_rows)}`",
        f"Figure generation: `{figure_status}`",
        "",
        "## Best Feasible Candidates",
        "",
        "| Workload | Arch | tile | block N/K/M | row | latency | LUT | DSP | BRAM | reason |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for row in top_rows:
        if row["candidate_category"] != "best_latency_feasible":
            continue
        lines.append(
            "| {workload} | {arch} | {tile} | {block_n}/{block_k}/{block_m} | "
            "{row_unroll} | {latency_cycles_est:.0f} | {lut_est:.0f} | "
            "{dsp_est:.0f} | {bram18_est:.0f} | {recommended_reason} |".format(**row)
        )
    lines.extend(
        [
            "",
            "## Important Interpretation",
            "",
            "- O1 remains the deployable baseline when the model picks O1-like points near the current constraints.",
            "- O2-like row banking can improve latency, but the calibrated LUT penalty often pushes it beyond 53,200 LUT.",
            "- The 224 full-only resource drop is modeled explicitly as removed generic tail, boundary check, address mux, and fallback hardware.",
            "- Route-D DATAFLOW candidates are analysis/prototype candidates unless they fit LUT/DSP/BRAM and are later confirmed by HLS reports.",
        ]
    )
    return "\n".join(lines) + "\n"


def best_matching(
    all_rows: list[dict[str, float | int | str]],
    workload: str,
    arch: str,
    lut_margin: float | None = None,
) -> dict[str, float | int | str] | None:
    rows = [
        r
        for r in all_rows
        if r["workload"] == workload
        and r["arch"] == arch
        and int(r["input_bits"]) == 8
        and int(r["resource_feasible"]) == 1
    ]
    if lut_margin is not None:
        rows = [r for r in rows if float(r["lut_est"]) <= LUT_LIMIT * lut_margin]
    return min_by(rows, "latency_cycles_est")


def row_summary(row: dict[str, float | int | str] | None) -> str:
    if row is None:
        return "N/A"
    return (
        f"`T={row['tile']}, B={row['block_n']}/{row['block_k']}/{row['block_m']}, "
        f"R={row['row_unroll']}`; "
        f"lat `{float(row['latency_cycles_est']):.0f}`; "
        f"LUT `{float(row['lut_est']):.0f}`; "
        f"DSP `{float(row['dsp_est']):.0f}`; "
        f"BRAM `{float(row['bram18_est']):.0f}`"
    )


def optimal_recommendations_md(all_rows: list[dict[str, float | int | str]]) -> str:
    main_workloads = [
        "square_128",
        "square_224",
        "square_256",
        "qkv_16x96x96",
        "qkt_16x96x16",
        "sv_16x16x96",
        "ffn_up_16x96x384",
        "ffn_down_16x384x96",
    ]
    lines = [
        "# Optimal Parameter Recommendations From V2 Model",
        "",
        "This file is generated from the scanned design points.  It separates",
        "model-optimal parameters into deployable/runtime-safe candidates,",
        "compile-time full-only candidates, and Route-D-lite prototype candidates.",
        "The DATAFLOW prototype rows are not final HLS results.",
        "",
        "## Main Result",
        "",
        "The strongest actionable result is that the current `N=K=M=128` test should",
        "not keep using `BLOCK=112` as the only baseline.  In the generic HLS code,",
        "tail blocks still execute fixed `BLOCK_N/K/M` loop bounds.  Therefore",
        "`128` with `BLOCK=112` expands into `2 x 2 x 2` block iterations.  The model",
        "selects `TILE=13, BLOCK=128, row_unroll=1` as the first synthesis candidate",
        "because it removes those tail blocks while using fewer DSPs than TILE=14.",
        "",
        "## Recommended Next HLS Cases",
        "",
        "| Case | Purpose | Parameters | Model estimate | Why this one |",
        "| --- | --- | --- | --- | --- |",
    ]

    cases = [
        (
            "O9a_square128_block128",
            "First real synthesis target",
            best_matching(all_rows, "square_128", "generic_o1_like", lut_margin=0.95),
            "Generic runtime path; removes 128-shape tail-block amplification; keeps LUT margin.",
        ),
        (
            "O9b_square224_block224_generic",
            "High-BRAM generic 224 check",
            best_matching(all_rows, "square_224", "generic_o1_like"),
            "Tests whether one-block 224 is worth the BRAM cost; still generic, so useful for DDR-style runtime path.",
        ),
        (
            "O9c_square224_fullonly_row2",
            "Specialized full-only check",
            best_matching(all_rows, "square_224", "full_only_static"),
            "Compile-time fixed-shape path; verifies whether removing generic boundary/address hardware plus row_unroll=2 is actually synthesizable.",
        ),
        (
            "D1_qkv_block_ab_pingpong_small",
            "Route-D-lite prototype",
            best_matching(all_rows, "qkv_16x96x96", "block_ab_pingpong_analysis", lut_margin=0.95),
            "Small Transformer-shaped prototype; checks A/B block ping-pong overlap before touching square mainline.",
        ),
    ]
    for name, purpose, row, why in cases:
        lines.append(f"| `{name}` | {purpose} | {row_summary(row)} | {why} |")

    lines += [
        "",
        "## Per-Workload Best Rows",
        "",
        "| Workload | Generic runtime safe | Full-only static | Route-D-lite prototype |",
        "| --- | --- | --- | --- |",
    ]
    for workload in main_workloads:
        generic = best_matching(all_rows, workload, "generic_o1_like", lut_margin=0.95)
        full = best_matching(all_rows, workload, "full_only_static")
        proto = best_matching(all_rows, workload, "block_ab_pingpong_analysis", lut_margin=0.95)
        lines.append(
            f"| `{workload}` | {row_summary(generic)} | {row_summary(full)} | {row_summary(proto)} |"
        )

    lines += [
        "",
        "## Interpretation",
        "",
        "- `O9a_square128_block128` is the most important next case.  It changes only",
        "  the compile-time tile/block shape and does not depend on DATAFLOW.",
        "- `O9b_square224_block224_generic` may be BRAM-heavy, but it tests the same",
        "  idea on 224 without relying on full-only specialization.",
        "- `O9c_square224_fullonly_row2` is a specialized fixed-shape path, not a",
        "  replacement for the generic DDR-facing baseline.",
        "- `D1_qkv_block_ab_pingpong_small` should only be used to read the HLS",
        "  DATAFLOW report and resource growth.  It should not be promoted to",
        "  TILE=14/BLOCK=112 or full square GEMM until overlap is proven.",
        "",
        "## Stop Criteria",
        "",
        "- If `O9a` exceeds `53,200` LUT or fails timing badly, do not expand the",
        "  block-size route without a new model correction.",
        "- If `D1` does not show real load/compute overlap in the HLS report, stop",
        "  Route D implementation work and keep it as analysis only.",
        "- If any candidate duplicates DSP from about 196 to about 392, mark it",
        "  exploration-only immediately.",
    ]
    return "\n".join(lines) + "\n"


def try_import_matplotlib():
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from mpl_toolkits.mplot3d import Axes3D  # noqa: F401

        return plt
    except Exception:
        return None


def aggregate_min(rows: list[dict[str, float | int | str]], x_key: str, y_key: str, z_key: str):
    xs = sorted({r[x_key] for r in rows}, key=lambda v: float(v))
    ys = sorted({r[y_key] for r in rows}, key=lambda v: float(v))
    table = [[math.nan for _ in xs] for _ in ys]
    index_x = {v: i for i, v in enumerate(xs)}
    index_y = {v: i for i, v in enumerate(ys)}
    for r in rows:
        i = index_y[r[y_key]]
        j = index_x[r[x_key]]
        val = float(r[z_key])
        if math.isnan(table[i][j]) or val < table[i][j]:
            table[i][j] = val
    return xs, ys, table


def save_heatmap(plt, rows, workload, x_key, y_key, z_key, title, file_name):
    if not rows:
        return
    xs, ys, table = aggregate_min(rows, x_key, y_key, z_key)
    fig, ax = plt.subplots(figsize=(8, 5))
    im = ax.imshow(table, aspect="auto", origin="lower")
    ax.set_xticks(range(len(xs)))
    ax.set_xticklabels([str(x) for x in xs], rotation=45, ha="right")
    ax.set_yticks(range(len(ys)))
    ax.set_yticklabels([str(y) for y in ys])
    ax.set_xlabel(x_key)
    ax.set_ylabel(y_key)
    ax.set_title(f"{workload}: {title}")
    fig.colorbar(im, ax=ax, label=z_key)
    fig.tight_layout()
    fig.savefig(FIGURE_DIR / file_name, dpi=140)
    plt.close(fig)


def svg_escape(value: object) -> str:
    text = str(value)
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def svg_write(path: Path, width: int, height: int, elements: list[str]) -> None:
    path.write_text(
        "\n".join(
            [
                f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
                '<rect width="100%" height="100%" fill="white"/>',
                *elements,
                "</svg>",
            ]
        ),
        encoding="utf-8",
    )


def svg_range(values: list[float]) -> tuple[float, float]:
    lo = min(values) if values else 0.0
    hi = max(values) if values else 1.0
    if lo == hi:
        pad = abs(lo) * 0.05 + 1.0
        return lo - pad, hi + pad
    pad = (hi - lo) * 0.05
    return lo - pad, hi + pad


def svg_sample(rows: list[dict[str, float | int | str]], max_points: int = 2500):
    if len(rows) <= max_points:
        return rows
    step = max(1, ceil_div(len(rows), max_points))
    return rows[::step]


def svg_scatter(
    rows: list[dict[str, float | int | str]],
    path: Path,
    title: str,
    x_key: str,
    y_key: str,
    color_key: str | None = None,
    max_points: int = 2500,
) -> None:
    if not rows:
        return
    width, height = 760, 520
    left, right, top, bottom = 86, 28, 48, 72
    xs_all = [float(r[x_key]) for r in rows]
    ys_all = [float(r[y_key]) for r in rows]
    x0, x1 = svg_range(xs_all)
    y0, y1 = svg_range(ys_all)
    plot_w = width - left - right
    plot_h = height - top - bottom

    def sx(v: float) -> float:
        return left + (v - x0) / (x1 - x0) * plot_w

    def sy(v: float) -> float:
        return top + plot_h - (v - y0) / (y1 - y0) * plot_h

    palette = ["#2563eb", "#059669", "#d97706", "#7c3aed", "#dc2626", "#0891b2"]
    elements = [
        f'<text x="{width/2}" y="24" text-anchor="middle" font-size="18" font-family="Arial" font-weight="700">{svg_escape(title)}</text>',
        f'<line x1="{left}" y1="{top+plot_h}" x2="{left+plot_w}" y2="{top+plot_h}" stroke="#222" stroke-width="1"/>',
        f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top+plot_h}" stroke="#222" stroke-width="1"/>',
        f'<text x="{width/2}" y="{height-20}" text-anchor="middle" font-size="12" font-family="Arial">{svg_escape(x_key)}</text>',
        f'<text x="18" y="{height/2}" text-anchor="middle" font-size="12" font-family="Arial" transform="rotate(-90 18 {height/2})">{svg_escape(y_key)}</text>',
        f'<text x="{left}" y="{top+plot_h+20}" font-size="10" font-family="Arial">{x0:.1f}</text>',
        f'<text x="{left+plot_w}" y="{top+plot_h+20}" text-anchor="end" font-size="10" font-family="Arial">{x1:.1f}</text>',
        f'<text x="{left-8}" y="{top+plot_h}" text-anchor="end" font-size="10" font-family="Arial">{y0:.1f}</text>',
        f'<text x="{left-8}" y="{top+4}" text-anchor="end" font-size="10" font-family="Arial">{y1:.1f}</text>',
    ]
    for row in svg_sample(rows, max_points=max_points):
        if color_key:
            color = palette[int(float(row[color_key])) % len(palette)]
        else:
            color = "#059669" if int(row["resource_feasible"]) else "#9ca3af"
        elements.append(
            f'<circle cx="{sx(float(row[x_key])):.2f}" cy="{sy(float(row[y_key])):.2f}" r="2.4" fill="{color}" opacity="0.45"/>'
        )
    svg_write(path, width, height, elements)


def svg_bar(
    rows: list[dict[str, float | int | str]],
    path: Path,
    title: str,
    label_key: str,
    value_key: str,
    max_rows: int = 16,
) -> None:
    if not rows:
        return
    rows = rows[:max_rows]
    width, height = 900, 520
    left, right, top, bottom = 92, 28, 50, 130
    plot_w = width - left - right
    plot_h = height - top - bottom
    max_value = max(float(r[value_key]) for r in rows) or 1.0
    bar_w = plot_w / max(len(rows), 1) * 0.72
    gap = plot_w / max(len(rows), 1)
    elements = [
        f'<text x="{width/2}" y="24" text-anchor="middle" font-size="18" font-family="Arial" font-weight="700">{svg_escape(title)}</text>',
        f'<line x1="{left}" y1="{top+plot_h}" x2="{left+plot_w}" y2="{top+plot_h}" stroke="#222" stroke-width="1"/>',
        f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top+plot_h}" stroke="#222" stroke-width="1"/>',
        f'<text x="18" y="{height/2}" text-anchor="middle" font-size="12" font-family="Arial" transform="rotate(-90 18 {height/2})">{svg_escape(value_key)}</text>',
    ]
    for i, row in enumerate(rows):
        value = float(row[value_key])
        x = left + i * gap + (gap - bar_w) / 2
        h = value / max_value * plot_h
        y = top + plot_h - h
        label = svg_escape(row[label_key])
        elements.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{bar_w:.2f}" height="{h:.2f}" fill="#2563eb" opacity="0.78"/>')
        elements.append(f'<text x="{x+bar_w/2:.2f}" y="{y-4:.2f}" text-anchor="middle" font-size="9" font-family="Arial">{value:.0f}</text>')
        elements.append(
            f'<text x="{x+bar_w/2:.2f}" y="{top+plot_h+14}" text-anchor="end" font-size="10" font-family="Arial" transform="rotate(-45 {x+bar_w/2:.2f} {top+plot_h+14})">{label}</text>'
        )
    svg_write(path, width, height, elements)


def svg_penalty_breakdown(all_rows: list[dict[str, float | int | str]], metric: str, path: Path) -> None:
    row = next(
        (
            r
            for r in all_rows
            if r["workload"] == "square_224"
            and r["arch"] == "generic_o1_like"
            and int(r["tile"]) == 14
            and int(r["block_n"]) == 112
            and int(r["block_k"]) == 112
            and int(r["block_m"]) == 112
            and int(r["row_unroll"]) == 1
            and int(r["input_bits"]) == 8
        ),
        None,
    )
    if row is None:
        return
    if metric == "lut":
        keys = [
            "boundary_check_lut_penalty",
            "address_mux_lut_penalty",
            "generic_tail_lut_penalty",
            "fallback_path_lut_penalty",
        ]
    else:
        keys = [
            "boundary_check_ff_penalty",
            "address_mux_ff_penalty",
            "generic_tail_ff_penalty",
            "fallback_path_ff_penalty",
        ]
    rows = [{"label": k.replace("_penalty", ""), "value": float(row[k])} for k in keys]
    svg_bar(rows, path, f"224 generic {metric.upper()} penalty breakdown", "label", "value", max_rows=len(rows))


def save_svg_figures(all_rows: list[dict[str, float | int | str]], pareto_rows) -> str:
    FIGURE_DIR.mkdir(parents=True, exist_ok=True)
    for workload in sorted({str(r["workload"]) for r in all_rows}):
        rows = [r for r in all_rows if r["workload"] == workload and int(r["input_bits"]) == 8]
        safe_name = sanitize(workload)
        svg_scatter(
            rows,
            FIGURE_DIR / f"{safe_name}_scatter_latency_lut.svg",
            f"{workload}: latency vs LUT",
            "lut_est",
            "latency_cycles_est",
            color_key="tile",
        )
        svg_scatter(
            rows,
            FIGURE_DIR / f"{safe_name}_roofline_scatter.svg",
            f"{workload}: roofline-style scatter",
            "external_ctc_mac_per_byte",
            "actual_like_mac_per_cycle_est",
            color_key="tile",
        )
        wr_pareto = [r for r in pareto_rows if r["workload"] == workload]
        svg_scatter(
            wr_pareto or rows,
            FIGURE_DIR / f"{safe_name}_pareto_latency_lut.svg",
            f"{workload}: Pareto latency vs LUT",
            "lut_est",
            "latency_cycles_est",
        )

    best_rows = [r for r in select_top_candidates(all_rows) if r["candidate_category"] == "best_latency_feasible"]
    svg_bar(
        best_rows,
        FIGURE_DIR / "all_workloads_best_feasible_latency.svg",
        "Best feasible latency by workload",
        "workload",
        "latency_cycles_est",
        max_rows=len(best_rows),
    )
    feasible_all = [r for r in all_rows if int(r["resource_feasible"])]
    svg_scatter(
        feasible_all,
        FIGURE_DIR / "all_workloads_resource_performance_pareto.svg",
        "Global feasible resource-performance scatter",
        "lut_est",
        "latency_cycles_est",
    )
    svg_penalty_breakdown(all_rows, "lut", FIGURE_DIR / "square_224_generic_lut_penalty_breakdown.svg")
    svg_penalty_breakdown(all_rows, "ff", FIGURE_DIR / "square_224_generic_ff_penalty_breakdown.svg")
    return "generated SVG fallback (matplotlib unavailable)"


def save_figures(all_rows: list[dict[str, float | int | str]], pareto_rows) -> str:
    plt = try_import_matplotlib()
    if plt is None:
        return save_svg_figures(all_rows, pareto_rows)
    FIGURE_DIR.mkdir(parents=True, exist_ok=True)
    for workload in sorted({str(r["workload"]) for r in all_rows}):
        rows = [r for r in all_rows if r["workload"] == workload and r["input_bits"] == 8]
        feasible = [r for r in rows if int(r["resource_feasible"])] or rows
        safe_name = sanitize(workload)

        save_heatmap(
            plt,
            feasible,
            workload,
            "tile",
            "block_k",
            "latency_cycles_est",
            "tile x block_k latency",
            f"{safe_name}_heatmap_tile_blockk_latency.png",
        )
        for r in feasible:
            r["block_nm_avg"] = int((int(r["block_n"]) + int(r["block_m"])) / 2)
        save_heatmap(
            plt,
            feasible,
            workload,
            "tile",
            "block_nm_avg",
            "latency_cycles_est",
            "tile x block_n/block_m latency",
            f"{safe_name}_heatmap_tile_blocknm_latency.png",
        )
        save_heatmap(
            plt,
            feasible,
            workload,
            "tile",
            "row_unroll",
            "latency_cycles_est",
            "tile x row_unroll latency",
            f"{safe_name}_heatmap_tile_row_latency.png",
        )
        save_heatmap(
            plt,
            feasible,
            workload,
            "tile",
            "row_unroll",
            "lut_est",
            "tile x row_unroll LUT",
            f"{safe_name}_heatmap_tile_row_lut.png",
        )
        for r in feasible:
            r["tail_ratio_bin"] = round(float(r["tail_ratio"]), 2)
        save_heatmap(
            plt,
            feasible,
            workload,
            "tail_ratio_bin",
            "tile",
            "latency_cycles_est",
            "tail_ratio x tile latency",
            f"{safe_name}_heatmap_tail_latency.png",
        )

        fig, ax = plt.subplots(figsize=(7, 5))
        sc = ax.scatter(
            [float(r["lut_est"]) for r in rows],
            [float(r["latency_cycles_est"]) for r in rows],
            s=[max(8, float(r["bram18_est"]) * 0.8) for r in rows],
            c=[float(r["tile"]) for r in rows],
            alpha=0.45,
        )
        ax.set_xlabel("LUT estimate")
        ax.set_ylabel("Latency cycles estimate")
        ax.set_title(f"{workload}: latency vs LUT")
        fig.colorbar(sc, ax=ax, label="tile")
        fig.tight_layout()
        fig.savefig(FIGURE_DIR / f"{safe_name}_scatter_latency_lut.png", dpi=140)
        plt.close(fig)

        fig, ax = plt.subplots(figsize=(7, 5))
        sc = ax.scatter(
            [float(r["bram18_est"]) for r in rows],
            [float(r["latency_cycles_est"]) for r in rows],
            s=[max(8, float(r["lut_est"]) / 1500.0) for r in rows],
            c=[float(r["row_unroll"]) for r in rows],
            alpha=0.45,
        )
        ax.set_xlabel("BRAM18 estimate")
        ax.set_ylabel("Latency cycles estimate")
        ax.set_title(f"{workload}: latency vs BRAM")
        fig.colorbar(sc, ax=ax, label="row_unroll")
        fig.tight_layout()
        fig.savefig(FIGURE_DIR / f"{safe_name}_scatter_latency_bram.png", dpi=140)
        plt.close(fig)

        wr_pareto = [r for r in pareto_rows if r["workload"] == workload]
        fig, ax = plt.subplots(figsize=(7, 5))
        ax.scatter([float(r["lut_est"]) for r in rows], [float(r["latency_cycles_est"]) for r in rows], s=6, alpha=0.20)
        ax.plot(
            [float(r["lut_est"]) for r in wr_pareto],
            [float(r["latency_cycles_est"]) for r in wr_pareto],
            marker="o",
            color="red",
            linewidth=1,
        )
        ax.set_xlabel("LUT estimate")
        ax.set_ylabel("Latency cycles estimate")
        ax.set_title(f"{workload}: Pareto latency vs LUT")
        fig.tight_layout()
        fig.savefig(FIGURE_DIR / f"{safe_name}_pareto_latency_lut.png", dpi=140)
        plt.close(fig)

        sample = feasible[: min(len(feasible), 6000)]
        fig = plt.figure(figsize=(7, 5))
        ax = fig.add_subplot(111, projection="3d")
        ax.scatter(
            [float(r["tile"]) for r in sample],
            [float(r["block_k"]) for r in sample],
            [float(r["latency_cycles_est"]) for r in sample],
            s=5,
            alpha=0.35,
        )
        ax.set_xlabel("tile")
        ax.set_ylabel("block_k")
        ax.set_zlabel("latency")
        ax.set_title(f"{workload}: tile, block_k, latency")
        fig.tight_layout()
        fig.savefig(FIGURE_DIR / f"{safe_name}_3d_tile_blockk_latency.png", dpi=140)
        plt.close(fig)

        fig = plt.figure(figsize=(7, 5))
        ax = fig.add_subplot(111, projection="3d")
        ax.scatter(
            [float(r["tile"]) for r in sample],
            [float(r["block_nm_avg"]) for r in sample],
            [float(r["latency_cycles_est"]) for r in sample],
            s=5,
            alpha=0.35,
        )
        ax.set_xlabel("tile")
        ax.set_ylabel("avg block_n/m")
        ax.set_zlabel("latency")
        ax.set_title(f"{workload}: tile, block_n/block_m, latency")
        fig.tight_layout()
        fig.savefig(FIGURE_DIR / f"{safe_name}_3d_tile_blocknm_latency.png", dpi=140)
        plt.close(fig)

        fig, ax = plt.subplots(figsize=(7, 5))
        sc = ax.scatter(
            [float(r["external_ctc_mac_per_byte"]) for r in rows],
            [float(r["actual_like_mac_per_cycle_est"]) for r in rows],
            c=[float(r["tile"]) for r in rows],
            s=8,
            alpha=0.4,
        )
        ax.set_xlabel("External CTC (MAC/byte)")
        ax.set_ylabel("Actual-like MAC/cycle")
        ax.set_title(f"{workload}: roofline-style scatter")
        fig.colorbar(sc, ax=ax, label="tile")
        fig.tight_layout()
        fig.savefig(FIGURE_DIR / f"{safe_name}_roofline_scatter.png", dpi=140)
        plt.close(fig)

    best_rows = [r for r in select_top_candidates(all_rows) if r["candidate_category"] == "best_latency_feasible"]
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.bar([str(r["workload"]) for r in best_rows], [float(r["latency_cycles_est"]) for r in best_rows])
    ax.set_ylabel("Best feasible latency")
    ax.set_title("Best feasible latency by workload")
    ax.tick_params(axis="x", rotation=60)
    fig.tight_layout()
    fig.savefig(FIGURE_DIR / "all_workloads_best_feasible_latency.png", dpi=140)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(8, 5))
    feasible_all = [r for r in all_rows if int(r["resource_feasible"])]
    ax.scatter(
        [float(r["lut_est"]) for r in feasible_all],
        [float(r["latency_cycles_est"]) for r in feasible_all],
        s=[max(6, float(r["bram18_est"]) * 0.4) for r in feasible_all],
        alpha=0.25,
    )
    ax.set_xlabel("LUT estimate")
    ax.set_ylabel("Latency cycles estimate")
    ax.set_title("Global feasible resource-performance scatter")
    fig.tight_layout()
    fig.savefig(FIGURE_DIR / "all_workloads_resource_performance_pareto.png", dpi=140)
    plt.close(fig)
    return "generated"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="GEMM scheduler variable design-space model v2")
    parser.add_argument("--full-block-scan", action="store_true", help="scan all fixed block candidates instead of compact subset")
    parser.add_argument(
        "--compact-block-candidates",
        type=int,
        default=DEFAULT_COMPACT_BLOCK_CANDIDATES,
        help="number of scored block candidates per N/K/M dimension in compact mode",
    )
    parser.add_argument(
        "--bram18-limit",
        type=int,
        default=BRAM18_LIMIT_ZYNQ7020,
        help="BRAM18K resource limit; ZYNQ-7020 default is 280, equivalent to 140 BRAM36K",
    )
    parser.add_argument(
        "--conservative-bram",
        action="store_true",
        help="deprecated compatibility flag; no longer halves the BRAM18K limit",
    )
    parser.add_argument("--no-figures", action="store_true", help="skip matplotlib figure generation")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    bram18_limit = args.bram18_limit
    measured = read_measured_points(EXPERIMENTS_CSV)
    cal = build_calibration(measured)

    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    all_rows: list[dict[str, float | int | str]] = []
    designs = generate_designs(
        WORKLOADS,
        bram18_limit,
        compact_blocks=not args.full_block_scan,
        compact_block_candidates=args.compact_block_candidates,
    )
    for design in designs:
        all_rows.append(evaluate_design(design, cal, bram18_limit))

    top_rows = select_top_candidates(all_rows)
    pareto_rows = pareto_frontier(all_rows)
    summary_rows = workload_summary(top_rows)

    write_csv(REPORT_DIR / "all_points.csv", all_rows)
    write_csv(REPORT_DIR / "top_candidates.csv", top_rows)
    write_csv(REPORT_DIR / "pareto_candidates.csv", pareto_rows)
    write_csv(REPORT_DIR / "workload_summary.csv", summary_rows)

    if args.no_figures:
        figure_status = "skipped by --no-figures"
    else:
        figure_status = save_figures(all_rows, pareto_rows)

    (REPORT_DIR / "model_assumptions.md").write_text(
        assumptions_md(
            cal,
            bram18_limit,
            compact=not args.full_block_scan,
            compact_block_candidates=args.compact_block_candidates,
        ),
        encoding="utf-8",
    )
    (REPORT_DIR / "summary.md").write_text(
        summary_md(all_rows, top_rows, pareto_rows, figure_status),
        encoding="utf-8",
    )
    (REPORT_DIR / "optimal_parameter_recommendations.md").write_text(
        optimal_recommendations_md(all_rows),
        encoding="utf-8",
    )

    print(f"Wrote {REPORT_DIR / 'all_points.csv'} ({len(all_rows)} points)")
    print(f"Wrote {REPORT_DIR / 'top_candidates.csv'}")
    print(f"Wrote {REPORT_DIR / 'pareto_candidates.csv'}")
    print(f"Wrote {REPORT_DIR / 'workload_summary.csv'}")
    print(f"Wrote {REPORT_DIR / 'model_assumptions.md'}")
    print(f"Wrote {REPORT_DIR / 'summary.md'}")
    print(f"Wrote {REPORT_DIR / 'optimal_parameter_recommendations.md'}")
    print(f"Figure status: {figure_status}")


if __name__ == "__main__":
    main()
