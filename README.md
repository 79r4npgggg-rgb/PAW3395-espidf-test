# PAW3395 test firmware for XIAO ESP32-S3

This is an ESP-IDF v6.0.2 test project for the wired XIAO ESP32-S3 and PAW3395 module.

| PAW3395 signal | XIAO ESP32-S3 pin |
| --- | --- |
| SCLK | GPIO7 / D8 |
| MOSI | GPIO9 / D10 |
| MISO | GPIO8 / D9 |
| NCS | GPIO1 / D0 |
| NRESET | GPIO2 / D1 |
| VDD, VDDIO | 3V3 on this regulator-equipped module |
| GND | GND |
| MOTION | not connected |

The bare PAW3395 IC requires VDD of 1.8-2.1 V, but this board's stated 1.8-6 V VDD input is possible only because it includes its own 2.0 V regulator. Keep VDDIO at 3.3 V or below.

## 1. Build and flash the safe SPI probe

The default configuration reads only the fixed identity registers. It does not write any sensor registers.

```sh
cd /Users/m-swm/Documents/Codex/2026-10-17/referenced-chatgpt-conversation-this-is-an/paw3395-espidf-test
. /Users/m-swm/.espressif/v6.0.2/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build flash monitor
```

Expected result:

```text
Product_ID=0x51 (expected 0x51), Inv_Product_ID=0xAE (expected 0xAE)
SPI probe passed.
```

If either value differs, stop there. Check NCS, MISO, MOSI, SCLK, common ground, and that the board's 3.3 V feeds the module input - do not proceed to phase 2.

## 2. Run the verified initialization and plot movement

After the identity check passes, run:

```sh
idf.py menuconfig
```

Select `PAW3395 test firmware` -> `Test phase` -> `2 - Full datasheet initialization and numeric plot output`, then save, exit, and run:

```sh
idf.py build flash monitor
```

The initial log confirms startup. Every following row has precisely three numeric fields:

```text
SQUAL,Delta_X,Delta_Y
42,0,0
57,-18,6
```

The firmware sends only numeric CSV after the startup log, so it can be used directly by a serial plotter at 115200 baud. SQUAL is the first channel; X and Y are signed 16-bit counts.

## 3. Inspect the 36 x 36 RAW image in a browser

This mode turns the XIAO into a local Wi-Fi access point and shows one still RAW frame each time `Capture` is pressed. Keep the sensor and its target surface completely still during capture.

1. Run `idf.py menuconfig`.
2. Select `PAW3395 test firmware` -> `Test phase` -> `3 - RAW image web viewer (Wi-Fi access point)`.
3. Optionally change the Wi-Fi password in the same menu. The default is `paw3395raw`.
4. Build and flash. Do not run the serial plotter at the same time.
5. Join Wi-Fi network `PAW3395-RAW` from a phone or computer, then open [http://192.168.4.1/](http://192.168.4.1/).

The browser page scales the 36 x 36, 7-bit PAW3395 frame without smoothing. Use `Capture` only when the sensor is still. Bright, structured texture indicates that the lens, illumination, and sensor are seeing the surface; a mostly black or uniform image is a useful indication that focus, alignment, or illumination needs attention.

## 4. Log motion to microSD and monitor it over BLE

This is a separate **Phase 4**. It runs the ordinary PAW3395 motion-read path
from Phase 2, then records every sample to a FAT-formatted microSD card and
sends the same current sample over BLE. It does not enable the Wi-Fi RAW
viewer, so Phase 3 stays independent.

In idf.py menuconfig, select:

~~~text
PAW3395 test firmware
  -> Test phase
  -> 4 - Motion logger (microSD CSV + BLE notifications)
~~~

The default sample interval is 20 ms. The same menu has settings for the SD
flush interval, SD mount retry interval, CS pin, and advertised BLE name.

### microSD wiring

The card and PAW3395 deliberately share one SPI bus. Each device has a unique
chip-select line, so the ESP-IDF SPI driver selects only one device for each
transaction.

| microSD SPI signal | XIAO ESP32-S3 | Shared with / reason |
| --- | --- | --- |
| SCK / CLK | GPIO7 / D8 | Shared PAW3395 SCLK |
| MOSI / DI | GPIO9 / D10 | Shared PAW3395 MOSI |
| MISO / DO | GPIO8 / D9 | Shared PAW3395 MISO |
| CS | **GPIO3 / D2** | Dedicated to microSD; PAW3395 keeps GPIO1 / D0 for NCS |
| VCC | 3V3 | Use a 3.3 V-logic microSD breakout |
| GND | GND | Must be common with XIAO and PAW3395 |

GPIO3 is the default and has been checked against the existing PAW3395 pins
(GPIO1, 2, 7, 8, and 9). The firmware rejects a configured SD CS that conflicts
with any of those pins. Do not use a 5 V SPI output level on any XIAO pin.
The SD adapter's MISO output must release the bus when its CS is high; use a
proper SPI microSD adapter with the normal pull-ups. This is especially
important because PAW3395 and the card share MISO.

The card is mounted at /sdcard without formatting. Use a FAT32-formatted
card. The logger appends to /sdcard/paw3395.csv, writing the header only if
the file is empty:

~~~csv
elapsed_ms,elapsed_us,delta_x,delta_y,squal,motion
60342,60342017,-18,6,57,128
~~~

Both time columns are monotonic time since the firmware boot, not wall-clock
UTC: elapsed_us is the precise timestamp and elapsed_ms is convenient for
plotting. The board has no RTC or time source in this mode, so do not interpret
them as calendar time. Rows are buffered and flushed once per second by
default. If the card is absent at boot, fails to mount, or is removed, motion
sampling and BLE continue; the firmware logs the failure on UART and retries
the mount every five seconds. A clean card removal is still preferred to avoid
losing the last buffered second of rows.

### BLE GATT contract and nRF Connect

The device advertises as PAW3395-Logger by default. Its custom 128-bit primary
service contains the original combined packet plus individual values. Each
characteristic is independently readable and subscribable for notifications:

| Item | UUID | Properties |
| --- | --- | --- |
| Motion logger service | 8f3c0001-8c18-4f6f-a69c-0c3a1df3e001 | Primary service |
| Current motion (existing) | 8f3c0002-8c18-4f6f-a69c-0c3a1df3e001 | Read, Notify; 10-byte packet |
| Delta X | 8f3c0003-8c18-4f6f-a69c-0c3a1df3e001 | Read, Notify; int16_le (2 bytes) |
| Delta Y | 8f3c0004-8c18-4f6f-a69c-0c3a1df3e001 | Read, Notify; int16_le (2 bytes) |
| SQUAL | 8f3c0005-8c18-4f6f-a69c-0c3a1df3e001 | Read, Notify; uint8 (1 byte) |

The existing Current motion characteristic is unchanged: every read and
notification is a fixed 10-byte, little-endian packet. It fits within the
default BLE notification payload, so monitoring does not depend on an MTU
change.

| Byte offset | Type | Meaning |
| --- | --- | --- |
| 0–3 | uint32_le | elapsed_ms since this boot |
| 4–5 | int16_le | delta_x |
| 6–7 | int16_le | delta_y |
| 8 | uint8 | squal |
| 9 | uint8 | PAW3395 Motion register, bit 7 indicates motion |

To monitor on iPhone with **nRF Connect**:

1. Flash Phase 4 and open the Scanner tab. Find PAW3395-Logger and tap
   **Connect**.
2. After service discovery, open
   8f3c0001-8c18-4f6f-a69c-0c3a1df3e001.
3. On 8f3c0002-8c18-4f6f-a69c-0c3a1df3e001, tap the notification bell
   (**Notify**). nRF Connect writes the CCCD; the UART log confirms
   "motion notifications enabled".
4. Notifications now arrive at the configured sample interval. Leave the
   display in hex and decode the fields as little-endian using the table
   above; a read returns the latest packet as a quick connection check.
5. To graph values independently, enable **Notify** on any combination of the
   Delta X, Delta Y, and SQUAL characteristics. Delta X and Delta Y are signed
   16-bit little-endian values; SQUAL is an unsigned 8-bit value. Enabling or
   disabling one characteristic does not affect the others or the existing
   10-byte Current motion notification.

All four notifications are produced from the same PAW3395 sample. They are
sent as separate BLE notifications, so their receive timestamps at the central
can differ slightly.

BLE is intentionally optional from the logger's point of view: a disconnect
only stops radio notifications. PAW3395 sampling and SD CSV recording continue
unchanged while there is no central connection.

## 5. Wireless real-time monitor (Wi-Fi + Chrome)

Phase 5 streams **every** PAW3395 sample at 100 Hz to Google Chrome on a
Windows PC over the XIAO's own Wi-Fi access point. Nothing has to be installed
on the PC: the page (HTML/CSS/JS, no external libraries or CDN) is embedded in
the firmware. Phases 1-4 are unchanged; microSD and BLE are not started in
Phase 5 so that they cannot disturb the sampling period.

### Build and use

1. `idf.py menuconfig` -> `PAW3395 test firmware` -> `Test phase` ->
   `5 - Wireless real-time monitor (Wi-Fi SoftAP + WebSocket)`.
   Settings are in the `Wireless monitor (phase 5)` submenu (table below).
2. `idf.py build flash monitor`. The UART log ends with
   `Join Wi-Fi 'PAW3395-MONITOR' and open http://192.168.4.1/`.
3. Connect the PC to Wi-Fi `PAW3395-MONITOR` (default password
   `paw3395monitor`; change it in menuconfig) and open
   [http://192.168.4.1/](http://192.168.4.1/) in Chrome.
4. **CONNECT** -> **START** -> drive the car -> **STOP** -> **CSV EXPORT**.

Keep the monitor tab in the foreground while recording. Chrome throttles
timers in background tabs, which delays automatic reconnects.

| menuconfig option | Default | Meaning |
| --- | --- | --- |
| Wi-Fi access point SSID | `PAW3395-MONITOR` | 1-32 characters |
| Wi-Fi access point password (WPA2) | `paw3395monitor` | 8-63 characters |
| Wi-Fi channel | 1 | 1-13 |
| Access point IPv4 address | `192.168.4.1` | /24 subnet; DHCP server for the PC |
| Maximum Wi-Fi stations | 2 | Designed for one PC |
| PAW3395 read period (ms) | 10 | 10 ms = 100 Hz; needs `CONFIG_FREERTOS_HZ=1000` |
| Samples per WebSocket packet | 5 | 5 x 10 ms = one packet every 50 ms |
| Sample history kept on the ESP32 | 2048 | Rounded up to a power of two; 16 bytes each (20.48 s at defaults) |
| Send generated test data | off | Link/UI test without moving the sensor; page shows `TEST DATA` |

### What the X and Y values are

The firmware calls the existing `paw3395_read_motion()` unchanged. Delta_X and
Delta_Y are signed 16-bit counts: the motion the PAW3395 accumulated since the
**previous read** (0 when Motion bit 7 is clear). They are the same values as
BLE 0003/0004. BLE 0006/0007 ("graph") carry `delta + 32768` as uint16 only so
that graphing apps show negative values; Phase 5 sends the signed value itself,
so no 32768 offset exists or is subtracted anywhere.

A Delta is therefore displacement per read period, proportional to speed only
while the period is constant. The page and the CSV keep the raw counts; the
trajectory is the running sum of the Deltas (sensor frame, no yaw correction).
Converting counts to a physical value (CPI, lens magnification, mounting
direction and sign) is left to the slope/intercept settings described below.

### How 100 Hz is kept

| Task | Core / priority | Work |
| --- | --- | --- |
| `paw_sampler` (motion_logger.c) | core 1 / 20 | `xTaskDelayUntil` every 10 ms -> `paw3395_read_motion()` -> write to ring buffer. No logging, Wi-Fi or allocation. |
| `paw_ws_sender` (motion_webserver.c) | any / 5 | every 50 ms: read new samples from the ring, encode, send over WebSocket; 1 s status JSON |
| HTTP server, lwIP, Wi-Fi | ESP-IDF defaults (Wi-Fi on core 0) | page delivery, `/ws`, TCP |

- Each sample carries a sequence number (+1 per scheduled read since boot) and
  the `esp_timer` microsecond timestamp taken just before the SPI read.
- If Wi-Fi stalls, the sender catches up with larger packets (up to 64
  samples each) instead of dropping samples. After a reconnect the browser
  asks to resume from the next sequence it needs, so a gap shorter than the
  ring history loses nothing. Anything older is counted as lost, never
  silently skipped.
- If a read starts after its scheduled tick, the schedule restarts (no burst
  of catch-up reads), the sample is flagged `late`, and `late_reads` increments.
  The resulting gap shows up in the measured rate rather than being hidden.
- The 100 Hz SPI read rate is not the PAW3395's internal frame rate. The page
  shows the share of reads with Motion bit 7 set as a hint of whether each read
  carried new motion; it is not a claim that 100 new sensor frames per second
  were captured.

### Monitoring page

- **Dashboard**: ESP32 reachability, WebSocket state, sensor state, `TEST DATA`
  and `REC` markers; acquisition rate (sequence numbers / ESP32 time), received
  rate, received/lost samples and packets, latest SQUAL/X/Y, RSSI seen by the
  ESP32, and recording time/size/lost count.
- **Charts**: X (0006), Y (0007) and SQUAL (0005) versus ESP32 time with a zero
  line, a 1/5/10/30 s window and a raw/physical switch. Charts redraw at about
  30 fps independently of data reception; gaps from lost samples are not bridged.
- **XY trajectory**: running sum of Deltas (raw) or time integral of the scaled
  value (physical), with origin, hold, zoom, auto-fit and reset. Screen up is
  +X (front/back), right is +Y (left/right); axis directions and signs are
  assumptions until checked on the car.
- **Statistics** since START or RESET: max/min/mean/standard deviation of X, Y,
  SQUAL, the magnitude sqrt(X^2 + Y^2), sample count, measured rate and lost
  samples.
- **ESP32 diagnostics** (1 s): period min/max, longest SPI read, Motion bit 7
  share, late reads, read errors, backlog, ring overwrite losses, packets,
  send failures and free heap.

| Button | Action |
| --- | --- |
| CONNECT / DISCONNECT | Open the WebSocket (reconnects automatically) / close it and stop reconnecting |
| START | Start a new recording; also resets statistics and trajectory |
| STOP | Stop recording |
| RESET | Clear charts, trajectory, statistics and loss counters; recorded data is kept |
| CSV EXPORT | Download the stopped recording |

### Physical-value conversion (PC side only)

`X physical = X raw x X slope + X intercept` and likewise for Y, each axis with
its own slope and intercept, plus a unit (m/s, mm/s, cm/s, ...). Changes apply
immediately to the physical charts, statistics and trajectory. The settings are
stored in the browser's `localStorage`, so they survive a reload and do not
depend on which ESP32 is connected. The ESP32 never sees them; raw values are
not modified.

### CSV

The file name contains the start time, sample count and acquisition rate, e.g.
`paw3395_20261116_093404_N2488_100.00Hz.csv`. Lines starting with `#` hold
metadata: export/start/stop times (PC clock), stop reason, firmware, protocol
version, ESP32 boot id, nominal period, samples, first/last sequence, lost
samples/packets, duration, acquisition and received rates, slopes, intercepts,
unit, and one `# gap,<first missing sequence>,<count>` line per gap.

| Column | Meaning |
| --- | --- |
| sequence | sample sequence number since ESP32 boot |
| timestamp_us | ESP32 `esp_timer` microseconds since boot (monotonic, unwrapped) |
| elapsed_ms | time since the first recorded sample |
| squal | SQUAL |
| x_raw, y_raw | Delta_X, Delta_Y int16 counts |
| x_processed, y_processed | raw x slope + intercept, in the configured unit |
| packet_sequence | WebSocket packet sequence number (restarts at 0 per connection) |
| motion | PAW3395 Motion register |
| flags | bit 0 SPI read error, bit 1 synthetic test data, bit 2 late read |

Recording keeps about 23 bytes per sample in the browser and stops
automatically at 2,000,000 samples (about 5.5 h at 100 Hz). Recording also
stops when the ESP32 reboots (new boot id) or on DISCONNECT. The page warns
before a reload or a new START would discard an unexported recording.

### WebSocket protocol v1 (`ws://<ip>/ws`)

After the handshake the ESP32 sends a JSON `hello` (protocol, boot id, period,
samples per packet, ring size, synthetic flag). The browser answers
`{"cmd":"stream"}` for live data or `{"cmd":"stream","from":<sequence>}` to
resume. Status JSON follows once per second. Samples arrive as binary
messages, all fields little-endian (see `main/motion_protocol.h`):

| Offset | Type | Header field (16 bytes) |
| --- | --- | --- |
| 0 | char[2] | magic `PW` |
| 2 | uint8 | protocol version (1) |
| 3 | uint8 | header size (16) |
| 4 | uint8 | sample size (16) |
| 5 | uint8 | sample count (1-64) |
| 6 | uint16 | reserved |
| 8 | uint32 | packet sequence (0 at each connection) |
| 12 | uint32 | nominal sample period in us |

| Offset | Type | Sample field (16 bytes each) |
| --- | --- | --- |
| 0 | uint32 | sample sequence |
| 4 | uint32 | timestamp in us (low 32 bits; the browser unwraps it) |
| 8 | int16 | Delta_X |
| 10 | int16 | Delta_Y |
| 12 | uint8 | SQUAL |
| 13 | uint8 | Motion register |
| 14 | uint8 | flags |
| 15 | uint8 | reserved |

Only one browser streams at a time; a new connection replaces the old one.

### Testing the page without hardware

`tools/mock_device.py` (Python 3 standard library only) serves `main/web` and
emulates the same HTTP/WebSocket protocol with generated data:

~~~sh
python tools/mock_device.py --port 8080
# open http://127.0.0.1:8080/ in Chrome
~~~

`--drop-every N` omits every N-th packet, `--disconnect-every S` closes the
connection every S seconds, and `--stall-at S --stall-seconds T` pauses
sending once per connection, so loss detection, resume and CSV gap reporting
can be checked.

### Checks on the real car

| Item | Pass criterion |
| --- | --- |
| Wi-Fi | Windows connects to `PAW3395-MONITOR` and stays connected |
| Web page | `http://192.168.4.1/` opens in Chrome |
| WebSocket | Stays connected; reconnects and resumes after Wi-Fi interruptions |
| Sensor reads | Acquisition rate about 100 Hz, period about 10 ms, read errors 0 |
| Display | X/Y/SQUAL update in real time |
| CSV | Every received sample is saved; gaps are listed |
| Loss detection | Lost samples are counted from sequence numbers |
| Endurance | 10 minutes or longer recording |
| Existing function | Phases 1-4 still build and behave as before |

Suggested runs: stationary, slow straight, forward/reverse, slalom, full
acceleration, hard stop and lateral slip. The CSV `x_raw`/`y_raw` columns are
the same values as BLE 0003/0004 (0006/0007 minus 32768), so earlier BLE
Insight CSV files can be compared directly.

## Design notes

- SPI is Mode 0 at 1 MHz. PAW3395 samples MOSI on rising SCLK edges and presents MISO after falling edges.
- In Phase 4, the microSD card is added as a second SPI2 device. The SPI driver
  applies each device's speed and CS independently; PAW3395's manually timed
  NCS sequence remains unchanged.
- Reads keep NCS low across the PAW3395-required 2 us address-to-data delay. Auto-CS transactions alone would not meet that requirement.
- The 137-write power-up table, its 1 ms/60-attempt readiness polling, fallback, and final bank writes are transcribed from PAW3395DM-T6QU datasheet v0.8 section 6.2.
- The project disables rest mode after initialization (`Performance=0x80`) because the datasheet specifies that SQUAL is valid in run mode only.
- RAW capture follows the dedicated procedure in datasheet section 7.1. It is intentionally a separate test phase because it temporarily takes the sensor out of ordinary motion-read operation.
