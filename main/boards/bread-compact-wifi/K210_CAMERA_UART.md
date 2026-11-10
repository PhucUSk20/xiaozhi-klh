# K210 camera over UART

`self.camera.take_photo` is registered automatically because the board exposes a
`K210UartCamera` through `GetCamera()`. When the tool runs, the ESP32-S3 asks the
K210 for one JPEG over UART and then uploads that JPEG to the vision URL using
the ESP32-S3 device session and token.

## Wiring

| K210 Maix Dock | ESP32-S3 bread-compact-wifi |
| --- | --- |
| IO6 (UART2 TX) | GPIO18 (UART1 RX) |
| IO7 (UART2 RX) | GPIO17 (UART1 TX) |
| GND | GND |

Both sides use 3.3 V logic. Do not connect UART TX or RX to 5 V.

This uses the proven K210 AI Extension path: UART2 RX=IO7, TX=IO6 at 115200
baud. IO6 and IO7 on the M1W are also connected to its onboard ESP8285, so the
companion holds the unused ESP8285 disabled via IO8 before assigning UART2. The
ESP32-S3 owns the authenticated vision connection; do not try to use the K210's
ESP8285 at the same time with this pin mapping.

## K210 program

Open `k210_camera_uart.py` in CanMV/MaixPy IDE and run it on the K210. For
automatic startup, save the same file as `boot.py` on the K210 filesystem.
The CanMV 1.0.7 firmware exposes GPIO from the lowercase `maix` module; Python
module names on this firmware are case-sensitive.

Both programs default to 115200 baud. If the wiring is long or unreliable,
change `K210_CAMERA_UART_BAUD_RATE` in `config.h` and `UART_BAUD` in the Python
file to the same lower value.

## Vision endpoint

The K210 never connects to Flask or any self-hosted endpoint. The central
Xiaozhi server sends its vision URL and bearer token to the S3 in MCP
capabilities. The S3 then uploads the JPEG to that URL as an authenticated
multipart request using its device and client IDs.

After startup, the S3 automatically sends a CRC-protected ping every two
seconds until the K210 returns a matching pong. The S3 then sends a final link
acknowledgement. `S3 LINK OK - TWO WAY` on the K210 LCD and `K210 UART link
confirmed` in the S3 log prove that TX and RX work in both directions without
calling the photo tool. If the LCD stays at `WAIT PING ... RX:0`, check the S3
TX to K210 RX wire and common ground. A growing `RX` count without `KCAM
COMMAND RECEIVED` indicates mismatched baud/protocol or a noisy UART line.
`SENT #...` confirms that a JPEG was returned to the ESP32-S3.

The UART packet contains a magic value, protocol version, packet type, request
ID, payload length, and CRC32. A JPEG is accepted only when the request ID,
length, checksum, and JPEG start/end markers are valid.
