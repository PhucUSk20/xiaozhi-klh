# Bread Compact Wi-Fi: SH1106 emoji + secondary SPI TFT

## Menuconfig layout

Under `Xiaozhi Assistant`, enable and enter `Add secondary SPI TFT for images`.
Its submenu contains the controller/resolution, the matching controller's
options, TFT SPI clock and vocabulary image source.
The existing Kconfig symbol names and values are unchanged.

### Secondary TFT pin selection

Inside this submenu, toggle **Use TFT pins CS38/RST40/DC41 (off:
CS12/RST14/DC13)** (`CONFIG_BREAD_COMPACT_WIFI_TFT_USE_CS38_RST40_DC41`,
ESP32-S3 only, default off). This applies to every secondary TFT controller
and the ILI9486 diagnostic.

| Signal | Off (original wiring) | On (new wiring) |
| --- | --- | --- |
| CS | GPIO12 | GPIO38 |
| RST | GPIO14 | GPIO40 |
| DC | GPIO13 | GPIO41 |
| MOSI | GPIO15 | GPIO15 |
| SCK | GPIO2 | GPIO2 |

With the new TFT pins enabled, **Use legacy microphone pins
(GPIO14/GPIO13/GPIO12)** becomes available in the main `Xiaozhi Assistant`
menu. Keep it disabled when using the original TFT pins because they conflict.
Connect the backlight separately.
Existing variants retain the original wiring. The new ESP32-S3 release
variant `bread-compact-wifi-sh1106-ili9341-320x240-sd-cs38` selects the new
pins with SH1106, ILI9341 and SD support, retaining the existing flash and
partition defaults. It uses the same single board factory. To build it:

```powershell
python scripts/build.py bread-compact-wifi --name bread-compact-wifi-sh1106-ili9341-320x240-sd-cs38
```

For another panel, select its controller in menuconfig and use a distinct
release variant name when distributing firmware with the new wiring.
Pin tables in the panel sections below describe the original wiring unless
this option is enabled. Verify both pin configurations on physical hardware.

Enable and enter `Read TFT GIF files from SDMMC` inside that submenu to see
the GIF directory and SDMMC clock. The ILI9486 diagnostic option appears in
this SD submenu only for the ILI9486 panel. Enable and enter that diagnostic
option to see its SPI clock and GIF filename. Disabling a parent hides its
child settings. The OLED type remains independent of the secondary TFT.

## Robot Hub music test

For this ESP32-S3 board, enable **Xiaozhi Assistant -> Enable Robot Hub music
test (search/play)** (`CONFIG_BREAD_COMPACT_WIFI_HUB_MUSIC_TEST`, default off).
This registers `self.music.play_url` and `self.music.stop`. When disabled,
neither device tool is advertised and the HTTP/MP3 player implementation is
excluded. Normal conversation/TTS remains available.

With the flag enabled, three seconds after activation reaches idle the robot
sends the embedded spoken request "Please search my Robot Hub music library
and play the first song on this robot." through the existing
`Application::SendAudioPrompt` path (`listen/start` -> Opus -> `listen/stop`).
No live microphone input, extra credential or direct Robot Hub HTTP tool call
is needed. The test runs once per boot. Interruption during the initial delay
cancels the test; it will not retry on later idle transitions.

The voice host recognizes the request and calls the already-connected MCP
endpoint: `music__search_music` -> `music__play_music` -> device
`self.music.play_url` with the returned `playbackArguments`. Credentials remain
on the server. The firmware cannot force the host's tool selection: enable My
Music on the connected endpoint and copy the Robot Hub music section of
`xiaozhi_me_prompt.md` into the assistant prompt. No Robot Hub server source
change or deployment is needed. A silent/broken physical mic does not affect
this prerecorded request, but ASR, host instructions and MCP availability do.

The asset `hub_music_test_prompt.ogg` is an English speech request synthesized
locally with Windows Microsoft Zira Desktop (rate -1), encoded as mono Opus,
16 kHz input, 24 kbps, 60 ms frames, VOIP application. It is included only when
the flag is enabled. The request is sent upstream, not played through the robot
speaker. Its small size is part of the selected board firmware image.

Check serial logs for `Startup MCP test: asking host`,
`Audio prompt 'hub_music_test' sent`, and the recognized request/host response.
Inspect Robot Hub connection logs for the actual search/play MCP calls. After
the host invokes device playback, expect `MP3 HTTP 200` and
`First MP3 PCM frame queued for speaker`. This confirms decoded PCM reached
the speaker queue; verify audible playback on hardware. URL tickets are never
printed by the player. Existing production stream URL restrictions still apply.

`Playback queued` only acknowledges acceptance, not audible playback. Request
stop using `self.music.stop`; wake word or entering listening also stops music.
Disable the option and flash again to verify the device music tools and startup
request are absent in a fresh session. The flag does not disable remote server
tools. If building via `scripts/build.py`, remember it regenerates configuration
from the selected variant; a later build can overwrite a manual menuconfig
selection. For a manual test, save menuconfig and use `idf.py build` directly.
Firmware builds and physical playback validation are left to the user.

## Voice Caro (3x3 tic-tac-toe)

Ported the `self.screen.show_caro` feature from
`D:\xiaozhi-origin8-orig\xiaozhi-origin8-orig`, adapting it to this board's
current panel variants. The server/assistant keeps the board state, selects
moves and judges wins/draws; firmware renders the supplied board. This is the
source project's 3x3 game (three in a row), not five-in-a-row Gomoku.

The tool is registered automatically when a secondary TFT or Compact SH1106
display is available. A TFT is preferred; otherwise the board appears on the
SH1106 OLED. Standard SSD1306-only and no-display paths do not advertise the tool.
No image download, SD card, new board factory or new pin assignments are needed.
ILI9341 320x240, ST7789 320x240, ST7735 160x128 and ILI9486 480x320 retain their
existing drivers; the TFT grid scales to the selected resolution. Disable the
diagnostic EduHub slideshow to make screen tools available.

Requests:

| Action | Tool arguments |
| --- | --- |
| New board | `{"cells":"123456789"}` |
| X in square 5, O in square 1 | `{"cells":"O234X6789"}` |
| Close board | `{"cells":"end"}` |

Empty cells must keep their own number. Invalid length, wrong square digits,
lowercase letters and other characters are rejected before changing the screen.
The board stays visible through speaking/listening and emoji updates. Opening it
cancels pending TFT image/GIF display jobs; closing it restores the normal display
or SD emotion animation. Returning to application idle also closes it, including
when the voice session disconnects, for both OLED and TFT configurations.
The application calls the optional `Board::OnSessionIdle()` hook on its own task;
other boards inherit a no-op. A running physical Rock Paper Scissors game rejects
Caro display requests.

Use the Caro section of `xiaozhi_me_prompt.md` in the assistant's server prompt
to enable voice turns: say "choi caro", then choose a square number or position.
The prompt file is guidance to copy to the server; it is not installed remotely
by changing firmware. Existing servo angles and Rock Paper Scissors logic are
unchanged. Validate voice turns, win/draw handling, QR/clock/emoji transitions,
disconnect cleanup and physical rendering after flashing. No firmware build or
hardware validation was performed for this port.

## Wheel stop pulse and button assignment

The saved wheel stop pulse is **1700 microseconds**, shared by PCA9685 PWM11
and PWM12. Change it under **Xiaozhi Assistant -> Bread Compact WiFi wheel
movement -> Stop both wheels: PCA9685 PWM11/PWM12 pulse (microseconds)**.
This also supplies the web control's continuous-servo neutral pulse. It is not
servo 3/PWM10's positional rest angle; the current implementation does not have
independent left/right stop calibration settings.

Under **Xiaozhi Assistant**, both short-press actions are now configurable:

- **Button A short-press action**: original idle GIF-cycle/game behavior, volume up or volume down.
- **Button B short-press action**: QR code, local wake-word conversation, volume up or volume down.
- **Button volume adjustment step (percentage points)**: 1-30, default 10, clamped volume 0-100.

For A to increase and B to decrease, choose **Increase speaker volume** for A
and **Decrease speaker volume** for B. Existing defaults remain A cycle/game and
B QR; existing wake-word choice remains valid. A long press still cancels the
physical game. Audio volume updates and notifications execute on the application
task and use the existing persisted audio setting. Regenerate menuconfig before
selecting new options; generated SDK headers predating the volume-step setting
use a source fallback of 10. No generated SDK configuration is edited by this change.

## Rock Paper Scissors servo 1

Each of the five throws now randomly selects **45, 92 or 140 degrees on servo 1**
(PCA9685 channel 8). The throw remains visible for one second, then servo 1 returns
to **0 degrees for all three throws: 45 -> 0, 92 -> 0, 140 -> 0**.
Cancelling a throw, completing the game, starting the next game and resetting
arms also return servo 1 to 0. Servo 1's manual angle range remains 0-180 degrees.

The game still counts down 3-2-1 and waits for the ultrasonic hand trigger for
each throw. Servos 2 and 3 retain their existing resting positions (15 and 0);
they are no longer selected randomly for game gestures. Dance choreography is
unchanged. No firmware build or physical servo test was performed for this change.

During the game, audio remains in wake-word-only mode: the local wake detector
continues receiving microphone input, while voice uplink and VAD events are paused.
Calling the configured wake word cancels the game, requests servo 1 rest at
0 degrees, then returns to normal voice interaction. The ultrasonic hand wait and
five-throw flow are unchanged. Holding button A still cancels the game.
Audio and display cleanup are scheduled on the application task, so delayed game
cleanup cannot reset audio a second time during the wake acknowledgement sound.
Browser Play Game requests are also queued on that task (one pending request);
the HTTP acknowledgement means accepted, and robot status reports startup/running.
The latest normal speaking/listening/idle audio requests are restored on exit.
Full input suspension for browser microphone capture continues taking precedence.
Verify wake recognition during countdown, hand wait, servo movement and after a
network disconnect on hardware. Look for `Game interrupted by wake word`; the game
must no longer log `Audio input suspended` merely because it has started.

The secondary TFT now also shows the physical game's **READY 3-2-1** countdown
and **ROUND 1/5 through 5/5**, with a hand-below-10-cm hint. Previously the game
only updated the optional Compact SH1106 emoji display, so SSD1306 + TFT variants
kept showing a neutral GIF. The game now updates both the custom OLED (when
configured) and TFT. The TFT's exclusive game mode pauses SD GIFs, invalidates
pending image workers and rejects image/text/Caro requests until game cleanup.
It remains visible through speaking/listening/idle transitions. Normal completion,
button cancellation and wake interruption release the display and resume emotion
GIFs. The layout adapts to the configured TFT dimensions; logs identify it with
`Game screen: READY ...` and `Game screen: ROUND ...`.

## ILI9341 3.2-inch SPI V2.0 display (320x240 landscape)

The user's module previously identified as a 3.5-inch ILI9486 is actually a
**3.2-inch ILI9341, 320x240, SPI V2.0**. Select the new ILI9341 option for this
hardware. The older ILI9486 option and standalone raw RGB666 diagnostic remain
specific to actual ILI9486 modules; they must not be selected for ILI9341.

Under **Xiaozhi Assistant -> Add secondary SPI TFT for images**, choose
**Secondary TFT controller / resolution -> ILI9341 3.2 inch SPI V2.0, 320x240 landscape**.
This uses `esp_lcd_new_panel_ili9341`, native driver initialization, RGB565
(two bytes per pixel), BGR order and hardware rotation from native 240x320.
For the installed 3.2-inch module the orientation is `swap_xy=true`, `mirror_x=true`,
`mirror_y=true` (MADCTL MX+MY+MV+BGR, 0xE8). Photos confirmed that the earlier
unreflected landscape was upside down, not a left/right mirror. Flipping both
axes rotates the entire display 180 degrees and preserves glyph handedness.
This single panel-level orientation applies to LVGL labels, Caro squares, the
Rock Paper Scissors countdown/rounds, vocabulary JPEG captions, PNGs and GIFs.
There are no per-content rotation exceptions or image-file changes. The eye GIF
is nearly symmetric, so it is not a reliable orientation reference. Check upright
`ABC`, Caro squares 1-2-3 across the top, 7-8-9 across the bottom, and vocabulary
captions after flashing. Other panel variants keep their own orientation settings.
It does not use the ILI9486 RGB666 conversion or its raw initialization sequence.
**ILI9341 display color inversion** defaults off; enable it if the physical
module requires inverted output. Start with **Secondary TFT SPI clock: 10 MHz**.

The existing secondary TFT wiring is unchanged:

| Signal | ESP32-S3 GPIO |
| --- | --- |
| SCK / CLK | 2 |
| MOSI / SDI | 15 |
| CS | 12 |
| DC / RS | 13 |
| RESET | 14 |

Connect power, ground and backlight as required by the module. No backlight GPIO
is assigned. Display MISO, touch controller and the module's SPI SD socket are
not used. Existing SD support remains the separate SDMMC wiring CLK10/CMD11/D0-16.

For the public server slideshow, turn off **Read TFT GIF files from SDMMC**,
select **Vocabulary image source -> HTTP/S3**, then enable
**Run LCD EduHub image slideshow at boot (ST7789 / ILI9341)**.
The five public images repeat with a default hold time of 10 seconds after loading;
no token, account or manual URL is needed. For SD GIF use, disable the slideshow
and enable the existing SDMMC support instead. Both paths render at 320x240.

The ESP32-S3 release variants are:

- `bread-compact-wifi-sh1106-ili9341-320x240-sd`
- `bread-compact-wifi-sh1106-ili9341-320x240`
- `bread-compact-wifi-ili9341-320x240-sd` (SSD1306 128x32)
- `bread-compact-wifi-ili9341-320x240` (SSD1306 128x32)

These variants inherit the repository's ESP32-S3 16 MiB flash and
`partitions/v2/16m.csv` defaults. Manual menuconfig selection uses the matching
ILI9341 OTA identity. The board still exports a single factory. Existing ST7789
and ILI9486 variants keep their identities and pins. No firmware build or physical
ILI9341 test was performed; verify color order, inversion, orientation, screen
bounds, SD GIF playback (if enabled), and network image rendering after flashing.

## Standalone ILI9486 LCD + SD diagnostic

`compact_lcd_sd_test.cc` is a dedicated diagnostic entry point based on
Xiaozhi-Robot's `main/main_emoji_display_test.c`. It runs instead of the
assistant when `CONFIG_BREAD_COMPACT_WIFI_ILI9486_SD_TEST` is enabled. It uses
direct polling SPI, with no ST7789 panel driver, no panel-IO adapter and no
LVGL display/rendering. LVGL is initialized only for the existing GIF decoder's
allocator/filesystem services. The SH1106, audio, Wi-Fi, robot servos and camera
are not initialized, and NVS is not erased.

To preserve the current SD directory and other hardware settings, run
`idf.py menuconfig` in Origin and:

1. Keep the secondary TFT enabled, choose ILI9486 480x320, and enable SD GIF support.
2. Enable `Run standalone ILI9486 LCD + SD diagnostic at boot`.
3. Keep `LCD diagnostic SPI clock (MHz)` at 5 initially.
4. Set `LCD diagnostic GIF filename in configured SD directory` to `neutral.gif`,
   or another indexed GIF filename. This is a filename, not a full path.
5. Check `Secondary TFT GIF directory relative to SD root` against the actual
   card. For example, the supplied log used `Kira-Emotes 3/Kira-Emotes/gif`.

Then build/flash/monitor using the normal ESP-IDF commands. Alternatively the
release variant `bread-compact-wifi-ili9486-sd-test` enables diagnostic mode;
the build script regenerates configuration, so check the SD directory again
when selecting that variant.

The repeated sequence is identified by `LCD_SD_TEST` logs:

| Stage | What should appear | Purpose |
| --- | --- | --- |
| FORMAT RGB666 | COLMOD 0x66, three bytes per pixel | Fixed confirmed format; no alternating RGB565 phase |
| 1 | Solid red, green, blue, white, black, two seconds each | No GIF or SD data needed |
| 2 | 20x20 squares, white border, red X=240 and green Y=160 center lines | Check screen bounds, center and orientation |
| SD mount | Existing 1-bit SDMMC pins CLK10/CMD11/D0-16 | Starts only after the first LCD-only stages |
| 3 | Centered GIF at native size, eight seconds | Check decode without scaling |
| 4 | Same GIF scaled to fit, eight seconds | Check scaling independently of LVGL rendering |

The current diagnostic uses native 320x480 portrait addressing (MADCTL 0x48)
and rotates logical 480x320 coordinates in software, as in Robot's original
portrait test. Each logical row is sent as an explicitly addressed native
column, avoiding reliance on hardware MV and row wrapping. A 480x320 GIF maps
logical (0,0), 480x320 to native (0,0), 320x480. The log `GIF placement` prints
both rectangles. This diagnostic change is separate from the production LVGL
path; verify its border/center lines and GIF before applying that addressing
change to the main display driver.

This repeats until reset. A native GIF larger than the screen skips stage 3
and proceeds to the fitted stage. Transparent pixels are composited over black.
The 160x128 GIFs work in both stages; the fitted size is 400x320, centered within
480x320 with 40-pixel black side margins. GIF delays are honored, with at least
20 ms between frames; SPI time also contributes to the frame period.

If solid colors/grid are corrupted, SD asset dimensions cannot explain that
failure. Verify the white border and center lines; lower the SPI clock for a wiring
check. Hardware feedback confirmed correct RGB666 output on the user's panel;
the production ILI9486 path now uses COLMOD 0x66 and converts LVGL RGB565 pixels
to three-byte RGB666 before transmission. If raw
patterns are correct but stage 3 fails, inspect GIF/file/decoder data. If stage
3 works and stage 4 fails, focus on scaling. If every stage works while normal
firmware is corrupted, focus on the production display/rendering path.

To restore the assistant, disable `Run standalone ILI9486 LCD + SD diagnostic
at boot` and rebuild/flash. The source does not modify the SD card.

## ST7789 2-inch display (320x240 landscape)

### Public EduHub image test (no token)

`compact_lcd_image_test.cc` downloads five public sample images directly from
EduHub's media server: abide by, agreement, assurance, cancellation and determine.
No account, Robot token, lesson ID, manually entered image URL or local server is
required. This is a fixed sample slideshow for checking the display, not a live
lesson synchronizer. The public image addresses are built into the test file.

In menuconfig:

1. Select **Bread Compact WiFi**, enable **Add secondary SPI TFT for images**,
   and select **ST7789 2 inch, 320x240 landscape** or
   **ILI9341 3.2 inch SPI V2.0, 320x240 landscape** to match the module.
2. Disable **Read TFT GIF files from SDMMC** and select **Vocabulary image source -> HTTP/S3**.
3. Enable **Run LCD EduHub image slideshow at boot (ST7789 / ILI9341)**.
4. Set **LCD image diagnostic SPI clock (MHz)** to **10** initially. This setting
   overrides the normal TFT SPI clock only while the diagnostic is enabled.
5. Keep **Test direct LCD solid colors and LVGL text before slideshow** enabled
   when investigating an all-white panel.
6. Optionally change **Number of public test images** (1-5, default 5) or
   **Display time per image after loading** (default 10 seconds).

With the startup color test enabled, the boot sequence first sends solid RED,
GREEN, BLUE, WHITE and BLACK (one second each) directly to panel RAM before TFT
LVGL registration. These are synchronous SPI command/parameter writes using one
DMA-capable scanline; they do not use Wi-Fi, JPEG decoding or LVGL flush callbacks.
Rotation is applied before the pattern. Stage 2 shows **LCD TEST** for two seconds
through LVGL, followed by the public image sequence.

After updating source that adds these menuconfig options, run `idf.py reconfigure`
before your next build/flash so the generated SDK header includes the startup
color option. Older generated headers without the diagnostic SPI option fall
back to 10 MHz in source; an explicitly configured speed is still respected.

- If all direct colors remain white, inspect panel power/common ground, RESET,
  display CS (not SD CS), DC, SCK and MOSI, the module's SPI interface, and selected
  controller. Successful SPI API calls cannot confirm physical panel reception.
- If direct colors work but LCD TEST does not, investigate the LVGL/flush path.
- If both colors and LCD TEST work but photographs do not, investigate image
  decode/format/scaling. The image log now says **Image submitted to TFT UI**;
  it does not claim physical LCD visibility.

The task waits for the normal firmware to connect to Wi-Fi, downloads the first
sample, holds it for approximately 10 seconds after download/decode, then advances.
After the last sample it starts again. The next image's download time is additional
to the display interval. Images are baseline JPEG 160x128 and scaled to fit the
320x240 display with aspect ratio preserved. Existing limits are 128 KiB and
320 pixels per side. These five public images were fetched without credentials
and their JPEG dimensions checked on 2026-10-26; future server changes may require
updating the built-in sample URLs.

The normal assistant and Wi-Fi setup remain active. TFT MCP screen tools are not
registered during the test, so they cannot interrupt it. Disable the test to restore
those tools. No change to `main.cc` is needed. Network/decode failures use the existing
TFT error notification, then move on; Wi-Fi loss pauses further requests.
See `LCD_IMAGE_TEST` and `CompactTft` logs.

No firmware build or hardware test was performed. On-board color, orientation,
network access and physical rendering still need checking after flashing.

Enable `Add secondary SPI TFT for images` in menuconfig, then select
`Secondary TFT controller / resolution -> ST7789 2 inch, 320x240 landscape`.
The native 240x320 panel is rotated using hardware X/Y swap and X mirroring;
the ESP-IDF ST7789 driver initializes RGB565 output. IPS color inversion defaults
to enabled, matching Xiaozhi-Robot. For a module with inverted-looking colors,
change `ST7789 display color inversion (usually enabled for IPS)`.

Pins are unchanged: CS12, RST14, DC13, MOSI15, SCK2. Connect the module's power
and backlight according to its specification; no backlight GPIO is assigned.
Keep legacy microphone wiring disabled with the original TFT pins; the new
CS38/RST40/DC41 option permits legacy microphone wiring. Start SPI at 10 MHz.

All existing `self.screen.*` tools use the selected panel after rebuilding and
flashing. Images and GIFs scale to fit 320x240 with their aspect ratio preserved.
PNG/baseline JPEG input retains the 320x320 and 128 KiB limits; GIF validation
allows up to 480x480. Prefer 320x240 assets for this panel.
Select SH1106 and enable SD GIF support to synchronize animated image emoji
with assistant emotion, using the same SD wiring and GIF directory as below.

Release variants with distinct OTA identities:

- `bread-compact-wifi-sh1106-st7789-320x240-sd`: SH1106 and SD GIF emoji.
- `bread-compact-wifi-sh1106-st7789-320x240`: SH1106 and image tools without SD.
- `bread-compact-wifi-st7789-320x240`: SSD1306 128x32 and image tools without SD.

```powershell
python scripts/build.py bread-compact-wifi --name bread-compact-wifi-sh1106-st7789-320x240-sd
```

Physical testing is still required to confirm color order, orientation and
refresh performance on the actual 2-inch module.

## ILI9486 3.5-inch display (480x320 landscape)

The secondary display can also use the ILI9486 module from Xiaozhi-Robot's
`bread-compact-wifi-s3cam`. Its native resolution is 320x480, presented here as
480x320 landscape. This is the confirmed 3.5-inch panel, not a 480x360 panel.
Production initialization uses reset, sleep out, COLMOD 0x66, MADCTL 0x68,
inversion off and display on. The current standalone diagnostic separately
checks native MADCTL 0x48 with software rotation.
The additional Robot power/timing register writes are omitted because they
were not used by that successful hardware test. The existing LVGL display
renders RGB565 and handles landscape rotation.
The ILI9486 uses a dedicated panel-IO adapter matching Robot's polling SPI:
command and data use separate CS pulses, DC stays driven between transfers,
and pixel data is converted from big-endian RGB565 to RGB666 one address-window
row at a time, matching the diagnostic's CS transaction boundaries. The row
width is taken from CASET (for example 400 pixels for the fitted 160x128 GIF),
rather than splitting at a fixed 512 pixels across row boundaries. A single
1536-byte DMA buffer is allocated at startup;
there are no per-frame conversion allocations. The base panel driver remains
configured for 16-bit input so it reads the correct amount of LVGL data.
The adapter reports completion to LVGL after transmission finishes. Only one SPI device owns CS;
ST7735 and ST7789 continue using the ESP-IDF SPI panel IO.

When diagnosing stripes or corrupted colors, first keep the existing SD assets.
A log such as `SD GIF playing: neutral.gif (160x128) ... TFT 480x320` confirms
the small GIF was decoded and scaled for the selected panel; it does not prove
that the SPI electrical signaling or the controller's pixel format is correct.
The updated firmware logs `ILI9486 IO: Robot polling SPI, RGB565 -> RGB666
(3 bytes/pixel)`. If corruption persists, run the standalone color/grid and SD GIF
diagnostic and try a lower TFT SPI clock (for example 5 MHz) to isolate asset
decoding from controller/wiring issues. Hardware validation is still required.

The standalone test does include zoom: stage 4 scales 160x128 to 400x320 with
nearest-neighbor sampling and sends rows directly. The production ILI9486 GIF
path now samples the same decoded BGRA canvas in software, composites on black,
and creates a tightly packed RGB565 image at the fitted dimensions. LVGL draws
that image at 1:1 without transforming ARGB8888 across partial refresh strips.
For a 160x128 GIF, the output is 400x320 with an 800-byte stride and a 256000-byte
PSRAM raster allocated once per loaded GIF and reused for subsequent frames.
This is logged as `GIF raster: 160x128 -> 400x320, RGB565 stride=800, LVGL 1:1`.
The RGB666 adapter still handles transmission of LVGL's partial rectangles.
The log `ILI9486 diagnostic-matched minimal init` identifies the updated
production initialization. Keep the normal SPI clock at the proven 5 MHz
while comparing these paths. Hardware validation is still required.

In `idf.py menuconfig`, select `Xiaozhi Assistant -> Board Type -> Bread Compact
Wi-Fi`, enable `Add secondary SPI TFT for images`, then choose `Secondary TFT
controller / resolution -> ILI9486 3.5 inch, 480x320 landscape`. Choose ST7735
160x128 to return to the 1.8-inch panel. The enable symbol remains
`CONFIG_BREAD_COMPACT_WIFI_TFT_128X160` for compatibility with existing configs;
the new panel is selected by `CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320`.
ILI9486 requires ESP32-S3 with PSRAM enabled.

For animated image emoji, select SH1106 128x64 as the OLED and enable
`Read TFT GIF files from SDMMC`. This connects assistant emotion updates to the
existing SD GIF player. Use the configured GIF directory with files such as
`neutral.gif`, `happy.gif`, `sad.gif`, and `thinking.gif` (see SD instructions
below). GIFs retain their aspect ratio and are scaled to fit 480x320; existing
small GIFs still work. Without SD support, image tools remain available but no
automatic emoji animation starts. Automatic emotion synchronization uses SH1106.

Release variants (each has a distinct OTA identity):

- `bread-compact-wifi-sh1106-ili9486-480x320-sd`: SH1106 and animated SD emoji.
- `bread-compact-wifi-sh1106-ili9486-480x320`: SH1106 and image tools without SD.
- `bread-compact-wifi-ili9486-480x320`: SSD1306 128x32 and image tools without SD.

Example build command for the user:

```powershell
python scripts/build.py bread-compact-wifi --name bread-compact-wifi-sh1106-ili9486-480x320-sd
```

All TFT types use the pin table below: CS12, RST14, DC13, MOSI15, SCK2.
The SD card remains CLK10/CMD11/D0-16. Connect the backlight separately and keep
legacy microphone wiring disabled when using the original TFT pins. Start SPI at the default 10 MHz. PNG and
baseline JPEG files may be up to 480x480 pixels for ILI9486 (320x320 for ST7735/ST7789),
with the existing 128 KiB encoded image limit. GIF validation allows up to
480x480. Use 480x320 assets for the large landscape panel when possible.
Physical testing must confirm orientation, color order and refresh performance
on the actual module; source checks cannot establish these.

## Existing ST7735 1.8-inch display

The `bread-compact-wifi-sh1106-tft-160x128` variant extends the existing ESP32-S3
Bread Compact Wi-Fi board, retaining its audio, robot control and K210 UART
camera support. It reuses the ST7735 128x160 initialization path from
`bread-compact-wifi-lcd` (which uses ESP-IDF's ST7789-compatible panel driver).
Physical testing is required to confirm the controller, offsets and RGB order
of a particular 1.8-inch module.

The SH1106 remains the primary 128x64 display for emoji, clock, QR code and
assistant state. It uses I2C SDA GPIO8 and SCL GPIO9. The secondary TFT only
shows images requested by MCP tools and its own download errors. It does not
receive assistant text or theme updates. The SD variant follows emotion updates
with GIFs; without SD, TFT startup is blank;
there is no automatic startup URL download.

The physical 128x160 TFT panel runs in landscape mode: the image area is
160 pixels wide by 128 pixels high, using hardware X/Y swap and Y mirroring.
The mirror flags are inverted from the earlier layout to rotate the mounted
panel 180 degrees while preserving landscape dimensions.

| TFT signal | ESP32-S3 GPIO |
| --- | --- |
| CS | 12 |
| RESET / RST | 14 |
| A0 / DC | 13 |
| SDA / MOSI | 15 |
| SCK / CLK | 2 |
| GND | GND |

SDA here means SPI MOSI, not I2C SDA. Connect VCC and BL/LED according to the
display module's electrical specification. There is no backlight GPIO or PWM
control in this variant. The display uses SPI3, mode 0, RGB565, currently at
10 MHz (`io_config.pclk_hz` in `compact_wifi_board.cc`). Startup logs print
the configured frequency in Hz/MHz and the TFT pins; this is not a measured
bus frequency. Changing the configured frequency requires rebuilding/flashing.
LCD black pixels still transmit some backlight, unlike OLED pixels. Reducing
backlight dims the entire image, including the emoji. Backlight PWM requires
the module's BL/LED wiring and electrical interface to be identified first;
the firmware does not assign an arbitrary GPIO or drive an LED load directly.

The legacy microphone pins 12/13/14 conflict with the original TFT pins and
are disabled unless the CS38/RST40/DC41 TFT pin option is enabled.
Use the current shared audio clocks: BCLK 47, WS 48, microphone data 45,
speaker data 21. I2C GPIO8/9 are shared by SH1106 and the robot's PCA9685.
OLED initializes LVGL once; TFT adds a second display to the same LVGL port.
The OLED remains the default LVGL display and the application's `GetDisplay()`.

## Select and build

Activate ESP-IDF 6.0.2, then run from the project root:

```powershell
python scripts/build.py bread-compact-wifi --name bread-compact-wifi-sh1106-tft-160x128
```

This uses the project's ESP32-S3 defaults (16 MB flash, octal PSRAM,
`partitions/v2/16m.csv`). Use hardware matching these settings. The distinct
firmware name is reported for OTA; the board type remains `bread-compact-wifi`.
There is still exactly one `DECLARE_BOARD` factory.

For manual configuration, select **Bread Compact Wi-Fi**, then enable
**Add secondary SPI TFT for images** in the XiaoZhi
configuration menu. Select **OLED Type: SH1106 128*64** and enable
**Show full-screen animated emoji only**. Both panels run simultaneously.
Build with `idf.py build`.
If the build script raises Windows `WinError 193` while launching `idf.py`,
use this manual menuconfig/build path. The release-script validation for this
change used a launcher that invokes `idf.py` with the active Python interpreter.

## Image from a URL

Pass a direct HTTP/HTTPS image URL to `self.screen.show_image`. For the EduHub
English lessons, call `english_get_word_image` with the exact `lessonId` and full
`key` of each new card, then pass the returned `imageUrl` as this device tool's
`url` when `available=true`. EduHub Role/tutorInstructions v20 requires this
before speaking each new course card. Missing mappings return `available=false`,
without an apple fallback. Calling the tool with no arguments still returns a
fixed apple image for explicit screen tests. While a static preview is shown,
automatic emotion GIF changes wait until it is hidden or explicitly replaced.
An acknowledgement only means background download started; physical display
must still be checked. Use a robot-reachable LAN/public origin, not localhost.
The download worker sends `User-Agent: Xiaozhi/1.0`: the EduHub CloudFront
distribution returns HTTP 403 to requests without a User-Agent. This requires
reflashing the updated firmware; changing the tutoring Role cannot fix HTTP headers.
The old
`TFT startup image URL` option has been removed: previously configured values
do not trigger downloads anymore. Wi-Fi connects and reconnects independently
of the image displayed on TFT.

The board registers these standard MCP tools, available to the connected
assistant/server:

| Tool | Arguments | Action |
| --- | --- | --- |
| `self.screen.show_image` | `{"url":"http://192.168.1.10:8080/photo.png"}` | Download and display an image asynchronously |
| `self.screen.hide_image` | `{}` | Clear TFT to black; OLED is unchanged |

To serve an image from a computer on the same Wi-Fi network, run
`python -m http.server 8080 --bind 0.0.0.0` in the image directory, then use that
computer's LAN IP in the URL. The URL must return the image bytes directly,
HTTP 200, and a Content-Length header. Network errors show a failure notification
on TFT; details appear in the serial log. SH1106 keeps its normal emoji flow.

Supported files are PNG and baseline JPEG, at most 128 KiB and at most 320 pixels
in each dimension. Resize to 160x128 landscape for best memory use and sharpness. Images
are fitted without changing their aspect ratio, with black borders as needed,
and remain visible until replaced or hidden. Progressive JPEG, HTML pages,
chunked responses without Content-Length and arbitrary LVGL binary files are
not supported by these board-specific tools.

## GIFs from a microSD card

### User configuration

### Vocabulary image source: HTTP/S3 or SD

Search `BREAD_COMPACT_WIFI_TFT_WORD_IMAGE_SOURCE` in menuconfig. Choose exactly
one source: **HTTP/S3** (default) or **SD card** (requires TFT SDMMC enabled).
This is a build-time choice; save, build and flash to switch.

For SD mode, download the captioned ZIP in EduHub Administration → Vocabulary
images and extract it at SD root. Keep `/manifest.json` and `/word-images/*.jpg`
as exported. This directory is separate from the emoji GIF directory. The
index accepts version 1, at most 4,000 images and a 2 MiB manifest. Each JPEG
is at most 128 KiB and its SHA-256 is verified before decoding. Filenames are
restricted to the archive's hashed image names. Reboot after replacing the
manifest; the successfully parsed index is cached in RAM for the session.

The full EduHub pack checked on 2026-10-28 contains 3,468 entries (manifest
1,735,741 bytes). The previous 2,000-entry limit rejected it in
`LoadSdWordImageIndex` (`Unsupported SD manifest`), matching the reported abort
backtraces. Expected SD read, schema, missing-key, checksum and JPEG errors now
return a bounded failure message to the image task rather than throwing C++
exceptions. The task shows an error and releases its busy flag so a later request
can retry. No HTTP fallback is added in SD mode. Unexpected allocation/decoder
exceptions retain the worker's existing exception handler.

Raw file buffers use non-throwing heap allocation, preferring PSRAM; the manifest
raw text is freed immediately after parsing. Optional captions/credits are freed
entry by entry as validated mappings are copied into a temporary index. Only the
complete validated index is cached. A large full pack still needs sufficient
heap/PSRAM on the physical board. Parse/allocation failures return diagnostics;
no successful on-device load is claimed by host/static checks.

SD layout verified against the saved menuconfig on 2026-10-28:

- GIF setting: `Kira-Emotes 3/Kira-Emotes/gif` (relative to SD root, without a drive letter).
- Runtime GIF directory: `/sdcard/Kira-Emotes 3/Kira-Emotes/gif`; 25 GIF files were present.
- Vocabulary source: SD card, enabled in saved configuration.
- Vocabulary manifest: `/sdcard/manifest.json` (fixed path, independent of the GIF directory setting).
- Vocabulary images: `/sdcard/word-images/<hash>.jpg`; all 3,468 referenced files were present.

All 3,468 mappings and image SHA-256 checksums matched on the inserted card;
the largest image was 10,722 bytes, within the 128 KiB limit.

The removable volume appears as `F:` on the computer; `F:` must not be entered
in firmware paths. No SD files or generated SDK configuration were modified by
this check. The new firmware has not been built or tested on hardware.

SD mode advertises `self.screen.show_word_image({"lessonId":"contracts",
"key":"contracts:abide by"})`. It reads the exact image from the manifest
in a background worker and does not call HTTP or fetch S3 URLs. A missing
manifest/word, unmounted card or corrupt image reports an error without
network fallback; continue teaching by voice. HTTP mode advertises the
existing show_image tool instead. Startup logs report the selected source;
successful SD display logs `SD word image displayed: ... (no HTTP)`.

Use EduHub Role v25 or later so course teaching prefers the SD device tool
when advertised. Topics still use show_text. An asynchronous "started" result
does not confirm the image is displayed. No firmware build/hardware validation
is implied by source/static checks.

### GIF directory and transfer clocks

Run `idf.py menuconfig`, press `/` to search a symbol, and edit:

| Symbol | Default | Meaning |
| --- | --- | --- |
| `BREAD_COMPACT_WIFI_TFT_GIF_DIRECTORY` | `Kira-Emotes/Kira-Emotes/gif` | Directory relative to SD root; spaces supported |
| `BREAD_COMPACT_WIFI_TFT_SPI_MHZ` | `10` | TFT SPI transfer clock in MHz (1..40) |
| `BREAD_COMPACT_WIFI_TFT_SD_MHZ` | `40` | SDMMC transfer ceiling in MHz (10..40) |

For the black-background set at Windows `F:\Kira-Emotes 2\Kira-Emotes\gif`,
enter `Kira-Emotes 2/Kira-Emotes/gif` or `Kira-Emotes 2\Kira-Emotes\gif`.
Both `/` and `\` separators are supported, including mixed separators; firmware
normalizes them to `/` for SD filesystem access and the GIF decoder. Omit `F:`
and any leading/trailing separator. The existing default is preserved until changed in menuconfig.
Save, rebuild and flash; these are build-time settings, not live MCP settings.
Do not edit generated `build/config/sdkconfig.h`. Existing configurations that
lack the new symbols retain their previous defaults. Directory paths with `.`
or `..` components, empty components or more than 128 bytes fail validation.
Directory selection does not recolor GIFs; the black set already contains
opaque #000000 background pixels. The 21-item idle A-button cycle remains.

Use variant `bread-compact-wifi-sh1106-tft-sd` to enable both displays and
SDMMC in 1-bit mode. Connect CLK to GPIO10, CMD to GPIO11 and D0 to GPIO16,
with 3.3 V power, common ground and the required SD pull-ups. SDMMC runs at
40 MHz independently of the TFT SPI bus at 20 MHz.

Format the card as FAT32 and keep GIFs in `/Kira-Emotes/Kira-Emotes/gif`, for example
`/Kira-Emotes/Kira-Emotes/gif/happy.gif`. Firmware never formats a card automatically. Insert the
card before boot; restart after adding files. The index holds up to 64 GIFs,
each at most 4 MiB and 480x480 pixels. Names may contain ASCII letters,
digits, underscores and hyphens, followed by `.gif`.

| Tool | Arguments | Action |
| --- | --- | --- |
| `self.screen.list_sd_gifs` | `{}` | Return card readiness and indexed filenames |
| `self.screen.play_sd_gif` | `{"name":"happy.gif"}` | Play the named GIF repeatedly on TFT |
| `self.screen.show_text` | `{"text":"ocean"}` | Show English text directly, without JPEG/network/S3 |
| `self.screen.show_emoji` | `{}` | Release vocabulary mode and resume emotion GIFs |
| `self.screen.hide_image` | `{}` | Same restoration as show_emoji |

Mounting and file validation run in background tasks. GIF frames are decoded
from the card and fitted to the landscape TFT. The 25-file Kira-Emotes set contains
angry, confident, cool, crying, deadpan, delicious, embarrassed, eyeroll, funny,
happy, hit, kissy, laughing, loving, neutral, relaxed, sad, shocked, sideeye,
silly, sleepy, starstruck, surprised, thinking and winking (`.gif`). Its files
are 160x128 and display at native size. Confused/skeptic uses sideeye because
this set has no confused.gif. SH1106 retains its emoji display. TFT starts with
`neutral.gif` and maps each of the 25 emotion names to its corresponding file.
Listening/curious uses thinking; unknown emotions use neutral.
Only the latest pending emotion is retained while mounting or an image worker
is busy; repeating an emotion does not restart playback. Manual display tools
still work. Vocabulary text/images survive listening/speaking changes until
explicitly replaced, released with show_emoji/hide_image, or the session becomes
idle. On leaving word practice for ordinary chat, the Role must call show_emoji.
Text accepts 1..80 ASCII English letters, spaces, apostrophes and hyphens,
including at least one letter. Long phrases scroll horizontally without clipping.
Topic teaching calls show_text for every new word, including after a successful
repetition; course illustrations still use english_get_word_image + show_image.
Old network/GIF jobs cannot overwrite text or a restored emoji after cancellation.
Listening shows the attentive state (`thinking.gif` on TFT). Speaking defaults
to chatty (`happy.gif`) until an explicit server emotion such as sad or surprised
is received. Server emotion overrides the speaking default, including when it
arrives just before speaking starts. Listening or standby resets that
request for the next turn. Spoken response text alone does not set an emotion;
the server must send an `llm` message with an `emotion` field. Serial logs tagged
`CompactEmotion` show primary OLED expressions; `CompactTft: Emotion requested=...
visible=... GIF=...` identifies the selected TFT animation.
The application now forwards state and server-emotion events through optional
`Board` hooks, independently of the OLED. A missing/failed SH1106, an SSD1306 or
`NoDisplay` primary therefore cannot strand the TFT on neutral. Only the latest
requested emotion is stored; the existing exclusive game, Caro and vocabulary
image modes retain control of the screen until explicitly released. Other boards
inherit no-op hooks. Normal speaking/listening state changes and server events
arrive on the application task; TFT state remains protected by the LVGL lock.

Playback speed depends on decoding, storage and display transfer; OLED changes
immediately, while TFT follows after background loading.

TFT SD playback uses the existing `display/lvgl_display/gif/gifdec` decoder,
the same composition family used by Xiaozhi-Robot's s3cam display. Each frame
is merged into an ARGB8888 canvas. ST7735/ST7789 display that canvas as an LVGL
image; ILI9486 first builds a fitted RGB565 raster and displays it at 1:1. LVGL
renders RGB565; ST7735/ST7789 receive that format, while the ILI9486 IO adapter
expands it to three-byte RGB666. Transparent delta pixels retain earlier pixels
instead of becoming holes exposing the screen background. Native 160x128 GIFs
use scale 256 (1:1) and an explicit 160x128 image object covering the TFT.
This path supports the selected disposal=1 assets; do not assume arbitrary
disposal=3 files render correctly with this decoder.
Both GIF sets use disposal=1 and transparent delta pixels to retain preceding
frame content. Kira-Emotes has an opaque dark blue background (#0C101C) when
composited correctly. Its 160x128 ARGB canvas needs 80 KiB instead of 675 KiB
for 480x360. The physical result still needs testing on the TFT.
Idle starts with neutral and loops indefinitely, independent of file loop
metadata. With its default cycle/game assignment, short-press A in idle selects the next
of 21 emotions, wrapping back
to neutral: neutral, happy, laughing, funny, loving, kissy, sad, crying, angry,
sleepy, surprised, shocked, thinking, confident, sideeye (confused), embarrassed,
winking, silly, delicious, cool, relaxed. Returning from chat to idle resets to
neutral. Outside idle, A retains its game action; wake/BOOT remain available.
Current clocks are TFT SPI 10 MHz and SDMMC 1-bit with a 40 MHz configured
ceiling. The LVGL refresh period is 33 ms (about 30 Hz), not a measured GIF FPS.
The source does not raise these clocks as part of the transparency fix.
On transient SD probe failures, a background worker retries at most four times
with transfer clock ceilings of 40, 20, 10 and 10 MHz, a 300 ms settling delay
and 500/1000/1500 ms between attempts. A successful mount logs its configured
ceiling, not a measured transfer rate. OCR probing still uses the driver's
identification clock; fallback ceilings cannot repair a card that never responds.
No failure formats the card. If all attempts fail, power off, reseat the card
and check power/contacts/pull-ups before rebooting. Text tools work without SD.
The show_text tool must run silently before lesson speech. Its result directs
the model to continue only unsaid speech, or wait if repetition was already
requested; it must not restart the whole lesson after displaying the word.

## Validation

Vocabulary interruption behavior: keep the card during repetition, retries and
word questions. A validated non-screen device MCP tool releases it before the
action executes. This uses a board-installed `McpServer::SetBeforeToolCall`
callback on the application task; other boards retain their existing behavior.
`ShowEmoji()` invalidates pending images and resumes the latest desired emotion.
For off-topic speech without a device tool, use AI Companion Role/context v32
or the interruption rules in `xiaozhi_me_prompt.md`: call `show_emoji {}` before
chat/jokes, suspend the unfinished word without saving/advancing, and redisplay
the same word when practice resumes. Save the new host Role and start a new
conversation. These interruption changes were not built/flashed on Origin;
the user builds this variant. Check cat → chat/joke/dancing → emoji → resume
cat, as well as repeated attempts that keep cat, on the actual board.

Host header validation covers truncated PNG/JPEG headers, oversized PNG
dimensions, invalid JPEG segment lengths and progressive JPEG rejection:

```text
clang++ -std=c++17 -fsyntax-only scripts/tests/compact_tft_image_info_test.cc
clang++ -std=c++17 -fsyntax-only scripts/tests/compact_sd_gif_validation_test.cc
```

A successful firmware build does not verify panel behavior. On hardware, check
the tool-triggered color bars, a PNG and JPEG from each source, repeated replacement,
hide/restore, failed downloads, audio capture/playback and wake-word detection.

Validation includes ESP-IDF 6.0.2 builds, image-header tests and formatting.
On Windows the unmodified host test
runner has four temporary-directory cleanup errors; the tests passed with a
runner-only workaround that leaves each temporary directory before deleting it.
Both panels and network image rendering still require physical hardware testing.
For the SD variant, also check card mounting, GIF playback/replacement/stopping,
missing files and audio operation during animation.
