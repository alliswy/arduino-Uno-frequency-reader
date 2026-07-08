"""Plot X/Y/Z from Messkoffer .BIN files (single file, each file, or concatenated)."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.widgets import TextBox

from read_bin import configured_rate, load_bin, read_meta


def resolve_plot_rate(
    meta: dict[str, str],
    rate_hz: float | None,
    effective_rate: float | None,
) -> float:
    if rate_hz is not None:
        return rate_hz
    if effective_rate is not None:
        return effective_rate
    return configured_rate(meta)


def default_max_points(rate_hz: float) -> int:
    # ~30 s at full resolution when zoomed (scales with header rate: 1600, 3200, ...)
    return min(250_000, max(80_000, int(rate_hz * 30)))


def print_load_info(meta: dict[str, str], n: int, rate_hz: float) -> None:
    cfg = configured_rate(meta)
    dur = n / rate_hz if rate_hz > 0 and n else 0.0
    print(f"Header rate: {cfg:.0f} Hz | plot time axis: {rate_hz:.0f} Hz | samples: {n} | duration: {dur:.2f} s")
    if rate_hz != cfg:
        print(f"Note: plotting at {rate_hz:.0f} Hz (header says {cfg:.0f} Hz)")
    elif cfg >= 1600:
        print("High-rate file — zoom in for waveform detail; use --effective-rate if Serial rate differed")


def list_bin_files(folder: Path) -> list[Path]:
    files = sorted(folder.glob("*.BIN"))
    if not files:
        raise FileNotFoundError(f"No .BIN files in {folder}")
    return files


def load_concatenated(
    paths: list[Path],
    rate_hz: float | None = None,
) -> tuple[list[dict[str, str]], float, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    metas: list[dict[str, str]] = []
    chunks_t: list[np.ndarray] = []
    chunks_x: list[np.ndarray] = []
    chunks_y: list[np.ndarray] = []
    chunks_z: list[np.ndarray] = []
    sample_idx = 0
    rate = rate_hz

    for path in paths:
        meta, _t, x, y, z = load_bin(path, rate_hz=rate_hz)
        metas.append(meta)
        file_rate = rate_hz if rate_hz is not None else configured_rate(meta)
        if rate is None:
            rate = file_rate
        elif file_rate != rate:
            print(
                f"WARNING: {path.name} header rate {file_rate:.0f} Hz "
                f"differs from first file {rate:.0f} Hz — time axis uses per-file header rate"
            )
        n = len(x)
        t = (np.arange(n, dtype=np.float64) + sample_idx) / file_rate
        sample_idx += n
        chunks_t.append(t)
        chunks_x.append(x)
        chunks_y.append(y)
        chunks_z.append(z)

    if not metas:
        raise ValueError("No files to load")
    if rate is None:
        rate = configured_rate(metas[0])

    return (
        metas,
        float(rate),
        np.concatenate(chunks_t),
        np.concatenate(chunks_x),
        np.concatenate(chunks_y),
        np.concatenate(chunks_z),
    )


def decimate(
    t: np.ndarray,
    x: np.ndarray,
    y: np.ndarray,
    z: np.ndarray,
    max_points: int,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    n = len(t)
    if n <= max_points:
        return t, x, y, z
    step = int(np.ceil(n / max_points))
    return t[::step], x[::step], y[::step], z[::step]


def select_window(
    t: np.ndarray,
    x: np.ndarray,
    y: np.ndarray,
    z: np.ndarray,
    start_sec: float,
    seconds: float | None,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    if seconds is None:
        mask = t >= start_sec
    else:
        mask = (t >= start_sec) & (t < start_sec + seconds)
    return t[mask], x[mask], y[mask], z[mask]


def _plot_lines(ax, t: np.ndarray, x: np.ndarray, y: np.ndarray, z: np.ndarray, axis: str) -> None:
    if axis in ("x", "all"):
        ax.plot(t, x, label="X", alpha=0.85, linewidth=0.8)
    if axis in ("y", "all"):
        ax.plot(t, y, label="Y", alpha=0.85, linewidth=0.8)
    if axis in ("z", "all"):
        ax.plot(t, z, label="Z", alpha=0.85, linewidth=0.8)


def _fmt_axis(v: float) -> str:
    return f"{v:.6g}"


def plot_xyz_static(
    t: np.ndarray,
    x: np.ndarray,
    y: np.ndarray,
    z: np.ndarray,
    title: str,
    axis: str,
    ylabel: str,
    save: Path,
    xlim: tuple[float, float] | None = None,
    ylim: tuple[float, float] | None = None,
) -> None:
    fig, ax = plt.subplots(figsize=(12, 4))
    _plot_lines(ax, t, x, y, z, axis)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    if xlim is not None:
        ax.set_xlim(xlim)
    if ylim is not None:
        ax.set_ylim(ylim)
    else:
        ax.autoscale(enable=True, axis="y")
    ax.legend()
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    save.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(save, dpi=150)
    print(f"Saved {save}")
    plt.close(fig)


class InteractiveVibrationPlot:
    """Zoomable plot: re-draws the visible time window at higher resolution when zoomed in."""

    def __init__(
        self,
        t: np.ndarray,
        x: np.ndarray,
        y: np.ndarray,
        z: np.ndarray,
        title: str,
        axis: str,
        ylabel: str,
        max_points: int,
        xlim: tuple[float, float] | None = None,
        ylim: tuple[float, float] | None = None,
    ) -> None:
        self.t = t
        self.x = x
        self.y = y
        self.z = z
        self.title = title
        self.axis = axis
        self.ylabel = ylabel
        self.max_points = max_points
        self._updating = False
        self._t_min = float(t[0])
        self._t_max = float(t[-1])
        self._y_autoscale = ylim is None

        self.fig = plt.figure(figsize=(12, 5))
        self.ax = self.fig.add_axes([0.08, 0.30, 0.88, 0.62])
        init_x = xlim if xlim is not None else (self._t_min, self._t_max)
        self.ax.set_xlim(init_x)
        if ylim is not None:
            self.ax.set_ylim(ylim)

        self._boxes: dict[str, TextBox] = {}
        self._add_limit_boxes()

        self.fig.canvas.mpl_connect("scroll_event", self._on_scroll)
        self.fig.canvas.mpl_connect("key_press_event", self._on_key)
        self.ax.callbacks.connect("xlim_changed", self._on_xlim_changed)
        self._redraw()
        self.fig.text(
            0.5,
            0.02,
            "Scroll = zoom time · edit limits below + Enter · R = reset view",
            ha="center",
            fontsize=9,
            color="0.45",
        )
        plt.show()

    def _add_limit_boxes(self) -> None:
        x0, x1 = self.ax.get_xlim()
        y0, y1 = self.ax.get_ylim()
        specs = [
            ("t min", "xmin", 0.10, _fmt_axis(x0)),
            ("t max", "xmax", 0.28, _fmt_axis(x1)),
            ("y min", "ymin", 0.50, _fmt_axis(y0)),
            ("y max", "ymax", 0.68, _fmt_axis(y1)),
        ]
        for label, key, left, initial in specs:
            axbox = self.fig.add_axes([left, 0.12, 0.14, 0.05])
            box = TextBox(axbox, label, initial=initial)
            box.on_submit(lambda text, k=key: self._apply_limit(k, text))
            self._boxes[key] = box

    def _sync_limit_boxes(self) -> None:
        x0, x1 = self.ax.get_xlim()
        y0, y1 = self.ax.get_ylim()
        values = {
            "xmin": _fmt_axis(x0),
            "xmax": _fmt_axis(x1),
            "ymin": _fmt_axis(y0),
            "ymax": _fmt_axis(y1),
        }
        for key, box in self._boxes.items():
            box.set_val(values[key])

    def _apply_limit(self, key: str, text: str) -> None:
        try:
            val = float(text.strip())
        except ValueError:
            return
        x0, x1 = self.ax.get_xlim()
        y0, y1 = self.ax.get_ylim()
        if key == "xmin" and val < x1:
            self.ax.set_xlim(val, x1)
        elif key == "xmax" and val > x0:
            self.ax.set_xlim(x0, val)
        elif key == "ymin" and val < y1:
            self._y_autoscale = False
            self.ax.set_ylim(val, y1)
            self._redraw()
        elif key == "ymax" and val > y0:
            self._y_autoscale = False
            self.ax.set_ylim(y0, val)
            self._redraw()

    def reset_view(self) -> None:
        self._y_autoscale = True
        self.ax.set_xlim(self._t_min, self._t_max)

    def _on_key(self, event) -> None:
        if event.key in ("r", "R", "home"):
            self.reset_view()

    def _slice_visible(self) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
        t_min, t_max = self.ax.get_xlim()
        if t_min > t_max:
            t_min, t_max = t_max, t_min
        mask = (self.t >= t_min) & (self.t <= t_max)
        if not np.any(mask):
            return self.t[:0], self.x[:0], self.y[:0], self.z[:0]
        return self.t[mask], self.x[mask], self.y[mask], self.z[mask]

    def _redraw(self) -> None:
        if self._updating:
            return
        self._updating = True
        xlim = self.ax.get_xlim()
        ylim = None if self._y_autoscale else self.ax.get_ylim()

        t_vis, x_vis, y_vis, z_vis = self._slice_visible()
        n_vis = len(t_vis)
        if n_vis > self.max_points:
            t_vis, x_vis, y_vis, z_vis = decimate(
                t_vis, x_vis, y_vis, z_vis, self.max_points
            )
        detail = f"{n_vis} samples in view"
        if n_vis > self.max_points:
            detail += f", drawing {len(t_vis)}"

        self.ax.cla()
        _plot_lines(self.ax, t_vis, x_vis, y_vis, z_vis, self.axis)
        self.ax.set_xlabel("Time (s)")
        self.ax.set_ylabel(self.ylabel)
        self.ax.set_title(f"{self.title}\n({detail})")
        self.ax.legend(loc="upper right")
        self.ax.grid(True, alpha=0.3)
        self.ax.set_xlim(xlim)
        if self._y_autoscale:
            self.ax.autoscale(enable=True, axis="y")
        elif ylim is not None:
            self.ax.set_ylim(ylim)
        self._sync_limit_boxes()
        self._updating = False

    def _on_xlim_changed(self, _ax) -> None:
        self._redraw()

    def _on_scroll(self, event) -> None:
        if event.inaxes is not self.ax or event.xdata is None:
            return
        cur = self.ax.get_xlim()
        width = cur[1] - cur[0]
        if width <= 0:
            return
        scale = 0.75 if event.button == "up" else 1.0 / 0.75
        new_width = width * scale
        rel = (event.xdata - cur[0]) / width
        rel = min(max(rel, 0.0), 1.0)
        self.ax.set_xlim(event.xdata - new_width * rel, event.xdata + new_width * (1.0 - rel))


def plot_xyz(
    t: np.ndarray,
    x: np.ndarray,
    y: np.ndarray,
    z: np.ndarray,
    title: str,
    axis: str,
    ylabel: str,
    save: Path | None,
    max_points: int,
    interactive: bool,
    xlim: tuple[float, float] | None = None,
    ylim: tuple[float, float] | None = None,
) -> None:
    if save is not None:
        t_d, x_d, y_d, z_d = decimate(t, x, y, z, max_points)
        plot_xyz_static(t_d, x_d, y_d, z_d, title, axis, ylabel, save, xlim, ylim)
    elif interactive:
        InteractiveVibrationPlot(
            t, x, y, z, title, axis, ylabel, max_points, xlim=xlim, ylim=ylim
        )
    else:
        t_d, x_d, y_d, z_d = decimate(t, x, y, z, max_points)
        fig, ax = plt.subplots(figsize=(12, 4))
        _plot_lines(ax, t_d, x_d, y_d, z_d, axis)
        ax.set_xlabel("Time (s)")
        ax.set_ylabel(ylabel)
        ax.set_title(title)
        if xlim is not None:
            ax.set_xlim(xlim)
        if ylim is not None:
            ax.set_ylim(ylim)
        else:
            ax.autoscale(enable=True, axis="y")
        ax.legend()
        ax.grid(True, alpha=0.3)
        plt.tight_layout()
        plt.show()


def apply_scale(
    x: np.ndarray,
    y: np.ndarray,
    z: np.ndarray,
    units: str,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, str]:
    if units == "raw":
        return x, y, z, "Raw ADXL345 counts"
    if units == "g":
        scale = 0.0039
        return x * scale, y * scale, z * scale, "Acceleration (g)"
    if units == "ms2":
        scale = 0.0039 * 9.81
        return x * scale, y * scale, z * scale, "Acceleration (m/s²)"
    raise ValueError(f"Unknown units: {units}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Plot Messkoffer .BIN vibration data (X/Y/Z vs time)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  One file name -> plots ALL .BIN files in that folder together:
    python plot_bin.py "E:\\2nd run\\26061622.BIN"

  Only that single file (not the whole folder):
    python plot_bin.py "E:\\2nd run\\26061622.BIN" --only

  Entire folder:
    python plot_bin.py --folder "E:\\2nd run"

  Specific files only (in order):
    python plot_bin.py "E:\\a.BIN" "E:\\b.BIN" "E:\\c.BIN"

  Each file as its own plot:
    python plot_bin.py --folder "E:\\2nd run" --each --save-dir "E:\\plots"
""",
    )
    parser.add_argument("files", type=Path, nargs="*", help="One or more .BIN files")
    parser.add_argument("--folder", type=Path, default=None, help="Folder with .BIN files")
    parser.add_argument(
        "--only",
        action="store_true",
        help="Plot only the given file(s), not every .BIN in the folder",
    )
    parser.add_argument(
        "--together",
        action="store_true",
        help="(Legacy) Same as default combine behaviour",
    )
    parser.add_argument(
        "--each",
        action="store_true",
        help="Plot each file separately (use with --folder or multiple files)",
    )
    parser.add_argument("--seconds", type=float, default=None, help="Plot length from --start (default: all)")
    parser.add_argument("--start", type=float, default=0.0, help="Start time in seconds (default: 0)")
    parser.add_argument(
        "--rate",
        type=float,
        default=None,
        help="Sample rate for time axis (default: # Rate from each file header)",
    )
    parser.add_argument(
        "--effective-rate",
        type=float,
        default=None,
        help="Effective Hz from Serial stop (overrides header if --rate not set)",
    )
    parser.add_argument("--axis", choices=("x", "y", "z", "all"), default="all")
    parser.add_argument(
        "--units",
        choices=("raw", "g", "ms2"),
        default="g",
        help="Y-axis units (default: g, using 3.9 mg/LSB)",
    )
    parser.add_argument(
        "--max-points",
        type=int,
        default=None,
        help="Max points per visible window when zoomed (default: ~30 s at file rate, min 80000)",
    )
    parser.add_argument(
        "--no-interactive",
        action="store_true",
        help="Static plot only (no scroll zoom / auto detail on zoom)",
    )
    parser.add_argument("--xmin", type=float, default=None, help="Time axis minimum (seconds)")
    parser.add_argument("--xmax", type=float, default=None, help="Time axis maximum (seconds)")
    parser.add_argument("--ymin", type=float, default=None, help="Y axis minimum (g, m/s², or raw)")
    parser.add_argument("--ymax", type=float, default=None, help="Y axis maximum (g, m/s², or raw)")
    parser.add_argument("--save", type=Path, default=None, help="Save single plot to PNG")
    parser.add_argument(
        "--save-dir",
        type=Path,
        default=None,
        help="Save plots to folder (--each mode)",
    )
    args = parser.parse_args()

    xlim: tuple[float, float] | None = None
    ylim: tuple[float, float] | None = None
    if args.xmin is not None or args.xmax is not None:
        if args.xmin is None or args.xmax is None:
            parser.error("Set both --xmin and --xmax, or neither")
        if args.xmin >= args.xmax:
            parser.error("--xmin must be less than --xmax")
        xlim = (args.xmin, args.xmax)
    if args.ymin is not None or args.ymax is not None:
        if args.ymin is None or args.ymax is None:
            parser.error("Set both --ymin and --ymax, or neither")
        if args.ymin >= args.ymax:
            parser.error("--ymin must be less than --ymax")
        ylim = (args.ymin, args.ymax)

    if args.only and args.folder is not None:
        parser.error("Use --folder or a file path, not both with --only")

    paths: list[Path] = []

    if args.folder is not None:
        paths = list_bin_files(args.folder)
    elif len(args.files) == 1 and not args.only:
        folder = args.files[0].parent
        if not args.files[0].is_file():
            parser.error(f"File not found: {args.files[0]}")
        paths = list_bin_files(folder)
        print(f"All .BIN in folder: {folder} ({len(paths)} files)")
    elif args.files:
        for p in args.files:
            if not p.is_file():
                parser.error(f"File not found: {p}")
        paths = list(args.files)
    else:
        parser.error("Provide a .BIN file path or --folder")

    paths = sorted({p.resolve(): p for p in paths}.values(), key=lambda p: p.name.upper())

    if args.together and args.each:
        parser.error("Use either --together or --each, not both")

    plot_rate_arg = args.rate
    effective_rate_arg = args.effective_rate

    if args.each:
        if len(paths) == 1:
            parser.error("Only one file — omit --each")
        for path in paths:
            meta = read_meta(path)
            plot_rate = resolve_plot_rate(meta, plot_rate_arg, effective_rate_arg)
            max_points = args.max_points if args.max_points is not None else default_max_points(plot_rate)
            meta, t, x, y, z = load_bin(
                path,
                rate_hz=plot_rate,
                start_sec=args.start,
                seconds=args.seconds,
            )
            print_load_info(meta, len(t), plot_rate)
            x, y, z, ylabel = apply_scale(x, y, z, args.units)
            start = meta.get("Start", "")
            dur = t[-1] if len(t) else 0.0
            title = f"{path.name}  ({start})  {dur:.1f}s @ {plot_rate:.0f} Hz"
            save = args.save_dir / f"{path.stem}.png" if args.save_dir else None
            plot_xyz(
                t, x, y, z, title, args.axis, ylabel, save,
                max_points=max_points,
                interactive=not args.no_interactive,
                xlim=xlim,
                ylim=ylim,
            )
        return

    if len(paths) > 1 or args.together:
        metas, rate, t, x, y, z = load_concatenated(
            paths, rate_hz=plot_rate_arg or effective_rate_arg
        )
        plot_rate = resolve_plot_rate(metas[0], plot_rate_arg, effective_rate_arg)
        max_points = args.max_points if args.max_points is not None else default_max_points(plot_rate)
        folder_name = paths[0].parent.name
        names = ", ".join(p.name for p in paths[:3])
        if len(paths) > 3:
            names += f", ... (+{len(paths) - 3} more)"
        print(f"Combined {len(paths)} files -> {len(t)} samples, {t[-1]:.1f} s @ {plot_rate:.0f} Hz")
        if plot_rate >= 1600 and len(t) > 500_000:
            print("Large high-rate plot — consider --only, --start, or --seconds")
        title = f"{folder_name}: {len(paths)} files, {t[-1]:.0f} s @ {plot_rate:.0f} Hz ({names})"
        t, x, y, z = select_window(t, x, y, z, args.start, args.seconds)
    else:
        path = paths[0]
        meta = read_meta(path)
        plot_rate = resolve_plot_rate(meta, plot_rate_arg, effective_rate_arg)
        max_points = args.max_points if args.max_points is not None else default_max_points(plot_rate)
        meta, t, x, y, z = load_bin(
            path,
            rate_hz=plot_rate,
            start_sec=args.start,
            seconds=args.seconds,
        )
        print_load_info(meta, len(t), plot_rate)
        start = meta.get("Start", "")
        dur = t[-1] if len(t) else 0.0
        title = f"{path.name}  ({start})  {dur:.1f}s @ {plot_rate:.0f} Hz"

    x, y, z, ylabel = apply_scale(x, y, z, args.units)
    plot_xyz(
        t, x, y, z, title, args.axis, ylabel, args.save,
        max_points=max_points,
        interactive=not args.no_interactive,
        xlim=xlim,
        ylim=ylim,
    )


if __name__ == "__main__":
    main()
