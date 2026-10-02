
# MiaSeriaPordo
# Serial Port Terminal (GTK)

A lightweight, cross-platform serial port terminalIt is built
with GTK 3 and runs on Linux and Windows.
---
<img width="549" height="592" alt="MiaSeriaPordoV5 0" src="https://github.com/user-attachments/assets/b8e7b926-5bdb-40f5-9f69-0abed91529e1" />

## What it does

- **Enumerate and connect** to serial ports (`/dev/ttyUSB*`, `/dev/ttyACM*`,
  `/dev/ttyS*`, `COM1…COM256`, etc.)
- **Full UART configuration**: baud rate (300 → 921600), data bits (5/6/7/8),
  parity (None / Odd / Even), stop bits (1/2)
- **Two view modes**:
  - **ASCII** — control characters are shown as escape tokens (`\n`, `\r`,
    `\t`, `\0`, `\XX`) with a distinct colour so they stand out
  - **HEX** — plain hex dump, with an automatic line break on your chosen
    end-of-line sequence
- **Send line** with a selectable line terminator (`\n`, `\n\r`, `\r\n`, `\r`)
- **Pause / resume** the display while the port keeps reading in the
  background (useful when you want to freeze a burst of data)
- **Clear** button for the log window
- **Show descriptor** — prints the current port settings and buffer size
- **Ctrl + mouse-wheel** over the log adjusts the font size
- **Auto-connect** on startup (optional)
- **Remembers everything**: port, baud, framing, newline, view mode, window
  size and position, and font size are restored next time you launch.

## Requirements

### Linux
- GTK 3
- `libconfig`
- The usual dev packages: `build-essential`, `pkg-config`

On Debian/Ubuntu:

```bash
sudo apt install build-essential pkg-config libgtk-3-dev libconfig-dev
