# Messkoffer — ADXL345 Vibration Logger

Portable high-rate vibration data logger for field measurements. An Arduino reads a triple-axis **ADXL345** accelerometer, timestamps recordings with a **DS3231** real-time clock, and writes data to an **SD card**. Companion Python scripts read, convert, and plot the binary log files on a PC.

## Hardware

| Component | Role |
|-----------|------|
| Arduino UNO R4 WiFi (or Mega) | Main controller |
| ADXL345 | 3-axis accelerometer (I2C, address `0x53`) |
| DS3231 | Real-time clock (I2C, address `0x68`) |
| SD card module | Data storage (CS on pin 10, FAT32) |

## Repository layout

```
arduino_logger/
├── Messkoffer_v0.6/
│   └── Messkoffer_v0.6.ino   # Arduino firmware (internal version v0.7)
├── read_bin.py               # Read .BIN files, quick plot, data-loss check
├── bin_to_csv.py             # Convert .BIN to CSV
├── plot_bin.py               # Interactive plotting (single file, folder, or combined)
└── README.md
```

## Firmware features

- **Sample rates:** 100, 200, 400, 800, 1600, or 3200 Hz
- **Measurement range:** ±2, ±4, ±8, or ±16 g
- **Output formats:**
  - **Binary (`.BIN`)** — recommended for high rates; text header + raw int16 X/Y/Z samples
  - **CSV (`.CSV`)** — human-readable; practical up to ~800 Hz only
- **Auto-start** on boot (optional)
- **File rotation** — new file every N minutes (default: 10 min)
- **Session notes** — stored in the file header (e.g. test frequency, sweep ID)
- **Robust logging:** I2C retries, SD write retries, RAM buffering, periodic flush
- **Diagnostics:** tracks samples read vs written, FIFO overflows, SD/I2C errors, and effective logging rate

### Binary file format

Each `.BIN` file contains:

1. A text header with metadata (`Start`, `Session`, `Rate`, `Range`, scaling info)
2. A `---DATA---` marker
3. Binary payload: int16 **X, Y, Z** interleaved, little-endian (3.9 mg/LSB in full-resolution mode)

Filenames use the RTC timestamp when available (e.g. `06071430.BIN`), otherwise `DAT0001.BIN`.

### Serial commands (115200 baud)

Open the Serial Monitor at **115200 baud** and send single-character commands:

| Key | Action |
|-----|--------|
| `s` | Start recording |
| `x` | Stop recording |
| `c` | SD card self-test |
| `i` | Show current settings |
| `t` | Sensor test (read FIFO samples) |
| `z` | Print current RTC time |
| `r` | Set RTC from sketch compile time |
| `w` | Set RTC manually (`DD.MM.YY HH:MM:SS`) |
| `n` | Set session note (stored in file header) |

### Compile-time settings

Edit the `#define` values at the top of `Messkoffer_v0.6.ino`:

| Setting | Description | Default |
|---------|-------------|---------|
| `CFG_RATE` | Sample rate (Hz) | 1600 |
| `CFG_RANGE` | Range (±g) | 16 |
| `CFG_BINARY` | 1 = `.BIN`, 0 = `.CSV` | 1 |
| `CFG_AUTOSTART` | Start logging on boot | 1 |
| `CFG_ROTATION` | Minutes per file (0 = off) | 10 |
| `CFG_DURATION` | Max recording length in seconds (0 = unlimited) | 0 |

## Python tools

Requires Python 3.10+ and:

```bash
pip install numpy matplotlib
```

### `read_bin.py` — inspect and quick-plot a single file

```bash
python read_bin.py path/to/file.BIN
python read_bin.py path/to/file.BIN --check --duration 120
python read_bin.py --scan path/to/folder --duration 120
```

Useful options: `--axis x|y|z|all`, `--seconds 5`, `--check` (data-loss report only), `--duration` and `--effective-rate` (values from the Serial stop message).

### `bin_to_csv.py` — convert binary logs to CSV

```bash
python bin_to_csv.py path/to/file.BIN
python bin_to_csv.py path/to/file.BIN -o output.CSV
python bin_to_csv.py path/to/file.BIN --rate 1600
```

Output columns: `Sample, time_ms, X_raw, Y_raw, Z_raw`.

### `plot_bin.py` — interactive plots

```bash
# Single file
python plot_bin.py path/to/file.BIN

# All .BIN files in a folder (combined into one plot)
python plot_bin.py --folder path/to/folder

# Each file as its own plot
python plot_bin.py --folder path/to/folder --each --save-dir path/to/plots

# Time window and units
python plot_bin.py path/to/file.BIN --start 10 --seconds 30 --units g
```

Useful options: `--axis`, `--units raw|g|ms2`, `--rate`, `--save plot.png`, `--no-interactive`.

## Typical workflow

1. Flash `Messkoffer_v0.6.ino` to the Arduino (Arduino IDE or `arduino-cli`).
2. Insert a FAT32 SD card.
3. Optionally set the RTC (`w`) and a session note (`n`).
4. Start recording with `s`, or let auto-start handle it.
5. Stop with `x` and review the Serial diagnostics.
6. Copy `.BIN` files from the SD card to your PC.
7. Use the Python scripts to convert, inspect, or plot the data.

## Notes

- Do not commit large `.BIN` or `.CSV` measurement files to git — upload code only.
- CSV mode at rates above 800 Hz will lose data; use binary mode for 1600 Hz and 3200 Hz.
- When stopping a recording, note the **Duration** and **Rate on SD** from the Serial output — pass them to `read_bin.py --check` to verify data integrity.
