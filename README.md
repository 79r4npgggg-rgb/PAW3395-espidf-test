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

## Design notes

- SPI is Mode 0 at 1 MHz. PAW3395 samples MOSI on rising SCLK edges and presents MISO after falling edges.
- In Phase 4, the microSD card is added as a second SPI2 device. The SPI driver
  applies each device's speed and CS independently; PAW3395's manually timed
  NCS sequence remains unchanged.
- Reads keep NCS low across the PAW3395-required 2 us address-to-data delay. Auto-CS transactions alone would not meet that requirement.
- The 137-write power-up table, its 1 ms/60-attempt readiness polling, fallback, and final bank writes are transcribed from PAW3395DM-T6QU datasheet v0.8 section 6.2.
- The project disables rest mode after initialization (`Performance=0x80`) because the datasheet specifies that SQUAL is valid in run mode only.
- RAW capture follows the dedicated procedure in datasheet section 7.1. It is intentionally a separate test phase because it temporarily takes the sensor out of ordinary motion-read operation.
