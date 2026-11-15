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

## Design notes

- SPI is Mode 0 at 1 MHz. PAW3395 samples MOSI on rising SCLK edges and presents MISO after falling edges.
- Reads keep NCS low across the PAW3395-required 2 us address-to-data delay. Auto-CS transactions alone would not meet that requirement.
- The 137-write power-up table, its 1 ms/60-attempt readiness polling, fallback, and final bank writes are transcribed from PAW3395DM-T6QU datasheet v0.8 section 6.2.
- The project disables rest mode after initialization (`Performance=0x80`) because the datasheet specifies that SQUAL is valid in run mode only.
- RAW capture follows the dedicated procedure in datasheet section 7.1. It is intentionally a separate test phase because it temporarily takes the sensor out of ordinary motion-read operation.
