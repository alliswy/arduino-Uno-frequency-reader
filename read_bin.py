"""Read Messkoffer v0.7 .BIN vibration logs (text header + binary X,Y,Z)."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


def parse_header(raw: bytes) -> tuple[dict[str, str], bytes]:
    marker = b"---DATA---"
    idx = raw.find(marker)
    if idx < 0:
        raise ValueError("Missing ---DATA--- marker in file")
    head = raw[:idx]
    data = raw[idx + len(marker) :]
    if data.startswith(b"\r\n"):
        data = data[2:]
    elif data.startswith(b"\n"):
        data = data[1:]
    meta: dict[str, str] = {}
    for line in head.decode("utf-8", errors="replace").splitlines():
        if line.startswith("# ") and ":" in line:
            key, val = line[2:].split(":", 1)
            meta[key.strip()] = val.strip()
    return meta, data


def configured_rate(meta: dict[str, str]) -> float:
    raw = meta.get("Rate")
    if not raw:
        raise ValueError("Missing '# Rate: ...' in file header — use --rate on the command line")
    rate_match = re.search(r"(\d+)", raw)
    if not rate_match:
        raise ValueError(f"Cannot parse Rate from header: {raw!r}")
    return float(rate_match.group(1))


def check_data_loss(
    n_samples: int,
    cfg_rate: float,
    duration_sec: float | None = None,
    effective_rate: float | None = None,
) -> None:
    print("\n=== Data loss check ===")

    if duration_sec is not None and duration_sec > 0:
        expected = duration_sec * cfg_rate
        captured_pct = 100.0 * n_samples / expected
        loss_pct = 100.0 - captured_pct
        eff = n_samples / duration_sec
        print(f"Recording duration (from Serial stop): {duration_sec:.1f} s")
        print(f"Configured rate (header):             {cfg_rate:.0f} Hz")
        print(f"Expected samples (duration × rate):   {expected:.0f}")
        print(f"Samples in file:                      {n_samples}")
        print(f"Effective rate (samples / duration):  {eff:.1f} Hz")
        print(f"Captured:                             {captured_pct:.1f}%")
        print(f"Missing vs configured rate:           {loss_pct:.1f}%")
        if loss_pct <= 1.0:
            print("Verdict: excellent — within ~1% of configured rate")
        elif loss_pct <= 10.0:
            print("Verdict: good — small loss; use effective rate for analysis")
        else:
            print("Verdict: significant loss — analyze at effective rate, not header rate")
    elif effective_rate is not None and effective_rate > 0:
        implied_duration = n_samples / effective_rate
        expected = implied_duration * cfg_rate
        loss_pct = 100.0 * (1.0 - n_samples / expected)
        print(f"Effective rate (from Serial):         {effective_rate:.1f} Hz")
        print(f"Implied duration:                     {implied_duration:.1f} s")
        print(f"Configured rate (header):             {cfg_rate:.0f} Hz")
        print(f"Samples in file:                      {n_samples}")
        print(f"Missing vs configured rate:           {loss_pct:.1f}%")
        print("Tip: pass --duration from 'Dauer:' for a direct check")
    else:
        print("Need either:")
        print("  --duration 137.2     (from Serial: Dauer: 137.2 s)")
        print("  --effective-rate 2982 (from Serial: Effektive Rate: 2982 Hz)")
        print("Without that, the file alone cannot prove loss — it has no per-sample timestamps.")


def count_samples(path: Path) -> tuple[dict[str, str], int]:
    meta, data = parse_header(path.read_bytes())
    if len(data) % 6 != 0:
        raise ValueError(f"{path.name}: binary size {len(data)} not divisible by 6")
    return meta, len(data) // 6


def check_full_run(
    folder: Path,
    duration_sec: float,
    serial_samples: int | None = None,
    serial_ovf: int | None = None,
    serial_eff_rate: float | None = None,
) -> None:
    files = sorted(folder.glob("*.BIN"))
    if not files:
        print(f"No .BIN files in {folder}")
        return

    print(f"=== Full run check: {folder} ===")
    print(f"Files found: {len(files)}\n")

    total = 0
    cfg_rate: float | None = None
    for f in files:
        meta, n = count_samples(f)
        total += n
        rate = configured_rate(meta)
        if cfg_rate is None:
            cfg_rate = rate
        elif rate != cfg_rate:
            print(f"  WARNING: {f.name} has Rate {rate:.0f} Hz (first file had {cfg_rate:.0f})")

    if cfg_rate is None:
        print("No .BIN files with a valid Rate header found.")
        return

    eff = total / duration_sec
    expected = duration_sec * cfg_rate
    captured_pct = 100.0 * total / expected
    loss_pct = 100.0 - captured_pct

    print(f"Configured rate (from headers):     {cfg_rate:.0f} Hz")
    print(f"Recording duration (Serial Dauer):  {duration_sec:.1f} s ({duration_sec/3600:.2f} h)")
    print(f"Expected samples (duration × rate): {expected:.0f}")
    print(f"Samples in all .BIN files:          {total}")
    if serial_samples is not None:
        print(f"Samples (Serial stop message):      {serial_samples}")
        diff = total - serial_samples
        print(f"SD total vs Serial:                 {diff:+d} samples")
    if serial_eff_rate is not None:
        print(f"Effective rate (Serial):            {serial_eff_rate:.1f} Hz")
    print(f"Effective rate (files / duration):  {eff:.1f} Hz")
    print(f"Captured vs configured rate:        {captured_pct:.1f}%")
    print(f"Missing vs configured rate:         {loss_pct:.1f}%")
    if serial_ovf is not None:
        print(f"FIFO overflows (Serial OVF):        {serial_ovf}")
    print()
    if loss_pct <= 1.0:
        print("Verdict: excellent")
    elif loss_pct <= 10.0:
        print("Verdict: good — use effective rate for analysis")
    else:
        print("Verdict: significant loss — report effective rate, not header rate")
    print(f"\nPer-file average: {total / len(files):.0f} samples ({total / len(files) / eff:.1f} s @ {eff:.0f} Hz)")


def read_meta(path: Path) -> dict[str, str]:
    return parse_header(path.read_bytes())[0]


def load_bin(
    path: Path,
    rate_hz: float | None = None,
    start_sec: float = 0.0,
    seconds: float | None = None,
) -> tuple[dict[str, str], np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    raw = path.read_bytes()
    meta, data = parse_header(raw)
    if len(data) % 6 != 0:
        raise ValueError(f"Binary section size {len(data)} is not a multiple of 6 bytes")

    n_total = len(data) // 6
    rate = rate_hz if rate_hz is not None else configured_rate(meta)

    i0 = int(max(0.0, start_sec) * rate)
    if seconds is not None:
        i1 = int((max(0.0, start_sec) + max(0.0, seconds)) * rate)
    else:
        i1 = n_total
    i0 = min(i0, n_total)
    i1 = min(max(i1, i0), n_total)

    chunk = data[i0 * 6 : i1 * 6]
    n = len(chunk) // 6
    if n == 0:
        empty = np.empty(0, dtype=np.float64)
        return meta, empty, empty, empty, empty

    xyz = np.frombuffer(chunk, dtype="<i2").reshape(n, 3)
    x = xyz[:, 0].astype(np.float64, copy=False)
    y = xyz[:, 1].astype(np.float64, copy=False)
    z = xyz[:, 2].astype(np.float64, copy=False)
    t = (np.arange(n, dtype=np.float64) + i0) / rate
    return meta, t, x, y, z


def main() -> None:
    parser = argparse.ArgumentParser(description="Read/plot Messkoffer .BIN files")
    parser.add_argument("file", type=Path, nargs="?", help="Path to .BIN file")
    parser.add_argument("--scan", type=Path, default=None, help="Scan all .BIN files in folder (full run check)")
    parser.add_argument("--serial-samples", type=int, default=None, help="Samples from Serial stop message")
    parser.add_argument("--ovf", type=int, default=None, help="FIFO overflows from Serial stop message")
    parser.add_argument("--seconds", type=float, default=5.0, help="Seconds to plot from start")
    parser.add_argument("--axis", choices=("x", "y", "z", "all"), default="z")
    parser.add_argument("--check", action="store_true", help="Print data-loss report (no plot)")
    parser.add_argument(
        "--duration",
        type=float,
        default=None,
        help="Recording length in seconds (from Serial stop: Dauer: ... s)",
    )
    parser.add_argument(
        "--effective-rate",
        type=float,
        default=None,
        help="Effective Hz from Serial stop (Effektive Rate: ... Hz)",
    )
    parser.add_argument(
        "--plot-rate",
        type=float,
        default=None,
        help="Sample rate for time axis / FFT (default: effective rate if given, else header rate)",
    )
    parser.add_argument("--no-plot", action="store_true", help="Skip plot window")
    args = parser.parse_args()

    if args.scan is not None:
        if args.duration is None:
            parser.error("--scan requires --duration (Serial Dauer in seconds)")
        check_full_run(
            args.scan,
            args.duration,
            serial_samples=args.serial_samples,
            serial_ovf=args.ovf,
            serial_eff_rate=args.effective_rate,
        )
        return

    if args.file is None:
        parser.error("Provide a .BIN file or use --scan FOLDER")

    meta = parse_header(args.file.read_bytes())[0]
    cfg_rate = configured_rate(meta)
    plot_rate = args.plot_rate or args.effective_rate or cfg_rate

    meta, t, x, y, z = load_bin(args.file, rate_hz=plot_rate)
    n = len(t)

    print("Header:")
    for k, v in meta.items():
        print(f"  {k}: {v}")

    print(f"\nSamples in file: {n}")
    print(f"Duration @ {plot_rate:.0f} Hz (for plotting): {t[-1]:.2f} s")

    check_data_loss(n, cfg_rate, args.duration, args.effective_rate)

    if args.check or args.no_plot:
        return

    mask = t <= args.seconds
    fig, ax = plt.subplots(figsize=(10, 4))
    if args.axis in ("x", "all"):
        ax.plot(t[mask], x[mask], label="X", alpha=0.8)
    if args.axis in ("y", "all"):
        ax.plot(t[mask], y[mask], label="Y", alpha=0.8)
    if args.axis in ("z", "all"):
        ax.plot(t[mask], z[mask], label="Z", alpha=0.8)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Raw ADXL345 counts")
    ax.set_title(args.file.name)
    ax.legend()
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.show()


if __name__ == "__main__":
    main()
