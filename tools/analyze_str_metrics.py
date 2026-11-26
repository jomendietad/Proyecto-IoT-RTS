#!/usr/bin/env python3
"""Analizador y capturador de metricas STR ([STR_METRIC], [STR_HEALTH], [STR_STACK])."""

from __future__ import annotations

import argparse
import csv
import math
import os
import re
import statistics
import sys
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[mGKHF]")

METRIC_RE = re.compile(
    r"\[STR_METRIC\]\s+node=(?P<node>\S+)\s+metric=(?P<metric>\S+)\s+"
    r"value_us=(?P<value>\d+)\s+deadline_us=(?P<deadline>\d+)\s+miss=(?P<miss>[01])"
)
HEALTH_RE = re.compile(
    r"\[STR_HEALTH\]\s+node=(?P<node>\S+)\s+uptime_s=(?P<uptime>\d+)\s+"
    r"free_heap=(?P<free_heap>\d+)\s+min_heap=(?P<min_heap>\d+)\s+"
    r"max_lat_us=(?P<max_lat>\d+)\s+avg_jit_us=(?P<avg_jit>\d+)\s+misses=(?P<misses>\d+)"
)
STACK_RE = re.compile(
    r"\[STR_STACK\]\s+node=(?P<node>\S+)\s+task=(?P<task>\S+)\s+min_free_bytes=(?P<bytes>\d+)"
)


@dataclass
class MetricSummary:
    """Resumen estadistico por (nodo, metrica)."""

    node: str
    metric: str
    count: int
    min_us: int
    mean_us: float
    stdev_us: float
    p50_us: float
    p95_us: float
    p99_us: float
    wcrt_us: int
    deadline_us: int
    misses: int

    @property
    def compliance_pct(self) -> float:
        if self.count == 0:
            return 100.0
        return 100.0 * (self.count - self.misses) / self.count


@dataclass
class ParsedLog:
    """Datos estructurados extraidos de uno o mas archivos de log."""

    samples: Dict[Tuple[str, str], List[Tuple[int, int, int]]]
    health_max_uptime: Dict[str, int]
    health_min_heap: Dict[str, int]
    health_last_free: Dict[str, int]
    stack_min_bytes: Dict[Tuple[str, str], int]


def percentile(sorted_vals: List[int], q: float) -> float:
    """Calcula el percentil lineal (q en 0..100) sobre una lista ya ordenada."""
    if not sorted_vals:
        return 0.0
    if len(sorted_vals) == 1:
        return float(sorted_vals[0])
    pos = (len(sorted_vals) - 1) * (q / 100.0)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return float(sorted_vals[lo])
    frac = pos - lo
    return sorted_vals[lo] * (1.0 - frac) + sorted_vals[hi] * frac


def parse_lines(lines: Iterable[str]) -> ParsedLog:
    """Extrae muestras STR, salud y watermarks de stack limpiando secuencias ANSI."""
    samples: Dict[Tuple[str, str], List[Tuple[int, int, int]]] = defaultdict(list)
    health_max_uptime: Dict[str, int] = {}
    health_min_heap: Dict[str, int] = {}
    health_last_free: Dict[str, int] = {}
    stack_min_bytes: Dict[Tuple[str, str], int] = {}

    for raw_line in lines:
        line = ANSI_RE.sub("", raw_line).strip()
        if not line:
            continue

        m = METRIC_RE.search(line)
        if m:
            key = (m.group("node"), m.group("metric"))
            samples[key].append(
                (int(m.group("value")), int(m.group("deadline")), int(m.group("miss")))
            )
            continue

        h = HEALTH_RE.search(line)
        if h:
            node = h.group("node")
            up = int(h.group("uptime"))
            free_h = int(h.group("free_heap"))
            min_h = int(h.group("min_heap"))
            health_max_uptime[node] = max(health_max_uptime.get(node, 0), up)
            prev_min = health_min_heap.get(node)
            health_min_heap[node] = min_h if prev_min is None else min(prev_min, min_h)
            health_last_free[node] = free_h
            continue

        s = STACK_RE.search(line)
        if s:
            key = (s.group("node"), s.group("task"))
            val = int(s.group("bytes"))
            prev = stack_min_bytes.get(key)
            stack_min_bytes[key] = val if prev is None else min(prev, val)

    return ParsedLog(
        samples=samples,
        health_max_uptime=health_max_uptime,
        health_min_heap=health_min_heap,
        health_last_free=health_last_free,
        stack_min_bytes=stack_min_bytes,
    )


def parse_files(paths: List[Path]) -> ParsedLog:
    """Lee y combina multiples archivos de log."""
    all_lines: List[str] = []
    for path in paths:
        with path.open("r", encoding="utf-8", errors="replace") as fh:
            all_lines.extend(fh.readlines())
    return parse_lines(all_lines)


def summarize_metrics(parsed: ParsedLog) -> List[MetricSummary]:
    """Calcula estadisticas descriptivas, percentiles y WCRT por (nodo, metrica)."""
    summaries: List[MetricSummary] = []
    for (node, metric) in sorted(parsed.samples.keys()):
        rows = parsed.samples[(node, metric)]
        vals = sorted(r[0] for r in rows)
        deadlines = [r[1] for r in rows]
        misses = sum(r[2] for r in rows)
        stdev = statistics.stdev(vals) if len(vals) > 1 else 0.0
        summaries.append(
            MetricSummary(
                node=node,
                metric=metric,
                count=len(vals),
                min_us=vals[0],
                mean_us=statistics.mean(vals),
                stdev_us=stdev,
                p50_us=percentile(vals, 50.0),
                p95_us=percentile(vals, 95.0),
                p99_us=percentile(vals, 99.0),
                wcrt_us=vals[-1],
                deadline_us=max(deadlines) if deadlines else 0,
                misses=misses,
            )
        )
    return summaries


def render_markdown(parsed: ParsedLog, summaries: List[MetricSummary]) -> str:
    """Formatea tablas Markdown listas para bitacora_pruebas.md."""
    out: List[str] = []
    out.append("## Resumen Estadistico STR (`[STR_METRIC]`)")
    out.append("")
    if not summaries:
        out.append("_No se encontraron lineas `[STR_METRIC]` en los archivos analizados._")
    else:
        out.append(
            "| Nodo | Metrica | N | Min (us) | Media (us) | Desv.Est (us) | "
            "p50 (us) | p95 (us) | p99 (us) | WCRT Max (us) | Deadline (us) | Misses | Cumplimiento |"
        )
        out.append("| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")
        for s in summaries:
            out.append(
                f"| {s.node} | `{s.metric}` | {s.count} | {s.min_us} | {s.mean_us:.1f} | "
                f"{s.stdev_us:.1f} | {s.p50_us:.1f} | {s.p95_us:.1f} | {s.p99_us:.1f} | "
                f"**{s.wcrt_us}** | {s.deadline_us} | {s.misses} | {s.compliance_pct:.2f}% |"
            )

    if parsed.health_max_uptime:
        out.append("")
        out.append("## Salud de Heap y Uptime (`[STR_HEALTH]`)")
        out.append("")
        out.append("| Nodo | Uptime max (s) | Ultimo `free_heap` (B) | Minimo `min_heap` (B) |")
        out.append("| :--- | :---: | :---: | :---: |")
        for node in sorted(parsed.health_max_uptime.keys()):
            out.append(
                f"| {node} | {parsed.health_max_uptime[node]} | "
                f"{parsed.health_last_free[node]:,} | {parsed.health_min_heap[node]:,} |"
            )

    if parsed.stack_min_bytes:
        out.append("")
        out.append("## Margen Minimo de Stack (`[STR_STACK]`)")
        out.append("")
        out.append("| Nodo | Tarea | Min `free_bytes` (B) |")
        out.append("| :--- | :--- | :---: |")
        for (node, task) in sorted(parsed.stack_min_bytes.keys()):
            out.append(f"| {node} | `{task}` | {parsed.stack_min_bytes[(node, task)]} |")

    return "\n".join(out) + "\n"


def write_csv(path: Path, summaries: List[MetricSummary]) -> None:
    """Exporta las estadisticas de metricas a un archivo CSV."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow([
            "node", "metric", "count", "min_us", "mean_us", "stdev_us",
            "p50_us", "p95_us", "p99_us", "wcrt_us", "deadline_us", "misses", "compliance_pct",
        ])
        for s in summaries:
            writer.writerow([
                s.node, s.metric, s.count, s.min_us, f"{s.mean_us:.2f}", f"{s.stdev_us:.2f}",
                f"{s.p50_us:.2f}", f"{s.p95_us:.2f}", f"{s.p99_us:.2f}",
                s.wcrt_us, s.deadline_us, s.misses, f"{s.compliance_pct:.2f}",
            ])


def pct_delta(base: float, other: float) -> str:
    """Calcula la variacion porcentual formateada respecto a base."""
    if base <= 0.0:
        return "N/A"
    return f"{((other - base) / base) * 100.0:+.1f}%"


def render_comparison(label_a: str, sum_a: List[MetricSummary], label_b: str, sum_b: List[MetricSummary]) -> str:
    """Genera tabla comparativa entre dos corridas (ej. sin_wifi vs con_wifi)."""
    map_a = {(s.node, s.metric): s for s in sum_a}
    map_b = {(s.node, s.metric): s for s in sum_b}
    all_keys = sorted(set(map_a.keys()) | set(map_b.keys()))

    out = [
        f"## Comparativa STR: `{label_a}` vs `{label_b}`",
        "",
        f"| Nodo | Metrica | {label_a} N | {label_a} Media | {label_a} p95 | {label_a} WCRT | "
        f"{label_b} N | {label_b} Media | {label_b} p95 | {label_b} WCRT | Delta Media | Delta WCRT |",
        "| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |",
    ]
    if not all_keys:
        out.append("| - | _Sin metricas comunes_ | 0 | - | - | - | 0 | - | - | - | - | - |")
        return "\n".join(out) + "\n"

    for key in all_keys:
        a = map_a.get(key)
        b = map_b.get(key)
        node, metric = key
        a_n = str(a.count) if a else "0"
        a_mean = f"{a.mean_us:.1f}" if a else "-"
        a_p95 = f"{a.p95_us:.1f}" if a else "-"
        a_wcrt = str(a.wcrt_us) if a else "-"
        b_n = str(b.count) if b else "0"
        b_mean = f"{b.mean_us:.1f}" if b else "-"
        b_p95 = f"{b.p95_us:.1f}" if b else "-"
        b_wcrt = str(b.wcrt_us) if b else "-"
        d_mean = pct_delta(a.mean_us, b.mean_us) if (a and b) else "N/A"
        d_wcrt = pct_delta(float(a.wcrt_us), float(b.wcrt_us)) if (a and b) else "N/A"
        out.append(
            f"| {node} | `{metric}` | {a_n} | {a_mean} | {a_p95} | {a_wcrt} | "
            f"{b_n} | {b_mean} | {b_p95} | {b_wcrt} | {d_mean} | {d_wcrt} |"
        )
    return "\n".join(out) + "\n"


def send_commands(ser, cmds: Optional[str]) -> None:
    """Envia comandos separados por ';' al puerto serial."""
    if not cmds:
        return
    for raw in cmds.split(";"):
        cmd = raw.strip()
        if cmd:
            ser.write((cmd + "\n").encode("utf-8"))
            ser.flush()
            time.sleep(0.15)


def capture_serial(args: argparse.Namespace) -> Path:
    """Captura lineas por puerto serie hacia archivo, con estimulos opcionales."""
    try:
        import serial  # type: ignore
    except ImportError:
        print(
            "Error: pyserial no esta instalado en este Python. Usa el venv de ESP-IDF:\n"
            "  /home/johan/.espressif/tools/python/v6.1.1/venv/bin/python3 tools/analyze_str_metrics.py ...",
            file=sys.stderr,
        )
        sys.exit(2)

    if not args.port:
        print("Error: --capture requiere --port (ej. --port /dev/ttyACM0).", file=sys.stderr)
        sys.exit(2)

    out_path = Path(args.out) if args.out else Path("exports") / f"capture_{int(time.time())}.log"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    print(f"[capture] Abriendo {args.port} @ {args.baud} bps -> {out_path}", file=sys.stderr)
    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    time.sleep(0.3)
    ser.reset_input_buffer()

    send_commands(ser, args.init_cmd)

    next_rep_t = time.monotonic() + 0.3
    sent_reps = 0
    end_t = time.monotonic() + float(args.duration)

    with out_path.open("w", encoding="utf-8") as fh:
        while True:
            now = time.monotonic()
            if args.repeat_cmd and sent_reps < args.repeat_count and now >= next_rep_t:
                ser.write((args.repeat_cmd.strip() + "\n").encode("utf-8"))
                ser.flush()
                sent_reps += 1
                next_rep_t = now + float(args.repeat_interval)
                if sent_reps == args.repeat_count and args.duration <= 0:
                    end_t = now + 2.0

            raw = ser.readline()
            if raw:
                text = raw.decode("utf-8", errors="replace")
                fh.write(text)
                if args.echo:
                    sys.stdout.write(text)

            if args.repeat_cmd and args.duration <= 0:
                if sent_reps >= args.repeat_count and time.monotonic() >= end_t:
                    break
            elif time.monotonic() >= end_t:
                break

        send_commands(ser, args.post_cmd)
        post_end = time.monotonic() + 1.0
        while time.monotonic() < post_end:
            raw = ser.readline()
            if raw:
                text = raw.decode("utf-8", errors="replace")
                fh.write(text)
                if args.echo:
                    sys.stdout.write(text)

    ser.close()
    print(f"[capture] Captura finalizada ({sent_reps} estimulos enviados).", file=sys.stderr)
    return out_path


def parse_compare_arg(spec: str) -> Tuple[str, Path]:
    """Parsea argumento etiqueta=ruta.log."""
    if "=" not in spec:
        raise argparse.ArgumentTypeError(f"Formato invalido '{spec}'. Usa etiqueta=ruta.log")
    label, path_str = spec.split("=", 1)
    return label.strip(), Path(path_str.strip())


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Captura y analiza logs [STR_METRIC], [STR_HEALTH] y [STR_STACK] de ESP-IDF."
    )
    p.add_argument("logs", nargs="*", help="Archivos de log a analizar.")
    p.add_argument("--csv", help="Ruta opcional para exportar estadisticas en CSV.")
    p.add_argument("--markdown", help="Ruta opcional para guardar el reporte Markdown.")
    p.add_argument(
        "--compare",
        nargs=2,
        metavar=("A=LOG_A", "B=LOG_B"),
        help="Compara dos corridas (ej. --compare sin_wifi=exports/a.log con_wifi=exports/b.log).",
    )
    p.add_argument("--capture", action="store_true", help="Captura logs por puerto serie antes de analizar.")
    p.add_argument("--port", help="Puerto serie (ej. /dev/ttyACM0 o /dev/ttyUSB0).")
    p.add_argument("--baud", type=int, default=115200, help="Baudrate serial (defecto: 115200).")
    p.add_argument("--duration", type=float, default=30.0, help="Duracion de captura en segundos (0 = hasta completar --repeat-count).")
    p.add_argument("--out", help="Archivo de salida para la captura (defecto: exports/capture_<ts>.log).")
    p.add_argument("--init-cmd", help="Comandos iniciales separados por ';' (ej. 'arm').")
    p.add_argument("--repeat-cmd", help="Comando repetido durante la captura (ej. 'sim_pir').")
    p.add_argument("--repeat-count", type=int, default=0, help="Cantidad de repeticiones de --repeat-cmd.")
    p.add_argument("--repeat-interval", type=float, default=0.35, help="Intervalo en segundos entre repeticiones.")
    p.add_argument("--post-cmd", help="Comandos finales separados por ';' (ej. 'disarm').")
    p.add_argument("--echo", action="store_true", help="Imprime el log en vivo durante la captura.")
    return p


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.compare:
        label_a, path_a = parse_compare_arg(args.compare[0])
        label_b, path_b = parse_compare_arg(args.compare[1])
        sum_a = summarize_metrics(parse_files([path_a]))
        sum_b = summarize_metrics(parse_files([path_b]))
        report = render_comparison(label_a, sum_a, label_b, sum_b)
        print(report, end="")
        if args.markdown:
            Path(args.markdown).write_text(report, encoding="utf-8")
        return 0

    log_paths: List[Path] = [Path(p) for p in args.logs]
    if args.capture:
        captured = capture_serial(args)
        log_paths.append(captured)

    if not log_paths:
        parser.print_help()
        return 1

    missing = [str(p) for p in log_paths if not p.is_file()]
    if missing:
        print(f"Error: archivos no encontrados: {', '.join(missing)}", file=sys.stderr)
        return 2

    parsed = parse_files(log_paths)
    summaries = summarize_metrics(parsed)
    report = render_markdown(parsed, summaries)
    print(report, end="")

    if args.csv:
        write_csv(Path(args.csv), summaries)
    if args.markdown:
        Path(args.markdown).parent.mkdir(parents=True, exist_ok=True)
        Path(args.markdown).write_text(report, encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
