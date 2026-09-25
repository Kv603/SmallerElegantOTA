# Async ESP32 footprint and heap regression policy

The `async-esp32-size` PlatformIO environment is the repeatable ESP32 baseline
for the `examples/AsyncDemo` sketch. It uses the `esp32dev` board and the
repository-owned `partitions/elegantota_4mb_ota.csv` scheme: two 1.875 MiB OTA
application slots and a 192 KiB SPIFFS partition on a 4 MiB flash device.

Build and print the report locally with:

```sh
PLATFORMIO_SRC_DIR=examples/AsyncDemo pio run -e async-esp32-size
python3 tools/report_esp32_size.py \
  --elf .pio/build/async-esp32-size/firmware.elf \
  --map .pio/build/async-esp32-size/firmware.map
```

The report reads the target toolchain's ELF section table and the generated OTA
binary. It prints `.text`, `.rodata`, `.data`, `.bss`, and the actual `.bin`
image size. The CI job writes the same report to `async-esp32-size-report.json`
and uploads it with the ELF and map so a change can be investigated without
rebuilding.

## Accepted regressions

Compare a pull request with the latest successful `Async ESP32 Footprint`
artifact from the target branch. A change is accepted when all of the following
are true:

| Measurement | Allowed regression |
| --- | ---: |
| `.text` + `.rodata` | at most 16 KiB |
| `.data` + `.bss` | at most 4 KiB |
| OTA `.bin` image size | at most 16 KiB |
| `ESP.getFreeHeap()` at every checkpoint | no decrease greater than 8 KiB |
| `ESP.getMinFreeHeap()` at every checkpoint | no decrease greater than 8 KiB |

Larger increases require an issue or pull-request note explaining the feature,
the before/after report values, and confirmation that the image still fits in
the 1.875 MiB OTA slot. Improvements (smaller sizes or higher heap readings)
are always accepted.

## Runtime heap capture

`AsyncDemo` logs free and minimum-free heap immediately before
`ElegantOTA.begin()`, immediately after it registers its routes, on upload
start, approximately once per second during upload, and on upload completion.
Use a firmware image large enough to produce at least one progress sample, and
retain the serial log with the change review. The ESP32 measurements are emitted
as `[heap] <checkpoint>: free=<bytes> min_free=<bytes>`.

With `ELEGANTOTA_DEBUG=1`, route setup also logs a rolling free-heap delta
after each `AsyncWebServer::on()` call:

```
[ElegantOTA] async route /update: free_heap=<bytes> delta=<bytes>
```

The deltas make the library's retained route-handler allocations visible. This
repository pins `esp32async/ESPAsyncWebServer` to 3.12.0 for that comparison:
the dependency allocates one retained callback handler per `on()` registration,
so `/ota/metadata` and `/ota/start` remain separate routes to preserve the
portal's existing GET API. The bundled portal is served from `/update` and
makes same-origin requests to these endpoints; it does not need CORS or a
forced `Connection: close` response header.
