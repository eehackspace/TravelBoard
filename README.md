# Departure Board S3

A live departure board on a 64×32 LED matrix, powered by an Adafruit MatrixPortal S3. It cycles between three feeds:

- **Trains** — next departures between two UK stations (default: Hockley → London Liverpool Street), with platform, delay/cancellation status, and minutes to departure
- **Buses** — next departures from a chosen UK bus stop, filtered to a specific route and direction (default: the Arriva 7 towards Rayleigh)
- **Overhead aircraft** — the nearest plane to your location, with callsign, route (for airliners) or aircraft type (for light aircraft), altitude, and distance

The clever bit: it hides departures you can't actually catch. Set how long it takes you to reach the station and the bus stop, and anything leaving sooner than that never appears.

Total build cost: about £30. No subscriptions.

![Departure board](docs/board.jpg)
*(Add a photo of your board here)*

## Hardware

| Part | Notes |
|---|---|
| Adafruit MatrixPortal S3 (or compatible) | Plugs directly onto the panel's HUB75 socket |
| 64×32 RGB LED matrix panel, HUB75, 1/16 scan | P4 (256×128mm) or P3 both work |
| USB-C power supply, 2A+ | A phone charger brick is fine; the board powers the panel |
| Panel power cable | 4-pin plug to panel, spade terminals to the MatrixPortal's screw standoffs |

Assembly: push the MatrixPortal onto the panel's **input** HUB75 socket, screw the spade terminals onto the +5V and GND standoffs (peel the protective stickers off first!), plug the 4-pin end into the panel, and power the MatrixPortal over USB-C. No soldering.

The sketch keeps brightness at 50% by default, which is comfortable indoors and well within USB power limits for text.

## Software setup

1. Install the **Arduino IDE 2.x** and add ESP32 board support (Boards Manager → search "esp32" by Espressif).
2. Install two libraries via the Library Manager:
   - **Adafruit Protomatter** (accept the dependencies, e.g. Adafruit GFX)
   - **ArduinoJson** (v7.x)
3. Select the board **Adafruit MatrixPortal ESP32-S3** and set **Tools → USB CDC On Boot: Enabled**.
4. Open `departure_board_s3_public.ino`, fill in the config (see below), and upload.

**Upload tips for the S3:** if the port refuses to open, hold **BOOT**, tap **RESET**, release BOOT to enter the bootloader, then reselect the (possibly new) COM port and upload again. The first upload attempt sometimes fails at the reboot step; running it a second time usually works.

## Configuration

Everything lives in the `CONFIG` block at the top of the sketch.

### WiFi
```c
#define WIFI_SSID      "your-network"
#define WIFI_PASSWORD  "your-password"
```
2.4GHz only (the ESP32-S3 can't see 5GHz). Guest networks with captive portals won't work.

### Trains — Realtime Trains (free)
1. Register at https://api-portal.rtt.io (free for personal, non-commercial use).
2. Copy the long refresh token (starts `eyJ...`) into `RTT_REFRESH_TOKEN`. The sketch automatically exchanges it for short-life access tokens and refreshes them as needed.
3. Set `TRAIN_ORIGIN` and `TRAIN_DEST` to your stations' three-letter CRS codes (find them on realtimetrains.co.uk).
4. Set `TRAIN_LEAD_MIN` to how many minutes it takes you to reach the station — trains leaving sooner are hidden.

### Buses — TransportAPI (free tier)
1. Sign up at https://developer.transportapi.com and create an app to get an `app_id` and `app_key`.
2. Find your stop's **ATCO code** on https://bustimes.org — it's the number in the stop page's URL (e.g. `bustimes.org/stops/15001101515`). Count the digits carefully; a truncated code returns HTTP 400.
3. Set `BUS_LINE` and `BUS_DEST_MATCH` to your route number and the destination text that identifies your direction of travel.
4. Set `BUS_LEAD_MIN` to your walking time to the stop.

**Important:** the TransportAPI free tier allows only **30 requests per day**, which is why the sketch polls buses hourly and uses scheduled (`nextbuses=no`) rather than live times. Repeatedly rebooting the board while testing will burn through the quota (each boot fetches once) and return HTTP 429 for the rest of the day.

### Aircraft — airplanes.live + adsbdb (free, no key)
Set `LAT`/`LON` (and the matching `LAT_STR`/`LON_STR` strings) to your location, and `RADIUS_NM` to the search radius in nautical miles (5–15 is sensible). Airliner callsigns get a route lookup (e.g. `LHR>JFK`); light-aircraft registrations show the aircraft type instead.

## How it behaves

On boot: `wifi...` → `loading` → then pages rotate every 5 seconds. The plane page only appears when an aircraft is in range. Trains refresh every 60s, planes every 10s, buses hourly (free-tier quota).

Colours: amber for normal departures, red for delays and cancellations, green for routes/types, an asterisk marks a live (rather than scheduled) bus estimate.

The board derives UK local time itself from NTP + explicit BST rules, so times stay correct year-round regardless of the device timezone.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Page stuck on "loading" | That feed's fetch is failing — check the Serial Monitor (115200) for the HTTP code |
| `[RTT token] no token` / 401 | Refresh token clipped when pasting — recopy it in full |
| `[BUSES]` HTTP 400 | ATCO code wrong or truncated |
| `[BUSES]` HTTP 429 | Daily 30-request quota spent — resets next day |
| `[HTTP] 404 ...adsbdb...` | Harmless: light aircraft have no route to look up |
| Board USB connect/disconnect loop | Bootlooping firmware — hold BOOT, tap RESET, erase-flash and re-upload |
| No serial output after upload | USB CDC On Boot not enabled, or wrong monitor baud (use 115200) |
| WiFi connects but no data | Captive-portal guest network, or 5GHz-only SSID |

## Notes

- Realtime Trains data is for personal, non-commercial use; public-facing use requires visible credit to [Realtime Trains](https://www.realtimetrains.co.uk).
- Aircraft data comes from the community ADS-B network via [airplanes.live](https://airplanes.live), with route enrichment from [adsbdb.com](https://www.adsbdb.com).
- Bus data via [TransportAPI](https://www.transportapi.com); stop lookups courtesy of [bustimes.org](https://bustimes.org).

Built at [East Essex Hackspace](https://eastessexhackspace.org.uk).

## License

MIT — see [LICENSE](LICENSE).
