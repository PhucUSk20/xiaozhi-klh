import sensor, image, time, lcd, struct, gc
from fpioa_manager import fm
from machine import UART
from maix import GPIO


# Sipeed Maix Dock M1/M1W UART camera for Xiaozhi bread-compact-wifi.
# K210 IO6 (UART2 TX) -> ESP32-S3 GPIO18 (RX)
# K210 IO7 (UART2 RX) <- ESP32-S3 GPIO17 (TX)
# K210 GND          --- ESP32-S3 GND
# Both boards use 3.3 V UART levels. Do not connect either signal to 5 V.
K210_TX_PIN = 6
K210_RX_PIN = 7
UART_BAUD = 115200
JPEG_QUALITY = 55

MAGIC = b"KCAM"
VERSION = 1
CAPTURE_REQUEST = 0x01
PING_REQUEST = 0x02
LINK_ACK = 0x03
JPEG_RESPONSE = 0x81
PONG_RESPONSE = 0x82
ERROR_RESPONSE = 0xFF
HEADER_SIZE = 20
SEND_CHUNK = 1024
WIFI_ENABLE_PIN = 8


def lcd_text(y, text, color=lcd.WHITE, background=lcd.BLACK):
    # Pad the line so a shorter update erases the previous text.
    lcd.draw_string(8, y, (text + " " * 38)[:38], color, background)


def show_startup_status(state, detail="", color=lcd.WHITE):
    lcd.clear(lcd.BLACK)
    lcd_text(20, "K210 UART CAMERA", lcd.CYAN)
    lcd_text(55, state, color)
    if detail:
        lcd_text(85, detail, lcd.WHITE)
    lcd_text(125, "IO6 TX -> S3 GPIO18 RX", lcd.WHITE)
    lcd_text(150, "IO7 RX <- S3 GPIO17 TX", lcd.WHITE)
    lcd_text(175, "UART %d 8N1" % UART_BAUD, lcd.WHITE)


def build_crc32_table():
    table = []
    for value in range(256):
        crc = value
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
        table.append(crc)
    return table


CRC32_TABLE = build_crc32_table()


def crc32(data):
    crc = 0xFFFFFFFF
    for value in data:
        crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ value) & 0xFF]
    return crc ^ 0xFFFFFFFF


def write_all(uart, data):
    offset = 0
    while offset < len(data):
        end = min(offset + SEND_CHUNK, len(data))
        written = uart.write(data[offset:end])
        if written is None:
            written = end - offset
        if written <= 0:
            raise OSError("UART write failed")
        offset += written


def send_packet(uart, packet_type, request_id, payload=b""):
    header = struct.pack(
        "<4sBBHIII",
        MAGIC,
        VERSION,
        packet_type,
        0,
        request_id,
        len(payload),
        crc32(payload),
    )
    write_all(uart, header)
    if payload:
        write_all(uart, payload)


def read_exact(uart, length):
    data = bytearray()
    while len(data) < length:
        chunk = uart.read(length - len(data))
        if chunk:
            data.extend(chunk)
        else:
            time.sleep_ms(2)
    return bytes(data)


def read_header(uart, link_ok):
    matched = 0
    received = 0
    started = time.ticks_ms()
    last_update = started
    if link_ok:
        lcd_text(210, "S3 LINK OK - WAIT PHOTO CMD", lcd.GREEN)
    else:
        lcd_text(210, "READY - WAIT S3 PING RX:0", lcd.YELLOW)
    while True:
        chunk = uart.read(1)
        if not chunk:
            now = time.ticks_ms()
            if time.ticks_diff(now, last_update) >= 1000:
                seconds = time.ticks_diff(now, started) // 1000
                if link_ok:
                    lcd_text(210, "LINK OK %ds - CMD RX:%d" % (seconds, received), lcd.GREEN)
                else:
                    lcd_text(210, "WAIT PING %ds RX:%d" % (seconds, received), lcd.YELLOW)
                last_update = now
            continue

        value = chunk[0]
        received += 1
        if value == MAGIC[matched]:
            matched += 1
            if matched == len(MAGIC):
                lcd_text(210, "KCAM COMMAND RECEIVED", lcd.YELLOW)
                return MAGIC + read_exact(uart, HEADER_SIZE - len(MAGIC))
        else:
            matched = 1 if value == MAGIC[0] else 0


def init_camera():
    lcd.init()
    show_startup_status("INITIALIZING CAMERA", "Please wait...", lcd.YELLOW)
    sensor.reset(dual_buff=True)
    sensor.set_pixformat(sensor.RGB565)
    sensor.set_framesize(sensor.QVGA)
    sensor.set_hmirror(0)
    sensor.set_vflip(0)
    sensor.skip_frames(time=2000)
    show_startup_status("CAMERA OK", "Initializing UART...", lcd.GREEN)


def init_uart():
    # This matches the UART path proven by the K210 AI Extension:
    # UART2 RX=IO7, TX=IO6 at 115200. IO6/IO7 are also wired to the onboard
    # ESP8285 on M1W, so hold that unused chip disabled through IO8 first.
    fm.register(WIFI_ENABLE_PIN, fm.fpioa.GPIOHS0, force=True)
    wifi_enable = GPIO(GPIO.GPIOHS0, GPIO.OUT)
    wifi_enable.value(0)
    fm.register(K210_TX_PIN, fm.fpioa.UART2_TX, force=True)
    fm.register(K210_RX_PIN, fm.fpioa.UART2_RX, force=True)
    uart = UART(UART.UART2, UART_BAUD, 8, 0, 0, timeout=1000, read_buf_len=4096)
    return uart, wifi_enable


def capture_jpeg():
    lcd_text(210, "CAPTURING...", lcd.YELLOW)
    img = sensor.snapshot()
    lcd.display(img)
    lcd_text(210, "ENCODING JPEG...", lcd.YELLOW)
    jpeg = img.compressed(quality=JPEG_QUALITY)
    data = jpeg.to_bytes()
    if len(data) < 4 or data[0] != 0xFF or data[1] != 0xD8:
        raise ValueError("JPEG encode failed")
    return data


def run():
    init_camera()
    uart, wifi_enable = init_uart()
    show_startup_status("READY", "ESP8285 OFF - UART2 ON", lcd.GREEN)
    print("K210 UART camera ready: TX=IO6 RX=IO7 baud=%d" % UART_BAUD)
    link_ok = False
    pending_ping_id = 0

    while True:
        header = read_header(uart, link_ok)
        magic, version, packet_type, flags, request_id, length, checksum = struct.unpack(
            "<4sBBHIII", header
        )
        if length:
            # Current S3 capture commands have no payload. Consume unknown data
            # so the next header can still be found.
            read_exact(uart, length)
        if version != VERSION:
            lcd_text(210, "IGNORED INVALID COMMAND", lcd.RED)
            continue
        if packet_type == PING_REQUEST:
            send_packet(uart, PONG_RESPONSE, request_id)
            pending_ping_id = request_id
            lcd_text(210, "PING RX - PONG SENT", lcd.YELLOW)
            print("S3 ping received, pong sent request=%d" % request_id)
            continue
        if packet_type == LINK_ACK and request_id == pending_ping_id:
            link_ok = True
            lcd_text(210, "S3 LINK OK - TWO WAY", lcd.GREEN)
            print("S3 UART two-way link confirmed request=%d" % request_id)
            time.sleep_ms(1000)
            continue
        if packet_type != CAPTURE_REQUEST:
            lcd_text(210, "IGNORED COMMAND 0x%02X" % packet_type, lcd.RED)
            continue

        try:
            lcd_text(210, "REQUEST #%d" % request_id, lcd.YELLOW)
            jpeg = capture_jpeg()
            lcd_text(210, "SENDING %d BYTES..." % len(jpeg), lcd.YELLOW)
            send_packet(uart, JPEG_RESPONSE, request_id, jpeg)
            lcd_text(210, "SENT #%d (%d BYTES)" % (request_id, len(jpeg)), lcd.GREEN)
            print("capture request=%d jpeg=%d" % (request_id, len(jpeg)))
            del jpeg
        except Exception as error:
            message = str(error).encode()
            send_packet(uart, ERROR_RESPONSE, request_id, message)
            lcd_text(210, "ERROR: %s" % str(error), lcd.RED)
            print("capture error:", error)
        gc.collect()


try:
    run()
except Exception as fatal_error:
    print("FATAL:", fatal_error)
    try:
        lcd.init()
        lcd.clear(lcd.RED)
        lcd.draw_string(8, 30, "K210 CAMERA FATAL", lcd.WHITE, lcd.RED)
        lcd.draw_string(8, 70, str(fatal_error)[:38], lcd.WHITE, lcd.RED)
        lcd.draw_string(8, 100, str(fatal_error)[38:76], lcd.WHITE, lcd.RED)
    except Exception:
        pass
    while True:
        time.sleep_ms(1000)
