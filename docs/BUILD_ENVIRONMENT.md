# 🧰 Build environment notes (sandbox quirks)

The sandbox workspace is snapshotted with a **~10,000-file cap**. The ESP-IDF tree
(~18,300 files) would eat the entire budget (it once truncated the firmware
sources themselves!), so since the "compact release" rebuild the layout is:

- **ESP-IDF v5.5.4 lives in `/home/user/.cache/esp-idf`** — `.cache` is excluded
  from workspace snapshots, so the 18k-file tree never counts against the cap.
  It is re-cloned automatically by `tools/ensure_idf.sh` when missing (a shallow
  clone takes ~10 s).
- **Toolchain + venv live in `~/.espressif`** (big files, few thousand entries).
  If the snapshot truncates them, `tools/ensure_idf.sh` reinstalls what's missing
  (`install.sh` + `idf_tools.py install <tool>` per missing tool).
- **Authored sources are backed up** in `orbit-sources.tar.gz` (ONE file — immune
  to file-count truncation). `tools/restore_sources.sh` re-extracts it, and
  `tools/ensure_idf.sh` runs it automatically when firmware sources are missing.

## One command

```bash
cd /home/user/orbit-assistant && bash tools/build_release.sh
```

This self-heals the environment (`tools/ensure_idf.sh`), builds the firmware,
regenerates the wake-word assets image, and assembles:

- `firmware/release/orbit_firmware.bin` — **main deliverable** (trimmed, ~7.7 MB, flash at 0x0)
- `firmware/release/orbit_full_flash.bin` — full 16 MB image (only if you want it)
- individual parts (`bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin`,
  `orbit.bin`, `assets.bin`) + `FLASH.md` / `RELEASE.md`

## Layout & self-healing details

| Piece | Where | Repaired by |
|---|---|---|
| ESP-IDF v5.5.4 tree | `/home/user/.cache/esp-idf` (snapshot-excluded) | fresh shallow clone if <15k files |
| Unused components (mqtt, bt, …) | moved to `components-disabled/` inside the tree | `mv` on every ensure run |
| Submodules (lwip, mbedtls, cJSON, unity, tlsf, spiffs, wifi/phy/coex libs, micro-ecc) | inside the tree | re-clone at pinned SHAs if <70% files |
| Managed components (esp-sr, esp-dsp, dl_fft, cjson, esp_websocket_client) | `firmware/managed_components/` | wiped + re-downloaded by the component manager when truncated (the `dependencies.lock` otherwise makes it a no-op — also wipe `firmware/build` so CMake reconfigures) |
| Toolchain + venv | `~/.espressif` | `install.sh esp32s3` + per-tool `idf_tools.py install` |
| System packages (cmake, ninja, libusb) | apt | installed if missing |
| Firmware sources, tools, docs, assets | `/home/user/orbit-assistant` | `orbit-sources.tar.gz` via `tools/restore_sources.sh` |

## Gotchas (IDF 5.5)

- `idf.py` has no `-j` — use `CMAKE_BUILD_PARALLEL_LEVEL=2`.
- `-Werror=format-truncation` — use precision specifiers (`"%.31s"`) or size buffers.
- `inline` is a C keyword — never use it as a variable name.
- `esp_websocket_client_config_t` has no `timeout_ms` (use `network_timeout_ms` /
  `pingpong_timeout_sec`); `ping_interval_sec` is the keepalive ping period.
- The component manager runs during `idf.py build` when the manifest is present
  (`main/idf_component.yml` and/or the project root); a stale `build/` cache can
  skip it — wipe `build/` when `managed_components/` was truncated.
- `EXCLUDE_COMPONENTS` does not stop CMakeLists processing — physically move
  unwanted components out of the tree instead.
- ESP32-S3 deep sleep has no `esp_deep_sleep_enable_gpio_wakeup` — use
  `esp_sleep_enable_ext1_wakeup()` + the RTC timer wake.
- esptool flash: `esptool.py --chip esp32s3 -b 921600 write_flash 0x0 orbit_firmware.bin`
