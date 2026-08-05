#!/usr/bin/env python3
"""Design and verify the stage-1 fan-feedback shadow band-pass filter."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from scipy import signal

FS_HZ = 10_000.0
LOW_HZ = 20.0
HIGH_HZ = 100.0
# scipy band-pass order N=2 produces a fourth-order digital band-pass.
SCIPY_ORDER = 2

FREQUENCIES_HZ = np.array(
    [0.1, 5.0, 10.0, 20.0, 30.0, 33.333333, 50.0, 76.666667,
     80.0, 100.0, 150.0, 300.0, 1000.0, 4500.0],
    dtype=np.float64,
)


def design() -> np.ndarray:
    return signal.butter(
        SCIPY_ORDER,
        [LOW_HZ, HIGH_HZ],
        btype="bandpass",
        fs=FS_HZ,
        output="sos",
    )


def response_table(sos: np.ndarray) -> list[tuple[float, float, float]]:
    omega = 2.0 * np.pi * FREQUENCIES_HZ / FS_HZ
    _, response = signal.sosfreqz(sos, worN=omega)
    magnitude = np.abs(response)
    db = 20.0 * np.log10(np.maximum(magnitude, np.finfo(float).tiny))
    return list(zip(FREQUENCIES_HZ.tolist(), magnitude.tolist(), db.tolist()))


def group_delay_table(sos: np.ndarray) -> list[tuple[float, float, float]]:
    b, a = signal.sos2tf(sos)
    result: list[tuple[float, float, float]] = []
    for frequency in [20.0, 30.0, 33.333333, 50.0, 76.666667, 80.0, 100.0]:
        omega = 2.0 * np.pi * frequency / FS_HZ
        _, delay_samples = signal.group_delay((b, a), w=[omega])
        delay = float(delay_samples[0])
        result.append((frequency, delay, delay * 1000.0 / FS_HZ))
    return result


def pole_summary(sos: np.ndarray) -> tuple[float, float]:
    poles64 = np.concatenate([np.roots(row[3:]) for row in sos])
    sos32 = sos.astype(np.float32).astype(np.float64)
    poles32 = np.concatenate([np.roots(row[3:]) for row in sos32])
    return float(np.max(np.abs(poles64))), float(np.max(np.abs(poles32)))


def format_coefficients(sos32: np.ndarray) -> str:
    lines = []
    for index, row in enumerate(sos32, start=1):
        values = ", ".join(f"{float(value):.10g}f" for value in row)
        lines.append(f"SOS {index}: {values}")
    return "\n".join(lines)


def build_report() -> str:
    sos64 = design()
    sos32 = sos64.astype(np.float32)
    response64 = response_table(sos64)
    response32 = response_table(sos32.astype(np.float64))
    pole64, pole32 = pole_summary(sos64)

    work_freq = np.linspace(33.333333, 76.666667, 2000)
    _, h_work = signal.sosfreqz(sos32.astype(np.float64), worN=2*np.pi*work_freq/FS_HZ)
    work_db = 20.0 * np.log10(np.maximum(np.abs(h_work), np.finfo(float).tiny))

    lines = [
        "# Fan BPF design verification",
        "",
        f"- Sample rate: {FS_HZ:.0f} Hz",
        f"- Filter: fourth-order Butterworth band-pass ({SCIPY_ORDER} scipy prototype order)",
        f"- Cutoffs: {LOW_HZ:.0f} Hz to {HIGH_HZ:.0f} Hz",
        "- Runtime form: two float32 SOS, DF2T",
        f"- Maximum float64 pole radius: {pole64:.9f}",
        f"- Maximum float32 pole radius: {pole32:.9f}",
        f"- 33.33 to 76.67 Hz float32 gain range: {work_db.min():.4f} to {work_db.max():.4f} dB",
        "",
        "## Float32 SOS coefficients",
        "",
        "`[b0, b1, b2, a0, a1, a2]` using scipy's denominator convention.",
        "",
        "```text",
        format_coefficients(sos32),
        "```",
        "",
        "## Frequency response",
        "",
        "| Frequency (Hz) | Gain float64 | dB float64 | dB float32 |",
        "|---:|---:|---:|---:|",
    ]
    for (f, gain64, db64), (_, _, db32) in zip(response64, response32):
        label = "DC approximation" if f == 0.1 else f"{f:.3f}"
        lines.append(f"| {label} | {gain64:.8f} | {db64:.3f} | {db32:.3f} |")

    lines.extend([
        "",
        "## Group delay",
        "",
        "| Frequency (Hz) | Samples | Milliseconds |",
        "|---:|---:|---:|",
    ])
    for frequency, samples, milliseconds in group_delay_table(sos64):
        lines.append(f"| {frequency:.3f} | {samples:.3f} | {milliseconds:.3f} |")

    lines.extend([
        "",
        "The runtime C implementation uses `state1 = b1*x - a1*y + state2` and",
        "`state2 = b2*x - a2*y`, matching scipy's SOS sign convention.",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, help="Write the Markdown report to this path")
    args = parser.parse_args()

    report = build_report()
    if args.output:
        args.output.write_text(report, encoding="utf-8")
    print(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
