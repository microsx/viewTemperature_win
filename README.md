# viewTemp

Tiny Windows overlay that shows CPU/GPU temperature, usage, VRAM and system
RAM in a top-right always-on-top window. Reads sensor data from the
[MSI Afterburner Hardware Monitoring](https://www.msi.com/Landing/afterburner)
shared memory block — viewTemp itself does not access any hardware directly.

## Features

- CPU/GPU temperature, usage, VRAM (used / total / percent), system RAM
- Threshold-based flashing alert (per-row 360 ms heartbeat on threshold breach)
- ini-based hardware database: CPU/GPU model matched at startup to pick
  sensible temperature thresholds automatically
- Position persistence, hot-reload of ini settings, top-most transparent
  overlay with hover-fade-in interactivity
- UI language auto-detected from the Windows user UI language at startup
  (Chinese / English supported — add more in `LANG_IF` calls)

## Build

Requires Visual Studio 2019/2022 with the C++ desktop workload and the
Windows 10+ SDK.

```cmd
build.bat
```

Output: `viewTemp.exe` (single self-contained binary, no DLLs to ship).

## Run

Start **MSI Afterburner** (with RivaTuner Statistics Server) first — viewTemp
reads sensor data from its shared memory block. Without Afterburner running,
only the system RAM and (read) VRAM total fields will populate; temperature
and usage will show `--`.

## Configuration

A `viewTemp.ini` file is created next to the executable on first run. It
holds the persistent window position, display flags, and the `[threshold]`
section with per-CPU/GPU alert thresholds. Edit it freely — changes are
picked up within ~1 second without restarting.

## Known issues

See [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for runtime requirements
(MSI Afterburner must be running), VRAM total caveats, and intentional
limitations.

## License

MIT — see [LICENSE](LICENSE).