# Known Issues

A list of things that are known, intentional, or currently out of scope.
Read this before opening an issue — if your question matches one of the
sections below, it's probably not a bug.

## Multi-GPU and integrated graphics (iGPU)

- **Mixed-mode (iGPU + dGPU) is fully supported.** viewTemp enumerates every
  GPU entry that MAHM reports (typically 2 in a hybrid-mode laptop:
  the dGPU at index 0 and the iGPU at index 1) and renders each card as a
  4-row block (name / temp / usage / VRAM).
- **iGPU temperature is not available.** MAHM does not fill
  `MAHM_SRC_GPU_TEMPERATURE` for Intel integrated graphics; the iGPU
  temperature row will display `--`. Usage and VRAM-percentage are usually
  populated by MAHM and are visible.
- **VRAM total for the iGPU is not available.** DXGI reports
  `DedicatedVideoMemory = 0` for Intel iGPUs (the iGPU shares system RAM,
  not dedicated VRAM). When no DXGI total is found, the VRAM row shows
  percentage only (`12.3%`) instead of `0.20G / -G (12.3%)`. This is
  intentional — there is no public API that exposes iGPU-shared VRAM as a
  fixed number.
- **Per-card show/hide.** The right-click menu has one checkbox per
  detected card (menu ID = `1015 + i`). Toggling writes the state to
  `viewTemp.ini` under `[view] ShowGpu_<PCI-path-sans-&-and-=>=`. The
  ini key is derived from the PCI device path (stable across reboots,
  BIOS mode switches, and adapter reordering), so toggling a card keeps
  working even if MAHM happens to report it at a different index on the
  next launch.
- **Same-vendor multi-GPU** (e.g. two NVIDIA cards) is rare in viewTemp's
  target use case (consumer laptops / desktops). DXGI adapter matching
  uses VendorId with "first match wins" — the second card may get the
  first card's VRAM total until we add a better disambiguator.

## Required runtime

- **MSI Afterburner must be running.** viewTemp reads sensor data from the
  [MAHM shared memory block](https://www.msi.com/Landing/afterburner).
  Without Afterburner (and RivaTuner Statistics Server) running, the
  temperature, usage, and VRAM-percentage fields will show `--`. The
  window, RAM display, and VRAM total from DXGI still work — they don't
  depend on MAHM.

## VRAM total vs. physical VRAM

- The "total" half of the VRAM display (`X / Y`) comes from
  `IDXGIAdapter::GetDesc().DedicatedVideoMemory`. This is the **OS-visible**
  amount, not the physical VRAM. NVIDIA drivers reserve roughly 256 MB
  for their own use, so an 8 GB RTX 4060 Laptop GPU shows `7.77 GB`, a
  12 GB RTX 3060 shows `11.77 GB`, a 10 GB RTX 3080 shows `9.77 GB`.
  This is by design (WDDM reserves driver-internal memory and does not
  expose it). Task Manager's "8 GB" is the physical amount; viewTemp's
  "7.77 GB" is the amount applications can actually allocate.
- The "used" half is computed from MAHM's percentage × the DXGI total,
  so it inherits the same caveat — it's an estimate of used VRAM
  relative to OS-visible memory.

## After exiting click-through mode

- Once the window has been switched to click-through
  (`WS_EX_TRANSPARENT`) — e.g. after the cursor leaves and hover-fade-in
  finishes — the cursor cannot re-enter the window without a double-click
  to toggle the mode. This is intentional; implementing auto re-entry
  would require timer-based mode flipping and is out of scope.

## Window position

- The window always opens in the **top-right corner of the primary
  display** at every launch. Position is not persisted across restarts.
  The right-click context menu has a "Reset Position" item that snaps
  it back if you've dragged it somewhere.

## Hover-fade-in

- The interactive cue (dark-green panel + bright-green border) appears
  instantly when the cursor enters and has been still for 600 ms. There
  is no smooth fade animation — it pops.

## Threshold database coverage

- The `hardware_db.h` table matches about 20 popular Intel/AMD/NVIDIA
  CPUs and GPUs against official Tjunction data. CPUs/GPUs not in the
  table fall back to the default 85 °C threshold. If your hardware
  isn't matched, the right-click menu's "Auto-match Hardware Thresholds"
  item will tell you in the log (`-> 85 (default)`); you can then
  manually set the threshold in `viewTemp.ini`'s `[threshold]` section
  to whatever you want.

## Language

- The UI auto-detects **Chinese / English** from `GetUserDefaultUILanguage`
  at startup. Other languages fall through to English. To add a new
  language, edit the `LANG_IF(zh, en)` macro calls in `viewTemp.cpp`
  and add a third branch.