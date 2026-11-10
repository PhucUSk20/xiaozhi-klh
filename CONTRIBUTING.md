# Contributing to XiaoZhi KLH

Contributions target `klh-dev` in
[PhucUSk20/xiaozhi-klh](https://github.com/PhucUSk20/xiaozhi-klh).
This fork is maintained independently of `78/xiaozhi-esp32`.

1. Fork this repository and create a branch from `klh-dev`.
2. Read [AGENTS.md](AGENTS.md), [the KLH guide](docs/klh-fork.md) and the closest
   existing implementation before making changes.
3. Keep commits focused. Include new source/assets required by your change, but
   exclude build output, downloaded components and personal configuration.
4. Run relevant host checks. For firmware work, use ESP-IDF v6.0.2 and document
   the board and variant. Format only touched C/C++ files with `.clang-format`.
5. Open a pull request to `klh-dev` describing the problem, resulting behavior,
   configuration, checks and physical hardware results (or what remains untested).

```powershell
python -m unittest discover -s scripts/tests -v
clang-format --dry-run -Werror path/to/touched_file.cc
```

On Windows, four existing host tests currently encounter temporary-directory
cleanup errors (`WinError 32`); record actual results rather than claiming a
fully passing suite. The test code does not require a firmware build.

Do not change existing variant pins to support other hardware: add a distinct
variant and document the wiring. Treat board identities as OTA compatibility
identifiers. Preserve the MIT license and third-party notices.

Do not publish credentials, private keys or personal network configuration.
Changes proposed for `klh-stable` must have firmware build and physical hardware
validation recorded for the relevant configuration. No validated KLH release
exists at the initial development snapshot.
