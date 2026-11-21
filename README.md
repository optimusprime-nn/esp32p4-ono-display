# ESP32-P4 ONO Display

Wall/desk display for **Open Network Observatory (ONO)** on the **Guition
JC8012P4A1**: a 10.1" 800×1280 touch panel with an ESP32-P4 and an ESP32-C6
Wi-Fi co-processor.

It polls the ONO HTTP API over Wi-Fi and shows, at a glance, whether anything
on the network needs attention. The board, display, touch, Wi-Fi, OTA and
crash-log code comes from
[esp32p4-adsb-display](https://github.com/Optimusprimeums/esp32p4-adsb-display),
so build, flashing, board revisions and the C6 update work the same way.

## Screens

| Tab | Shows |
|---|---|
| **Overview** | Status hero card (*N down* in red / *N open alerts* in amber / *All quiet* in green, with the names of devices that are down), tiles for devices, SNMP-managed devices, traffic in the last hour, flow exporters, service checks and discovery, a per-minute traffic chart for the last hour, top talkers and top services |
| **Events** | ONO's event feed (severity colour, subject, kind, age, message). *Open only* hides acknowledged events. Tap an event to read it in full and **acknowledge** it |
| **Network** | Devices down, failing service checks, inventory by device type, backups / compliance / remote collectors, discovery state, and connection details (server, last poll time, display IP) |
| **Settings** | Wi-Fi scan, SSID and password, ONO URL and API token, HTTPS certificate check, refresh interval, wake-on-alert, brightness, panel VCOM, 180° rotation, screen saver, time zone, firmware upload and rollback, C6 update |

With **Wake on alert** on (the default), the screen turns on and jumps to
Overview when the number of devices down or failing service checks goes up,
even while the screen saver has the backlight off.

## Preparing ONO

ONO listens on `127.0.0.1:8787` by default, which the display cannot reach.

1. In `ono.toml` (on OPNsense: **Services → Network Observatory → Settings**),
   make the dashboard listen on the LAN and protect it:

   ```toml
   [server]
   listen = "0.0.0.0:8787"
   token  = "a-long-random-string"
   ```

   Or keep your user accounts and create an **API token** for the display in
   the ONO web UI (Users → API tokens). A read-only token is enough unless you
   want to acknowledge events from the display.
2. Allow TCP 8787 from the display's IP if a firewall sits in between.
3. Check from a PC:

   ```bash
   curl -H "Authorization: Bearer <token>" http://<ono-host>:8787/api/v1/overview
   ```

If ONO is served over HTTPS with a self-signed certificate (e.g. the OPNsense
GUI certificate), use an `https://` URL and turn **Verify HTTPS** off.

## Build and flash

Requires ESP-IDF 6.1 (the same as the ADS-B display).

```bash
cd esp32p4-ono-display
idf.py build
idf.py -p <port> flash monitor
```

Then set Wi-Fi, the ONO URL and the token in the **Settings** tab and press
**Save and apply**.

- Older boards with ESP32-P4 silicon rev v1.x:
  `idf.py -B build_rev1 -D SDKCONFIG=build_rev1/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.rev1" build`
- Private build-time defaults (Wi-Fi, URL, token): copy
  `sdkconfig.defaults.local.example` to `sdkconfig.defaults.local` (git-ignored).
- Later updates go over Wi-Fi: **Settings → Allow upload**, then open
  `http://<display-ip>/` and upload `build/ono_display.bin`, or
  `curl --data-binary @build/ono_display.bin http://<display-ip>/update`.
  New images boot "pending verify" and roll back automatically if the UI
  doesn't come up or Wi-Fi doesn't connect.

## How it talks to ONO

Every refresh (10 s by default) the client task on core 1 makes these
requests over one keep-alive connection, the same endpoints ONO's own web
dashboard uses:

| Request | Used for |
|---|---|
| `GET /api/v1/overview` | devices, down list, open alerts, services, backups, compliance, collectors, kinds |
| `GET /api/v1/stats` | NetFlow collector counters and discovery (absent for site-limited accounts) |
| `GET /api/v1/traffic/series?minutes=60` | traffic chart |
| `GET /api/v1/traffic/top?by=talkers&minutes=60&limit=8` | top talkers |
| `GET /api/v1/traffic/top?by=ports&minutes=60&limit=8` | top services |
| `GET /api/v1/events?limit=40` | event feed |
| `POST /api/v1/events/ack` `{"ids":[id]}` | acknowledge (only when you press it) |

Only `/overview` is required; if the others fail, those panels just stay empty.
On an error the last good data stays on screen, marked stale in the top bar.

## Source layout

```text
main/
  main.c            start-up order, LVGL task
  ono_client.c      ONO API polling, JSON parsing, acknowledgements
  ui.c              LVGL user interface
  settings.c        NVS-backed settings, timezone table
  board.c           LCD, touch, backlight, VCOM, flip, screen saver   (from the ADS-B display)
  panel_jc8012_v2.h JD9365 init sequence for the V2 panel           (from the ADS-B display)
  wifi.c            Wi-Fi via the ESP32-C6, scan, SNTP              (from the ADS-B display)
  ota.c             web upload, rollback, C6 update                 (from the ADS-B display)
  crashlog.c        panic reason across reboots                     (from the ADS-B display)
components/esp_lcd_touch_gsl3680   GSL3680 touch driver             (from the ADS-B display)
c6_coprocessor/                    ESP-Hosted firmware for the C6   (from the ADS-B display)
```

## Licence

GPL-3.0-or-later, like the ADS-B display it is derived from — see
[LICENSE](LICENSE).
