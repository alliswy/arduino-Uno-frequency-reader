"""Convert Messkoffer .BIN logs (text header + binary X,Y,Z) to CSV."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from read_bin import configured_rate, parse_header

SAMPLES_PER_CHUNK = 8192


def default_csv_path(bin_path: Path) -> Path:
    return bin_path.with_suffix(".CSV")


def convert_bin_to_csv(
    bin_path: Path,
    csv_path: Path,
    rate_hz: float | None = None,
    show_progress: bool = True,
) -> int:
    raw = bin_path.read_bytes()
    meta, data = parse_header(raw)

    if len(data) % 6 != 0:
        raise ValueError(f"{bin_path.name}: binary size {len(data)} is not a multiple of 6 bytes")

    rate = rate_hz if rate_hz is not None else configured_rate(meta)
    n_samples = len(data) // 6
    dt_ms = 1000.0 / rate

    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", encoding="utf-8", newline="\n") as out:
        for key, val in meta.items():
            out.write(f"# {key}: {val}\n")
        out.write("Sample,time_ms,X_raw,Y_raw,Z_raw\n")

        written = 0
        offset = 0
        while offset < len(data):
            chunk_bytes = data[offset : offset + SAMPLES_PER_CHUNK * 6]
            offset += len(chunk_bytes)
            n_chunk = len(chunk_bytes) // 6
            if n_chunk == 0:
                break

            values = struct.unpack(f"<{n_chunk * 3}h", chunk_bytes)
            lines: list[str] = []
            for i in range(n_chunk):
                idx = written + i
                p = i * 3
                t_ms = idx * dt_ms
                lines.append(
                    f"{idx},{t_ms:.2f},{values[p]},{values[p + 1]},{values[p + 2]}\n"
                )
            out.writelines(lines)
            written += n_chunk

            if show_progress and written % (SAMPLES_PER_CHUNK * 10) == 0:
                pct = 100.0 * written / n_samples
                print(f"  {written}/{n_samples} samples ({pct:.1f}%)")

    if show_progress:
        print(f"Wrote {written} samples -> {csv_path}")
        print(f"Time axis uses {rate:.0f} Hz from {'--rate' if rate_hz else 'file header'}")

    return written


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert Messkoffer .BIN to CSV")
    parser.add_argument("input", type=Path, help="Input .BIN file")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Output .CSV path (default: same name as input)",
    )
    parser.add_argument(
        "--rate",
        type=float,
        default=None,
        help="Sample rate for time_ms column (default: Rate from file header)",
    )
    parser.add_argument("-q", "--quiet", action="store_true", help="No progress output")
    args = parser.parse_args()

    if not args.input.is_file():
        raise SystemExit(f"File not found: {args.input}")

    out = args.output if args.output is not None else default_csv_path(args.input)
    convert_bin_to_csv(args.input, out, rate_hz=args.rate, show_progress=not args.quiet)


if __name__ == "__main__":
    main()
