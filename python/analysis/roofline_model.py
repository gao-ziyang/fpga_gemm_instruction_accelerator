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

INT8_BYTES = 1
INT32_BYTES = 4
DEFAULT_FREQ_MHZ = 100.0
DEFAULT_DDR_MB_PER_S = 800.0


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


def write_csv(rows: list[dict[str, float | int | str]]) -> None:
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    with POINTS_CSV.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


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
    lines.append("1. 外部 roofline 当前给出的 attainable roof 是 `128 MAC/cycle`，而 O2 只有 `6.613 MAC/cycle`，所以 O0-O5 都应该归类为 `internal/scheduler-bound`。\n")
    lines.append("2. O2 的 `attainable_roof_util` 约为 5.17%，`compute_peak_util` 约为 3.37%，说明内部 scheduler 和 local feeding 还有很大优化空间。\n")
    lines.append("3. 旧的 `modeled_mac_active_ratio` 只能解释局部 output tile，不能解释完整 scheduler latency；新增的 `local_model_gap` 和 `total_model_gap` 用来暴露模型没解释掉的 HLS 开销。\n")
    lines.append("4. O2 绝对性能最好，但 BRAM/LUT 效率下降；O4/O5 功能通过但性能和资源效率都明显失败。\n")
    lines.append("5. 下一步更应该做 full-block fast path、边界判断剥离、地址/mux 简化和更细的 TILE/BLOCK/ROW_UNROLL sweep，而不是继续粗暴合并 helper。\n")

    with SUMMARY_MD.open("w", encoding="utf-8") as f:
        f.writelines(lines)


def main() -> None:
    points = load_points(INPUT_CSV)
    rows = [metrics(point) for point in points]
    write_csv(rows)
    write_markdown(rows)
    print(f"Wrote {POINTS_CSV.relative_to(ROOT)}")
    print(f"Wrote {SUMMARY_MD.relative_to(ROOT)}")
    for row in rows:
        print(
            "{name}: bound={bound_classification}, actual={actual_mac_per_cycle:.3f} MAC/cycle, "
            "attainable={attainable_mac_per_cycle:.3f}, latency/roof={latency_over_roof_lower_bound:.2f}x".format(**row)
        )


if __name__ == "__main__":
    main()
