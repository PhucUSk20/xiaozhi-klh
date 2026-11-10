# XiaoZhi KLH: setup and development

This is an independently maintained fork of
[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32), not an official upstream
release. The original history and MIT license are retained. Existing upstream
documentation remains available from the root README.

## Branches and validation

- `klh-dev` is the default branch for KLH development and contributions.
- `klh-stable` starts at upstream commit
  `dd99da0` (Add M5Stack CoreP4 board support). Initially it does not contain the
  KLH customizations. Move validated KLH changes here only after firmware builds
  and physical testing; record the tested commit, board, wiring and SDK version.
- `feature/kleaderbit-web-remote-control` remains the original local working branch.

The initial KLH development snapshot includes existing local source changes and
their assets. It has not received a complete firmware build or hardware
validation as a release. The maintainer builds Origin firmware locally; firmware
builds are not run by the assistant unless explicitly requested.

The current Windows host test baseline is 44 passing tests and four errors caused
by temporary-directory cleanup while the process still uses that directory
(`WinError 32`). Pin and microphone configuration paths have also been checked
with preprocessing; this does not establish audio or display operation on hardware.

## Get the source

```powershell
git clone -b klh-dev https://github.com/PhucUSk20/xiaozhi-klh.git
cd xiaozhi-klh
git switch -c feature/my-change
```

For contributions, fork `PhucUSk20/xiaozhi-klh` into your own account first, clone
your fork, and open a pull request targeting this project's `klh-dev` branch.
You do not need write access to the upstream project.

## SDK and build configuration

Install ESP-IDF **v6.0.2** and open its PowerShell environment (or source its
`export.sh` on Linux/macOS). Check the active version before building:

```powershell
idf.py --version
python scripts/build.py --list-boards
```

The main KLH hardware is `bread-compact-wifi`, targeting ESP32-S3. The complete
variant list is in [the board configuration](../main/boards/bread-compact-wifi/config.json).
For SH1106 + ILI9341 320x240 + SD with the new TFT pins:

```powershell
python scripts/build.py bread-compact-wifi --name bread-compact-wifi-sh1106-ili9341-320x240-sd-cs38
```

That named variant selects the shared-clock microphone by default. For manual
configuration, open `idf.py menuconfig` after configuring the ESP32-S3 target and
select the board under `Xiaozhi Assistant`. Build that configuration using
`idf.py build`. The canonical `scripts/build.py` command regenerates sdkconfig
from the named variant, so it can overwrite manual menuconfig selections.

Use a uniquely named variant in the board's `config.json` when distributing a
different combination, including legacy microphone wiring. Do not replace the
pins of an existing variant. Follow [the board guide](custom-board.md).

Local build state, generated output and downloaded components are excluded from
Git. A fresh checkout downloads dependencies through ESP-IDF Component Manager.
The source under `local_components/esp-wifi-connect` is an intentional maintained
dependency override referenced by `main/idf_component.yml`; keep it in the checkout.

## TFT and microphone wiring

In `Xiaozhi Assistant`, enable and enter `Add secondary SPI TFT for images`.
Choose the matching controller and toggle
`Use TFT pins CS38/RST40/DC41 (off: CS12/RST14/DC13)` as required:

| Signal | Original TFT wiring (option off) | New TFT wiring (option on) |
| --- | --- | --- |
| CS | GPIO12 | GPIO38 |
| RST | GPIO14 | GPIO40 |
| DC | GPIO13 | GPIO41 |
| MOSI | GPIO15 | GPIO15 |
| SCK | GPIO2 | GPIO2 |

The new TFT pins free GPIO12/13/14. After enabling them, return to the main
`Xiaozhi Assistant` menu to enable `Use legacy microphone pins
(GPIO14/GPIO13/GPIO12)` if that matches your wiring:

| Microphone mode | BCLK/SCK | WS | DIN |
| --- | --- | --- | --- |
| Shared clock (default) | GPIO47 | GPIO48 | GPIO45 |
| Legacy | GPIO14 | GPIO12 | GPIO13 |

Legacy microphone wiring is available when TFT is disabled or uses the new
pins. It is blocked with the original TFT pins because they overlap.
SH1106 uses SDA GPIO8 / SCL GPIO9. SDMMC uses CLK GPIO10 / CMD GPIO11 / D0 GPIO16.
Connect TFT backlight separately. See the
[board documentation](../main/boards/bread-compact-wifi/README.md) for panel,
orientation, image, GIF and diagnostic settings.

## KLH customizations

- Robot control web pages, configurable controls and rock-paper-scissors behavior.
- SH1106 emoji display, button actions and wheel movement configuration.
- Secondary ST7735, ST7789, ILI9341 and ILI9486 display paths, image tools,
  optional SD image/GIF support and diagnostics.
- K210 UART camera bridge; see
  [the connection guide](../main/boards/bread-compact-wifi/K210_CAMERA_UART.md).
- Optional Robot Hub music tools and startup test. Server-side accounts and
  MCP connections must be configured separately; see the board README.
- Audio mode changes and supporting application/MCP integration.
- Locally maintained Wi-Fi provisioning component and Windows CMake response files.

These are development features. Select the options matching your actual board;
source availability does not imply every combination has been tested.

## Contributing and keeping up with upstream

Read [CONTRIBUTING.md](../CONTRIBUTING.md) and [AGENTS.md](../AGENTS.md).
On a fresh clone, add the original project as `upstream`:

```powershell
git remote add upstream https://github.com/78/xiaozhi-esp32.git
git fetch upstream
```

Integrate upstream changes on a separate branch, resolve conflicts there and
open a pull request to `klh-dev`. Do not force-push shared branches. Changes to
`klh-stable` must include actual firmware and hardware validation notes.

Keep credentials, local Wi-Fi settings, generated sdkconfig, build directories,
downloaded components and release binaries out of source commits. Keep the
original license and third-party notices with redistributed source.
