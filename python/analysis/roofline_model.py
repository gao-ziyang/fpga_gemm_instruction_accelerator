#!/usr/bin/env python3
"""Roofline-style model for the GEMM scheduler experiments.

This script is deliberately dependency-free.  It does not try to replace HLS
reports; it turns HLS latency/resource numbers into a clearer set of roofline,
internal scheduler, and resource-efficiency metrics.
"""

from __future__ import annotations

import csv
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[2]
INPUT_CSV = ROOT / "python" / "analysis" / "roofline_experiments.csv"
REPORT_DIR = ROOT / "reports"
POINTS_CSV = REPORT_DIR / "internal_roofline_points.csv"
SUMMARY_MD = REPORT_DIR / "internal_roofline_summary.md"
IDEAL_POINTS_CSV = REPORT_DIR / "ideal_lower_bound_points.csv"
IDEAL_SUMMARY_MD = REPORT_DIR / "ideal_lower_bound_summary.md"
HLS_LOOP_POINTS_CSV = REPORT_DIR / "hls_loop_schedule_points.csv"
HLS_LOOP_SUMMARY_MD = REPORT_DIR / "hls_loop_schedule_summary.md"
COMBINED_POINTS_CSV = REPORT_DIR / "combined_roofline_points.csv"
COMBINED_SUMMARY_MD = REPORT_DIR / "combined_roofline_summary.md"

INT8_BYTES = 1
INT32_BYTES = 4
DEFAULT_FREQ_MHZ = 100.0
DEFAULT_DDR_MB_PER_S = 800.0
ZYNQ7020_LUT_LIMIT = 53200


@dataclass(frozen=True)
class DesignPoint:
    name: str
    note: str
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
    estimated_clock_ns: float
    c_traffic_mode: str = "write_only"
    freq_mhz: float = DEFAULT_FREQ_MHZ
    ddr_mb_per_s: float = DEFAULT_DDR_MB_PER_S
    ddr_efficiency: float = 1.0
    axi_burst_bytes: int = 64
    axi_burst_latency_cycles: int = 0
    overlap_ext_load_compute: bool = False
    overlap_store_compute: bool = False
    external_ab_combined: bool = True
    load_a_ii: int = 1
    load_b_ii: int = 1
    load_ab_ii: int = 1
    store_c_ii: int = 1
    local_ab_combined: bool = False
    local_a_ii: int = 1
    local_b_ii: int = 1
    local_ab_ii: int = 1
    local_c_read_ii: int = 1
    local_c_write_ii: int = 1
    compute_ii: int = 1
    pipeline_fill_drain_cycles: int = 0
    function_call_overhead_cycles: int = 0
    address_mux_overhead_per_output_tile: int = 0
    boundary_overhead_per_tail_output_tile: int = 0
    bram_conflict_penalty_per_output_tile: int = 0


def ceil_div(a: int, b: int) -> int:
    return (a + b - 1) // b


def safe_div(numerator: float | int, denominator: float | int) -> float:
    return float(numerator) / float(denominator) if denominator else 0.0


def as_bool(value: str | bool | int) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        return value != 0
    return value.strip().lower() in {"1", "true", "yes", "y", "on"}


def get(row: dict[str, str], key: str, default: str) -> str:
    value = row.get(key, "")
    return default if value == "" else value


def load_points(path: Path) -> list[DesignPoint]:
    with path.open("r", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))

    points: list[DesignPoint] = []
    for row in rows:
        points.append(
            DesignPoint(
                name=get(row, "name", ""),
                note=get(row, "note", ""),
                n=int(get(row, "n", "0")),
                k=int(get(row, "k", "0")),
                m=int(get(row, "m", "0")),
                tile=int(get(row, "tile", "1")),
                block_n=int(get(row, "block_n", "1")),
                block_k=int(get(row, "block_k", "1")),
                block_m=int(get(row, "block_m", "1")),
                row_unroll=int(get(row, "row_unroll", "1")),
                latency_cycles=int(get(row, "latency_cycles", "0")),
                bram18k=int(get(row, "bram18k", "0")),
                dsp=int(get(row, "dsp", "0")),
                ff=int(get(row, "ff", "0")),
                lut=int(get(row, "lut", "0")),
                estimated_clock_ns=float(get(row, "estimated_clock_ns", "0")),
                c_traffic_mode=get(row, "c_traffic_mode", "write_only"),
                freq_mhz=float(get(row, "freq_mhz", str(DEFAULT_FREQ_MHZ))),
                ddr_mb_per_s=float(get(row, "ddr_mb_per_s", str(DEFAULT_DDR_MB_PER_S))),
                ddr_efficiency=float(get(row, "ddr_efficiency", "1.0")),
                axi_burst_bytes=int(get(row, "axi_burst_bytes", "64")),
                axi_burst_latency_cycles=int(get(row, "axi_burst_latency_cycles", "0")),
                overlap_ext_load_compute=as_bool(get(row, "overlap_ext_load_compute", "0")),
                overlap_store_compute=as_bool(get(row, "overlap_store_compute", "0")),
                external_ab_combined=as_bool(get(row, "external_ab_combined", "1")),
                load_a_ii=int(get(row, "load_a_ii", "1")),
                load_b_ii=int(get(row, "load_b_ii", "1")),
                load_ab_ii=int(get(row, "load_ab_ii", "1")),
                store_c_ii=int(get(row, "store_c_ii", "1")),
                local_ab_combined=as_bool(get(row, "local_ab_combined", "0")),
                local_a_ii=int(get(row, "local_a_ii", "1")),
                local_b_ii=int(get(row, "local_b_ii", "1")),
                local_ab_ii=int(get(row, "local_ab_ii", "1")),
                local_c_read_ii=int(get(row, "local_c_read_ii", "1")),
                local_c_write_ii=int(get(row, "local_c_write_ii", "1")),
                compute_ii=int(get(row, "compute_ii", "1")),
                pipeline_fill_drain_cycles=int(get(row, "pipeline_fill_drain_cycles", "0")),
                function_call_overhead_cycles=int(get(row, "function_call_overhead_cycles", "0")),
                address_mux_overhead_per_output_tile=int(get(row, "address_mux_overhead_per_output_tile", "0")),
                boundary_overhead_per_tail_output_tile=int(get(row, "boundary_overhead_per_tail_output_tile", "0")),
                bram_conflict_penalty_per_output_tile=int(get(row, "bram_conflict_penalty_per_output_tile", "0")),
            )
        )
    return points


def block_sizes(total: int, block: int) -> list[int]:
    return [min(block, total - start) for start in range(0, total, block)]


def c_traffic_bytes(p: DesignPoint) -> int:
    c_base = p.n * p.m * INT32_BYTES
    if p.c_traffic_mode == "write_only":
        return c_base
    if p.c_traffic_mode == "read_write":
        return 2 * c_base
    if p.c_traffic_mode == "per_k_block_read_write":
        return 2 * ceil_div(p.k, p.block_k) * c_base
    raise ValueError(f"unknown c_traffic_mode: {p.c_traffic_mode}")


def external_traffic_bytes(p: DesignPoint) -> tuple[int, int, int, int]:
    # Current scheduler order reloads A for each output-column block and B for
    # each output-row block.
    a_bytes = ceil_div(p.m, p.block_m) * p.n * p.k * INT8_BYTES
    b_bytes = ceil_div(p.n, p.block_n) * p.k * p.m * INT8_BYTES
    c_bytes = c_traffic_bytes(p)
    return a_bytes, b_bytes, c_bytes, a_bytes + b_bytes + c_bytes


def burst_cycles(bytes_count: int, bytes_per_cycle: float, burst_bytes: int, burst_latency: int) -> float:
    if bytes_count <= 0:
        return 0.0
    payload_cycles = bytes_count / bytes_per_cycle
    burst_count = ceil_div(bytes_count, burst_bytes)
    return payload_cycles + burst_count * burst_latency


def local_load_cycles(base_rows: int, ii: int, fill: int) -> float:
    return base_rows * ii + fill


def compute_cycles(p: DesignPoint) -> float:
    # gemm_core_mac pipelines the K dimension of a TILE x TILE local multiply.
    return p.tile * p.compute_ii + p.pipeline_fill_drain_cycles


def internal_scheduler_estimate(p: DesignPoint) -> dict[str, float | int]:
    n_blocks = block_sizes(p.n, p.block_n)
    m_blocks = block_sizes(p.m, p.block_m)
    k_blocks = block_sizes(p.k, p.block_k)

    base_rows = ceil_div(p.tile, p.row_unroll)
    local_c_load = local_load_cycles(base_rows, p.local_c_read_ii, p.pipeline_fill_drain_cycles)
    local_c_store = local_load_cycles(base_rows, p.local_c_write_ii, p.pipeline_fill_drain_cycles)

    if p.local_ab_combined:
        local_ab_load = local_load_cycles(base_rows, p.local_ab_ii, p.pipeline_fill_drain_cycles)
        local_a_load = local_ab_load
        local_b_load = 0.0
    else:
        local_a_load = local_load_cycles(base_rows, p.local_a_ii, p.pipeline_fill_drain_cycles)
        local_b_load = local_load_cycles(base_rows, p.local_b_ii, p.pipeline_fill_drain_cycles)

    mac_compute = compute_cycles(p)

    internal_cycles = 0.0
    compute_only_cycles = 0.0
    full_output_tiles = 0
    tail_output_tiles = 0
    compute_block_calls = 0

    for cur_n in n_blocks:
        for cur_m in m_blocks:
            out_tiles = ceil_div(cur_n, p.tile) * ceil_div(cur_m, p.tile)
            for cur_k in k_blocks:
                tile_k_count = ceil_div(cur_k, p.tile)
                is_full_block = cur_n == p.block_n and cur_m == p.block_m and cur_k == p.block_k
                if is_full_block:
                    full_output_tiles += out_tiles
                else:
                    tail_output_tiles += out_tiles

                per_tile = (
                    local_c_load
                    + tile_k_count * (local_a_load + local_b_load + mac_compute)
                    + local_c_store
                    + p.address_mux_overhead_per_output_tile
                    + p.bram_conflict_penalty_per_output_tile
                )
                if not is_full_block:
                    per_tile += p.boundary_overhead_per_tail_output_tile

                internal_cycles += out_tiles * per_tile
                compute_only_cycles += out_tiles * tile_k_count * mac_compute
                compute_block_calls += 1

    internal_cycles += compute_block_calls * p.function_call_overhead_cycles
    modeled_mac_active_ratio = compute_only_cycles / internal_cycles if internal_cycles else 0.0

    return {
        "n_block_count": len(n_blocks),
        "m_block_count": len(m_blocks),
        "k_block_count": len(k_blocks),
        "compute_block_calls": compute_block_calls,
        "full_output_tile_count": full_output_tiles,
        "tail_output_tile_count": tail_output_tiles,
        "local_a_load_cycles_per_tile": local_a_load,
        "local_b_load_cycles_per_tile": local_b_load,
        "local_c_load_cycles_per_tile": local_c_load,
        "local_c_store_cycles_per_tile": local_c_store,
        "compute_cycles_per_k_tile": mac_compute,
        "internal_model_cycles": internal_cycles,
        "internal_compute_only_cycles": compute_only_cycles,
        "modeled_mac_active_ratio": modeled_mac_active_ratio,
    }


def common_result_fields(p: DesignPoint) -> dict[str, float | int | str]:
    macs = p.n * p.k * p.m
    actual_mac_per_cycle = safe_div(macs, p.latency_cycles)
    actual_ops_per_cycle = 2 * actual_mac_per_cycle
    actual_gmac_s = actual_mac_per_cycle * p.freq_mhz / 1000.0
    actual_gops = actual_gmac_s * 2

    return {
        "name": p.name,
        "note": p.note,
        "N": p.n,
        "K": p.k,
        "M": p.m,
        "tile": p.tile,
        "block_N": p.block_n,
        "block_K": p.block_k,
        "block_M": p.block_m,
        "row_unroll": p.row_unroll,
        "latency_cycles": p.latency_cycles,
        "actual_mac_per_cycle": actual_mac_per_cycle,
        "actual_ops_per_cycle": actual_ops_per_cycle,
        "actual_gmac_s_at_freq": actual_gmac_s,
        "actual_gops_at_freq": actual_gops,
        "bram18k": p.bram18k,
        "dsp": p.dsp,
        "ff": p.ff,
        "lut": p.lut,
        "estimated_clock_ns": p.estimated_clock_ns,
    }


def block_tile_counts(p: DesignPoint) -> dict[str, int]:
    return {
        "n_blk": ceil_div(p.n, p.block_n),
        "m_blk": ceil_div(p.m, p.block_m),
        "k_blk": ceil_div(p.k, p.block_k),
        "tile_n": ceil_div(p.block_n, p.tile),
        "tile_m": ceil_div(p.block_m, p.tile),
        "tile_k": ceil_div(p.block_k, p.tile),
    }


def ideal_lower_bound_metrics(p: DesignPoint) -> dict[str, float | int | str]:
    """Optimistic lower-bound model.

    This intentionally does not model the current HLS element-level loops. It is
    useful as a roofline-style lower bound for DDR bandwidth and ideal compute.
    """

    macs = p.n * p.k * p.m
    a_bytes, b_bytes, c_bytes, external_bytes = external_traffic_bytes(p)
    ddr_bytes_per_cycle = (p.ddr_mb_per_s / p.freq_mhz) * p.ddr_efficiency

    ideal_ext_load_cycles = safe_div(a_bytes + b_bytes, ddr_bytes_per_cycle)
    ideal_ext_store_cycles = safe_div(c_bytes, ddr_bytes_per_cycle)
    ideal_external_total_cycles = safe_div(external_bytes, ddr_bytes_per_cycle)

    compute_roof_mac_per_cycle = p.tile * p.tile
    ideal_compute_cycles = safe_div(macs, compute_roof_mac_per_cycle)

    counts = block_tile_counts(p)
    ideal_local_load_cycles = 0.0
    ideal_local_store_cycles = 0.0
    ideal_local_c_load_per_output_tile = 1
    ideal_local_a_load_per_k_tile = 1
    ideal_local_b_load_per_k_tile = 1
    ideal_local_c_store_per_output_tile = 1
    ideal_compute_per_k_tile = p.tile * p.compute_ii + p.pipeline_fill_drain_cycles

    ideal_internal_tile_cycles = (
        ideal_local_c_load_per_output_tile
        + counts["tile_k"] * (
            ideal_local_a_load_per_k_tile
            + ideal_local_b_load_per_k_tile
            + ideal_compute_per_k_tile
        )
        + ideal_local_c_store_per_output_tile
    )
    ideal_internal_cycles = (
        counts["n_blk"]
        * counts["m_blk"]
        * counts["k_blk"]
        * counts["tile_n"]
        * counts["tile_m"]
        * ideal_internal_tile_cycles
    )

    ideal_no_overlap_cycles = (
        ideal_ext_load_cycles
        + ideal_internal_cycles
        + ideal_ext_store_cycles
    )
    ideal_roofline_cycles = max(ideal_external_total_cycles, ideal_compute_cycles)

    return {
        **common_result_fields(p),
        "model_kind": "ideal_lower_bound_model",
        "bound_note": "optimistic lower bound",
        "macs": macs,
        "a_bytes": a_bytes,
        "b_bytes": b_bytes,
        "c_bytes": c_bytes,
        "external_bytes": external_bytes,
        "ddr_bytes_per_cycle": ddr_bytes_per_cycle,
        "compute_roof_mac_per_cycle": compute_roof_mac_per_cycle,
        **counts,
        "ideal_ext_load_cycles": ideal_ext_load_cycles,
        "ideal_ext_store_cycles": ideal_ext_store_cycles,
        "ideal_external_total_cycles": ideal_external_total_cycles,
        "ideal_compute_cycles": ideal_compute_cycles,
        "ideal_local_load_cycles": ideal_local_load_cycles,
        "ideal_local_store_cycles": ideal_local_store_cycles,
        "ideal_local_c_load_per_output_tile": ideal_local_c_load_per_output_tile,
        "ideal_local_a_load_per_k_tile": ideal_local_a_load_per_k_tile,
        "ideal_local_b_load_per_k_tile": ideal_local_b_load_per_k_tile,
        "ideal_local_c_store_per_output_tile": ideal_local_c_store_per_output_tile,
        "ideal_compute_per_k_tile": ideal_compute_per_k_tile,
        "ideal_internal_tile_cycles": ideal_internal_tile_cycles,
        "ideal_internal_cycles": ideal_internal_cycles,
        "ideal_no_overlap_cycles": ideal_no_overlap_cycles,
        "ideal_roofline_cycles": ideal_roofline_cycles,
        "latency_over_ideal_no_overlap": safe_div(p.latency_cycles, ideal_no_overlap_cycles),
        "latency_over_ideal_roofline": safe_div(p.latency_cycles, ideal_roofline_cycles),
    }


def hls_loop_schedule_metrics(p: DesignPoint) -> dict[str, float | int | str]:
    """Tripcount x II model for the current HLS scheduler loop structure."""

    counts = block_tile_counts(p)
    n_blk = counts["n_blk"]
    m_blk = counts["m_blk"]
    k_blk = counts["k_blk"]
    tile_n = counts["tile_n"]
    tile_m = counts["tile_m"]
    tile_k = counts["tile_k"]

    if p.external_ab_combined:
        t_load_per_block = p.block_k * max(p.block_n, p.block_m) * p.load_ab_ii
    else:
        t_load_a_per_block = p.block_n * p.block_k * p.load_a_ii
        t_load_b_per_block = p.block_k * p.block_m * p.load_b_ii
        t_load_per_block = t_load_a_per_block + t_load_b_per_block

    t_load_ab_block = n_blk * m_blk * k_blk * t_load_per_block

    t_local_c_load = ceil_div(p.tile, p.row_unroll) * p.local_c_read_ii
    t_local_a_load = ceil_div(p.tile, p.row_unroll) * p.local_a_ii
    t_local_b_load = ceil_div(p.tile, p.row_unroll) * p.local_b_ii
    t_local_ab_load = p.tile * p.local_ab_ii if p.local_ab_combined else 0
    t_compute = p.tile * p.compute_ii + p.pipeline_fill_drain_cycles
    t_local_c_store = ceil_div(p.tile, p.row_unroll) * p.local_c_write_ii

    if p.local_ab_combined:
        t_one_output_tile = (
            t_local_c_load
            + tile_k * (t_local_ab_load + t_compute)
            + t_local_c_store
        )
        local_load_store_per_output_tile = (
            t_local_c_load
            + tile_k * t_local_ab_load
            + t_local_c_store
        )
    else:
        t_one_output_tile = (
            t_local_c_load
            + tile_k * (t_local_a_load + t_local_b_load + t_compute)
            + t_local_c_store
        )
        local_load_store_per_output_tile = (
            t_local_c_load
            + tile_k * (t_local_a_load + t_local_b_load)
            + t_local_c_store
        )

    compute_per_output_tile = tile_k * t_compute
    local_load_store_share = safe_div(local_load_store_per_output_tile, t_one_output_tile)

    t_compute_block_internal = (
        n_blk
        * m_blk
        * k_blk
        * tile_n
        * tile_m
        * t_one_output_tile
    )

    t_store_c_block = (
        n_blk
        * m_blk
        * p.block_n
        * p.block_m
        * p.store_c_ii
    )

    t_hls_loop_model = t_load_ab_block + t_compute_block_internal + t_store_c_block
    t_unexplained = p.latency_cycles - t_hls_loop_model
    t_unexplained_ratio = safe_div(t_unexplained, p.latency_cycles)
    t_model_gap = safe_div(p.latency_cycles, t_hls_loop_model)

    positive_control = max(t_unexplained, 0)
    component_cycles = {
        "T_load_AB_block": t_load_ab_block,
        "T_compute_block_internal": t_compute_block_internal,
        "T_store_C_block": t_store_c_block,
        "T_control": positive_control,
    }
    latency_category = max(component_cycles, key=component_cycles.get)
    deployable_status = "not deployable" if p.lut > ZYNQ7020_LUT_LIMIT else "deployable"

    return {
        **common_result_fields(p),
        "model_kind": "hls_loop_schedule_model",
        **counts,
        "external_ab_combined": int(p.external_ab_combined),
        "local_ab_combined": int(p.local_ab_combined),
        "load_a_ii": p.load_a_ii,
        "load_b_ii": p.load_b_ii,
        "load_ab_ii": p.load_ab_ii,
        "store_c_ii": p.store_c_ii,
        "local_a_ii": p.local_a_ii,
        "local_b_ii": p.local_b_ii,
        "local_ab_ii": p.local_ab_ii,
        "local_c_read_ii": p.local_c_read_ii,
        "local_c_write_ii": p.local_c_write_ii,
        "compute_ii": p.compute_ii,
        "pipeline_fill_drain_cycles": p.pipeline_fill_drain_cycles,
        "T_load_per_block": t_load_per_block,
        "T_one_output_tile": t_one_output_tile,
        "T_local_C_load": t_local_c_load,
        "T_local_A_load": t_local_a_load,
        "T_local_B_load": t_local_b_load,
        "T_local_AB_load": t_local_ab_load,
        "T_compute": t_compute,
        "T_local_C_store": t_local_c_store,
        "local_load_store_cycles_per_output_tile": local_load_store_per_output_tile,
        "compute_cycles_per_output_tile": compute_per_output_tile,
        "local_load_store_share_of_output_tile": local_load_store_share,
        "local_load_store_dominated": int(local_load_store_share > 0.5),
        "hls_loop_load_cycles": t_load_ab_block,
        "hls_loop_compute_internal_cycles": t_compute_block_internal,
        "hls_loop_store_cycles": t_store_c_block,
        "hls_loop_total_cycles": t_hls_loop_model,
        "hls_loop_unexplained_cycles": t_unexplained,
        "hls_loop_unexplained_ratio": t_unexplained_ratio,
        "hls_loop_model_gap": t_model_gap,
        "latency_category": latency_category,
        "deployable_status": deployable_status,
    }


def classify_bound(actual_mac_per_cycle: float, attainable_mac_per_cycle: float, compute_roof: float, mem_roof: float) -> str:
    if attainable_mac_per_cycle > 0 and actual_mac_per_cycle < 0.2 * attainable_mac_per_cycle:
        return "internal/scheduler-bound"
    if compute_roof <= mem_roof:
        return "compute-bound"
    return "external-memory-bound"


def metrics(p: DesignPoint) -> dict[str, float | int | str]:
    macs = p.n * p.k * p.m
    ops = 2 * macs
    a_bytes, b_bytes, c_bytes, external_bytes = external_traffic_bytes(p)

    ddr_bytes_per_cycle = (p.ddr_mb_per_s / p.freq_mhz) * p.ddr_efficiency
    external_ctc_ops_per_byte = ops / external_bytes
    external_ctc_mac_per_byte = macs / external_bytes

    compute_roof_mac_per_cycle = p.tile * p.tile
    compute_roof_ops_per_cycle = 2 * compute_roof_mac_per_cycle
    mem_roof_ops_per_cycle = external_ctc_ops_per_byte * ddr_bytes_per_cycle
    mem_roof_mac_per_cycle = mem_roof_ops_per_cycle / 2
    attainable_mac_per_cycle = min(compute_roof_mac_per_cycle, mem_roof_mac_per_cycle)
    attainable_ops_per_cycle = 2 * attainable_mac_per_cycle

    actual_mac_per_cycle = macs / p.latency_cycles
    actual_ops_per_cycle = 2 * actual_mac_per_cycle
    actual_gmac_s = actual_mac_per_cycle * p.freq_mhz / 1000.0
    actual_gops = actual_gmac_s * 2

    compute_peak_util = actual_mac_per_cycle / compute_roof_mac_per_cycle
    attainable_roof_util = actual_mac_per_cycle / attainable_mac_per_cycle

    external_mem_cycles_min = external_bytes / ddr_bytes_per_cycle
    compute_cycles_min = macs / compute_roof_mac_per_cycle
    roof_cycles_min = max(external_mem_cycles_min, compute_cycles_min)
    latency_over_roof_lower_bound = p.latency_cycles / roof_cycles_min

    ext_a_cycles = burst_cycles(a_bytes, ddr_bytes_per_cycle, p.axi_burst_bytes, p.axi_burst_latency_cycles)
    ext_b_cycles = burst_cycles(b_bytes, ddr_bytes_per_cycle, p.axi_burst_bytes, p.axi_burst_latency_cycles)
    ext_c_cycles = burst_cycles(c_bytes, ddr_bytes_per_cycle, p.axi_burst_bytes, p.axi_burst_latency_cycles)
    ext_load_cycles = ext_a_cycles + ext_b_cycles
    internal = internal_scheduler_estimate(p)
    internal_model_cycles = float(internal["internal_model_cycles"])

    if p.overlap_ext_load_compute:
        load_plus_internal = max(ext_load_cycles, internal_model_cycles)
    else:
        load_plus_internal = ext_load_cycles + internal_model_cycles
    total_model_cycles = max(load_plus_internal, ext_c_cycles) if p.overlap_store_compute else load_plus_internal + ext_c_cycles

    local_model_gap = p.latency_cycles / internal_model_cycles if internal_model_cycles else 0.0
    total_model_gap = p.latency_cycles / total_model_cycles if total_model_cycles else 0.0

    gops_per_dsp = actual_gops / p.dsp if p.dsp else 0.0
    gops_per_bram18k = actual_gops / p.bram18k if p.bram18k else 0.0
    gops_per_klut = actual_gops / (p.lut / 1000.0) if p.lut else 0.0

    bound = classify_bound(actual_mac_per_cycle, attainable_mac_per_cycle, compute_roof_mac_per_cycle, mem_roof_mac_per_cycle)

    return {
        "name": p.name,
        "note": p.note,
        "N": p.n,
        "K": p.k,
        "M": p.m,
        "tile": p.tile,
        "block_N": p.block_n,
        "block_K": p.block_k,
        "block_M": p.block_m,
        "row_unroll": p.row_unroll,
        "c_traffic_mode": p.c_traffic_mode,
        "macs": macs,
        "ops": ops,
        "a_bytes": a_bytes,
        "b_bytes": b_bytes,
        "c_bytes": c_bytes,
        "external_bytes": external_bytes,
        "ddr_bytes_per_cycle": ddr_bytes_per_cycle,
        "external_ctc_ops_per_byte": external_ctc_ops_per_byte,
        "external_ctc_mac_per_byte": external_ctc_mac_per_byte,
        "compute_roof_mac_per_cycle": compute_roof_mac_per_cycle,
        "compute_roof_ops_per_cycle": compute_roof_ops_per_cycle,
        "mem_roof_mac_per_cycle": mem_roof_mac_per_cycle,
        "mem_roof_ops_per_cycle": mem_roof_ops_per_cycle,
        "attainable_mac_per_cycle": attainable_mac_per_cycle,
        "attainable_ops_per_cycle": attainable_ops_per_cycle,
        "actual_mac_per_cycle": actual_mac_per_cycle,
        "actual_ops_per_cycle": actual_ops_per_cycle,
        "compute_peak_util": compute_peak_util,
        "attainable_roof_util": attainable_roof_util,
        "external_mem_cycles_min": external_mem_cycles_min,
        "compute_cycles_min": compute_cycles_min,
        "roof_cycles_min": roof_cycles_min,
        "latency_over_roof_lower_bound": latency_over_roof_lower_bound,
        "bound_classification": bound,
        "ext_load_a_cycles_est": ext_a_cycles,
        "ext_load_b_cycles_est": ext_b_cycles,
        "ext_store_c_cycles_est": ext_c_cycles,
        "ext_load_compute_overlap": int(p.overlap_ext_load_compute),
        "store_compute_overlap": int(p.overlap_store_compute),
        **internal,
        "total_model_cycles": total_model_cycles,
        "local_model_gap": local_model_gap,
        "total_model_gap": total_model_gap,
        "latency_cycles": p.latency_cycles,
        "actual_gmac_s_at_freq": actual_gmac_s,
        "actual_gops_at_freq": actual_gops,
        "gops_per_dsp": gops_per_dsp,
        "gops_per_bram18k": gops_per_bram18k,
        "gops_per_klut": gops_per_klut,
        "bram18k": p.bram18k,
        "dsp": p.dsp,
        "ff": p.ff,
        "lut": p.lut,
        "estimated_clock_ns": p.estimated_clock_ns,
    }


def write_rows_csv(path: Path, rows: list[dict[str, float | int | str]]) -> None:
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def write_csv(rows: list[dict[str, float | int | str]]) -> None:
    write_rows_csv(POINTS_CSV, rows)


def table(lines: list[str], headers: Iterable[str], rows: Iterable[Iterable[str]]) -> None:
    header_list = list(headers)
    lines.append("| " + " | ".join(header_list) + " |\n")
    lines.append("| " + " | ".join(["---"] * len(header_list)) + " |\n")
    for row in rows:
        lines.append("| " + " | ".join(row) + " |\n")


def pct(value: float | int | str) -> str:
    return f"{float(value) * 100:.2f}%"


def num(value: float | int | str, digits: int = 3) -> str:
    if isinstance(value, int):
        return str(value)
    return f"{float(value):.{digits}f}"


def write_markdown(rows: list[dict[str, float | int | str]]) -> None:
    lines: list[str] = []
    lines.append("# Internal Roofline Summary\n\n")
    lines.append("这份表由 `python/analysis/roofline_model.py` 生成，实验点来自 `python/analysis/roofline_experiments.csv`。\n\n")
    lines.append("当前默认假设：`freq=100MHz`，`DDR effective bandwidth=800MB/s`，即 `8 bytes/cycle`；计算口径同时保留 MAC/cycle 和 ops/cycle，避免单位混用。\n\n")

    lines.append("## Roofline 分类\n\n")
    table(
        lines,
        ["Case", "attainable MAC/cycle", "actual MAC/cycle", "compute util", "attainable util", "roof cycles min", "latency/roof", "bound"],
        (
            [
                str(r["name"]),
                num(r["attainable_mac_per_cycle"]),
                num(r["actual_mac_per_cycle"]),
                pct(r["compute_peak_util"]),
                pct(r["attainable_roof_util"]),
                num(r["roof_cycles_min"], 1),
                num(r["latency_over_roof_lower_bound"], 2) + "x",
                str(r["bound_classification"]),
            ]
            for r in rows
        ),
    )

    lines.append("\n## 外部 traffic 与 roof 下界\n\n")
    table(
        lines,
        ["Case", "A bytes", "B bytes", "C bytes", "external bytes", "mem roof MAC/cycle", "compute roof MAC/cycle"],
        (
            [
                str(r["name"]),
                str(r["a_bytes"]),
                str(r["b_bytes"]),
                str(r["c_bytes"]),
                str(r["external_bytes"]),
                num(r["mem_roof_mac_per_cycle"]),
                num(r["compute_roof_mac_per_cycle"]),
            ]
            for r in rows
        ),
    )

    lines.append("\n## 内部 scheduler 模型\n\n")
    table(
        lines,
        ["Case", "internal model cycles", "total model cycles", "actual latency", "local model gap", "total model gap", "modeled MAC active", "full tiles", "tail tiles"],
        (
            [
                str(r["name"]),
                num(r["internal_model_cycles"], 1),
                num(r["total_model_cycles"], 1),
                str(r["latency_cycles"]),
                num(r["local_model_gap"], 2) + "x",
                num(r["total_model_gap"], 2) + "x",
                pct(r["modeled_mac_active_ratio"]),
                str(r["full_output_tile_count"]),
                str(r["tail_output_tile_count"]),
            ]
            for r in rows
        ),
    )

    lines.append("\n## 资源效率\n\n")
    table(
        lines,
        ["Case", "GOPS@freq", "GOPS/DSP", "GOPS/BRAM18K", "GOPS/kLUT", "DSP", "BRAM18K", "LUT"],
        (
            [
                str(r["name"]),
                num(r["actual_gops_at_freq"]),
                num(r["gops_per_dsp"], 5),
                num(r["gops_per_bram18k"], 5),
                num(r["gops_per_klut"], 5),
                str(r["dsp"]),
                str(r["bram18k"]),
                str(r["lut"]),
            ]
            for r in rows
        ),
    )

    lines.append("\n## 我的直接理解\n\n")
    lines.append("1. 在 128 规模实验里，外部 roofline 当前给出的 attainable roof 是 `128 MAC/cycle`，而 O2 只有约 `6.61 MAC/cycle`；在 224 full-block 实验里，attainable roof 到 `196 MAC/cycle`，O1_224_generic/O6c 也只有约 `29.43-29.45 MAC/cycle`，所以这些点仍然归类为 `internal/scheduler-bound`。\n")
    lines.append("2. 128 规模下 O2 的 `attainable_roof_util` 约为 5.17%；224 full-block 下 O1_224_generic/O6c 提升到约 15.02%-15.03%，但离 MAC 阵列真正吃满仍然很远，说明内部 scheduler 和 local feeding 还有很大优化空间。\n")
    lines.append("3. 旧的 `modeled_mac_active_ratio` 只能解释局部 output tile，不能解释完整 scheduler latency；新增的 `local_model_gap` 和 `total_model_gap` 用来暴露模型没解释掉的 HLS 开销。\n")
    lines.append("4. O2 绝对性能最好，但 BRAM/LUT 效率下降；O7a/O7b 继续提高 row banking 后资源明显爆炸且 latency 退化，所以行方向 banking 不适合作为落地路线。\n")
    lines.append("5. O4/O5 功能通过但性能和资源效率都明显失败；O4inline 虽然 LUT 下降，但 latency 退化到接近 `3M` cycles，说明 `INLINE off` 不是唯一主因，combined local A/B load 结构本身不适合继续加码。\n")
    lines.append("6. O6 runtime full-block fast path 功能通过但会把 full/fallback 双路径一起综合；O6c full-only 和 `O1_224_generic` 的干净对照说明，边界/generic 控制主要增加 LUT/FF，而不是显著增加当前调度结构下的 latency。\n")
    lines.append("7. 下一步更应该保留必要的列方向 banking，转向更清楚的数据流、double buffer 和 load-compute overlap，而不是继续 row banking 或 local A/B helper 合并。\n")

    with SUMMARY_MD.open("w", encoding="utf-8") as f:
        f.writelines(lines)


def write_ideal_markdown(rows: list[dict[str, float | int | str]]) -> None:
    lines: list[str] = []
    lines.append("# Ideal Lower-Bound Model\n\n")
    lines.append("这份表由 `python/analysis/roofline_model.py` 生成，模型口径是 optimistic lower bound。\n\n")
    lines.append("它保留外部 traffic 计算，但把 DDR、compute 和 local tile 搬运都按理想下界处理；它用于判断理论 roof，不用于精确预测当前 HLS latency。\n\n")

    table(
        lines,
        [
            "Case",
            "ideal roofline",
            "ideal no-overlap",
            "actual latency",
            "actual/roofline",
            "actual/no-overlap",
            "compute cycles",
            "external cycles",
            "ideal internal",
        ],
        (
            [
                str(r["name"]),
                num(r["ideal_roofline_cycles"], 1),
                num(r["ideal_no_overlap_cycles"], 1),
                str(r["latency_cycles"]),
                num(r["latency_over_ideal_roofline"], 2) + "x",
                num(r["latency_over_ideal_no_overlap"], 2) + "x",
                num(r["ideal_compute_cycles"], 1),
                num(r["ideal_external_total_cycles"], 1),
                num(r["ideal_internal_cycles"], 1),
            ]
            for r in rows
        ),
    )

    lines.append("\n## 直接理解\n\n")
    lines.append("1. `ideal_roofline_cycles` 是传统 roofline 下界，只取 DDR total cycles 和 compute cycles 的最大值。\n")
    lines.append("2. `ideal_no_overlap_cycles` 是五阶段理想模型，不做 load/compute/store overlap，但假设 local tile 搬运已经达到理想 banking。\n")
    lines.append("3. 如果实际 latency 仍远高于这两个值，说明问题主要在当前 HLS loop schedule、local feeding、控制和资源约束，而不是矩阵乘本身的理论算力。\n")

    with IDEAL_SUMMARY_MD.open("w", encoding="utf-8") as f:
        f.writelines(lines)


def write_hls_loop_markdown(rows: list[dict[str, float | int | str]]) -> None:
    lines: list[str] = []
    lines.append("# HLS Loop Schedule Model\n\n")
    lines.append("这份表按当前 HLS C++ 的 loop tripcount 和 II 估算 latency，不用 `bytes / DDR bandwidth` 去估当前 memory 版 load/store。\n\n")
    lines.append("模型形式对应：`T_total ~= T_load_AB_block + T_compute_block_internal + T_store_C_block + T_control`。\n\n")

    table(
        lines,
        [
            "Case",
            "load",
            "compute internal",
            "store",
            "model total",
            "actual",
            "unexplained",
            "gap",
            "category",
            "deploy",
        ],
        (
            [
                str(r["name"]),
                num(r["hls_loop_load_cycles"], 1),
                num(r["hls_loop_compute_internal_cycles"], 1),
                num(r["hls_loop_store_cycles"], 1),
                num(r["hls_loop_total_cycles"], 1),
                str(r["latency_cycles"]),
                num(r["hls_loop_unexplained_cycles"], 1),
                num(r["hls_loop_model_gap"], 2) + "x",
                str(r["latency_category"]),
                str(r["deployable_status"]),
            ]
            for r in rows
        ),
    )

    lines.append("\n## Loop Breakdown\n\n")
    table(
        lines,
        [
            "Case",
            "n/m/k blk",
            "tile n/m/k",
            "T_load/block",
            "T_one_tile",
            "C read",
            "A load",
            "B load",
            "AB load",
            "compute",
            "C write",
            "local share",
        ],
        (
            [
                str(r["name"]),
                f"{r['n_blk']}/{r['m_blk']}/{r['k_blk']}",
                f"{r['tile_n']}/{r['tile_m']}/{r['tile_k']}",
                num(r["T_load_per_block"], 1),
                num(r["T_one_output_tile"], 1),
                num(r["T_local_C_load"], 1),
                num(r["T_local_A_load"], 1),
                num(r["T_local_B_load"], 1),
                num(r["T_local_AB_load"], 1),
                num(r["T_compute"], 1),
                num(r["T_local_C_store"], 1),
                pct(r["local_load_store_share_of_output_tile"]),
            ]
            for r in rows
        ),
    )

    with HLS_LOOP_SUMMARY_MD.open("w", encoding="utf-8") as f:
        f.writelines(lines)


def combined_rows(
    ideal_rows: list[dict[str, float | int | str]],
    hls_rows: list[dict[str, float | int | str]],
) -> list[dict[str, float | int | str]]:
    ideal_by_name = {str(r["name"]): r for r in ideal_rows}
    rows: list[dict[str, float | int | str]] = []
    for h in hls_rows:
        i = ideal_by_name[str(h["name"])]
        rows.append(
            {
                "name": h["name"],
                "note": h["note"],
                "N": h["N"],
                "K": h["K"],
                "M": h["M"],
                "tile": h["tile"],
                "block_N": h["block_N"],
                "block_K": h["block_K"],
                "block_M": h["block_M"],
                "row_unroll": h["row_unroll"],
                "latency_cycles": h["latency_cycles"],
                "ideal_roofline_cycles": i["ideal_roofline_cycles"],
                "ideal_no_overlap_cycles": i["ideal_no_overlap_cycles"],
                "hls_loop_load_cycles": h["hls_loop_load_cycles"],
                "hls_loop_compute_internal_cycles": h["hls_loop_compute_internal_cycles"],
                "hls_loop_store_cycles": h["hls_loop_store_cycles"],
                "hls_loop_total_cycles": h["hls_loop_total_cycles"],
                "hls_loop_unexplained_cycles": h["hls_loop_unexplained_cycles"],
                "hls_loop_model_gap": h["hls_loop_model_gap"],
                "latency_over_ideal_roofline": i["latency_over_ideal_roofline"],
                "latency_over_ideal_no_overlap": i["latency_over_ideal_no_overlap"],
                "local_load_store_share_of_output_tile": h["local_load_store_share_of_output_tile"],
                "latency_category": h["latency_category"],
                "deployable_status": h["deployable_status"],
                "actual_mac_per_cycle": h["actual_mac_per_cycle"],
                "actual_gops_at_freq": h["actual_gops_at_freq"],
                "bram18k": h["bram18k"],
                "dsp": h["dsp"],
                "ff": h["ff"],
                "lut": h["lut"],
                "estimated_clock_ns": h["estimated_clock_ns"],
            }
        )
    return rows


def write_combined_markdown(rows: list[dict[str, float | int | str]]) -> None:
    lines: list[str] = []
    lines.append("# Combined Roofline And HLS Loop Summary\n\n")
    lines.append("这份总表把 theoretical lower bound 和当前 HLS loop schedule 模型分开列出，避免把理想 roofline 当成 HLS latency 预测。\n\n")

    lines.append("## Ideal lower-bound model\n\n")
    lines.append("这是理论下界，用于判断 DDR/compute roof，不用于精确预测 HLS latency。`ideal_roofline_cycles` 是完全重叠意义下的 roofline 下界；`ideal_no_overlap_cycles` 是五阶段理想但不 overlap 的下界。\n\n")
    table(
        lines,
        ["Case", "actual", "ideal roofline", "actual/roof", "ideal no-overlap", "actual/no-overlap"],
        (
            [
                str(r["name"]),
                str(r["latency_cycles"]),
                num(r["ideal_roofline_cycles"], 1),
                num(r["latency_over_ideal_roofline"], 2) + "x",
                num(r["ideal_no_overlap_cycles"], 1),
                num(r["latency_over_ideal_no_overlap"], 2) + "x",
            ]
            for r in rows
        ),
    )

    lines.append("\n## HLS loop schedule model\n\n")
    lines.append("这是根据当前 C++ 循环 tripcount 和 II 建的模型，更适合解释 HLS latency。它对应图里的 `T_load_AB_block + T_compute_block_internal + T_store_C_block + T_control`。\n\n")
    table(
        lines,
        ["Case", "load", "compute internal", "store", "T_control", "model", "actual", "gap", "category", "deploy"],
        (
            [
                str(r["name"]),
                num(r["hls_loop_load_cycles"], 1),
                num(r["hls_loop_compute_internal_cycles"], 1),
                num(r["hls_loop_store_cycles"], 1),
                num(r["hls_loop_unexplained_cycles"], 1),
                num(r["hls_loop_total_cycles"], 1),
                str(r["latency_cycles"]),
                num(r["hls_loop_model_gap"], 2) + "x",
                str(r["latency_category"]),
                str(r["deployable_status"]),
            ]
            for r in rows
        ),
    )

    closest = min(rows, key=lambda r: abs(float(r["hls_loop_model_gap"]) - 1.0))
    local_dominated = [
        f"{r['name']}({float(r['local_load_store_share_of_output_tile']) * 100:.1f}%)"
        for r in rows
        if float(r["local_load_store_share_of_output_tile"]) > 0.5
    ]
    not_deployable = [str(r["name"]) for r in rows if str(r["deployable_status"]) == "not deployable"]
    by_name = {str(r["name"]): r for r in rows}

    lines.append("\n## 结论\n\n")
    lines.append(f"1. 当前 HLS loop model 最接近实际的是 `{closest['name']}`，model gap 约为 `{float(closest['hls_loop_model_gap']):.2f}x`。\n")
    lines.append("2. local load/store 占每个 output tile 时间超过 50% 的版本有：" + ", ".join(local_dominated) + "。这说明主要问题不是 MAC 数量不够，而是 local feeding 没有被隐藏。\n")
    if {"O1", "O2", "O4", "O5"}.issubset(by_name):
        o1 = by_name["O1"]
        o2 = by_name["O2"]
        o4 = by_name["O4"]
        o5 = by_name["O5"]
        lines.append(
            "3. O1/O2/O4/O5 的差异："
            f"O1 模型为 `{float(o1['hls_loop_total_cycles']):.0f}` cycles；"
            f"O2 通过 row_unroll=2 把模型降到 `{float(o2['hls_loop_total_cycles']):.0f}` cycles；"
            f"O4/O5 的 local A/B helper 让每个 output tile 的 local load 变重，模型升到 `{float(o4['hls_loop_total_cycles']):.0f}` / `{float(o5['hls_loop_total_cycles']):.0f}` cycles。\n"
        )
    lines.append(f"4. LUT 超过 `{ZYNQ7020_LUT_LIMIT}` 的版本标记为 not deployable：" + ", ".join(not_deployable) + "。\n")
    lines.append("5. O1 是当前 ZYNQ-7020 可落地 baseline：资源没有超过 LUT 上限，虽然 latency 还明显高于理想下界。\n")
    if "O2" in by_name:
        lines.append(f"6. O2 虽然更快，但 LUT={by_name['O2']['lut']} 超过 `{ZYNQ7020_LUT_LIMIT}`，只能作为性能探索点，不能作为当前落地版本。\n")

    with COMBINED_SUMMARY_MD.open("w", encoding="utf-8") as f:
        f.writelines(lines)


def main() -> None:
    points = load_points(INPUT_CSV)
    rows = [metrics(point) for point in points]
    ideal_rows = [ideal_lower_bound_metrics(point) for point in points]
    hls_rows = [hls_loop_schedule_metrics(point) for point in points]
    combined = combined_rows(ideal_rows, hls_rows)

    write_csv(rows)
    write_markdown(rows)
    write_rows_csv(IDEAL_POINTS_CSV, ideal_rows)
    write_ideal_markdown(ideal_rows)
    write_rows_csv(HLS_LOOP_POINTS_CSV, hls_rows)
    write_hls_loop_markdown(hls_rows)
    write_rows_csv(COMBINED_POINTS_CSV, combined)
    write_combined_markdown(combined)

    print(f"Wrote {POINTS_CSV.relative_to(ROOT)}")
    print(f"Wrote {SUMMARY_MD.relative_to(ROOT)}")
    print(f"Wrote {IDEAL_POINTS_CSV.relative_to(ROOT)}")
    print(f"Wrote {IDEAL_SUMMARY_MD.relative_to(ROOT)}")
    print(f"Wrote {HLS_LOOP_POINTS_CSV.relative_to(ROOT)}")
    print(f"Wrote {HLS_LOOP_SUMMARY_MD.relative_to(ROOT)}")
    print(f"Wrote {COMBINED_POINTS_CSV.relative_to(ROOT)}")
    print(f"Wrote {COMBINED_SUMMARY_MD.relative_to(ROOT)}")
    for row in rows:
        print(
            "{name}: bound={bound_classification}, actual={actual_mac_per_cycle:.3f} MAC/cycle, "
            "attainable={attainable_mac_per_cycle:.3f}, latency/roof={latency_over_roof_lower_bound:.2f}x".format(**row)
        )


if __name__ == "__main__":
    main()
