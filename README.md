# OpenBird

Door station firmware for the **Olimex ESP32-PoE-ISO**, compatible with the
DoorBird LAN API. It works with the existing nymea DoorBird plugin, including
discovery and doorbell events.

- Ethernet (LAN8720) with DHCP, powered via PoE
- Doorbell button input → `doorbell:H` / `doorbell:L` events on `monitor.cgi`
- Door opener relay output → `open-door.cgi?r=1`
- Optional: second relay (`r=2`), PIR motion input (`motionsensor` events), light output (`light-on.cgi`)
- Favorites and schedules (`favorites.cgi`, `schedule.cgi`): HTTP(S) notifications and relay actions on ring or motion, e.g. for Home Assistant's DoorBird integration
- Settings page (username, password, relay pulse) stored in flash
- Network firmware updates (ArduinoOTA), watchdog, factory reset button

## Wiring (defaults, change in `src/config.h`)

| Function        | GPIO | Notes                                                    |
|-----------------|------|----------------------------------------------------------|
| Doorbell button | 33   | Button between GPIO33 and GND, internal pull-up          |
| Relay 1         | 32   | Drive a relay module (active high, `RELAY_ACTIVE_HIGH`)  |
| Relay 2         | –    | `RELAY2_PIN`                                             |
| Motion (PIR)    | –    | `MOTION_PIN`                                             |
| Light           | –    | `LIGHT_PIN`                                              |

For a bench test without wiring, set `DOORBELL_PIN 34` and `DOORBELL_PULLUP 0`
to use the on-board BUT1 button.

Don't drive a relay coil from a GPIO directly. Use a relay module with a
transistor driver or an opto-isolated input.

## Configuration

Open `http://bha-<mac>.local/` in a browser and log in with the current
credentials (default `openbird` / `changeme`). There you can change the
username, password and relay pulse length. The settings are kept in flash.
After changing the credentials, re-pair the device in nymea.

**Factory reset:** hold the on-board BUT1 button for 10 seconds. This restores
the compiled-in defaults. The reset is disabled if BUT1 is configured as the
doorbell.

Compile-time defaults and pins can be overridden in `src/config_local.h`
(git-ignored):

```c
#define OPENBIRD_USER     "doorbird"
#define OPENBIRD_PASSWORD "a-long-secret"
#define RELAY_ACTIVE_HIGH 0
#define RELAY_PULSE_MS    2000
```

## Build & flash

```sh
pio run -t upload        # first flash over USB
pio device monitor
```

Keep DTR and RTS deasserted on the USB serial port. The serial default asserts
them, which disturbs the board and drops the Ethernet link until the next
reset. `platformio.ini` sets `monitor_dtr = 0` and `monitor_rts = 0` for
`pio device monitor`. Opening the port this way resets the board once.

Later updates can go over the network. The OTA password is the device
password:

```sh
OPENBIRD_HOST=bha-<mac>.local OPENBIRD_PASSWORD=<password> pio run -e ota -t upload
```

## Discovery

OpenBird announces itself the way the nymea plugin expects:

- mDNS host `bha-<mac>.local` (the plugin filters on the `bha-` prefix)
- service `_axis-video._tcp` on port 80, first TXT record `macaddress=<MAC>`
- service instance name `OpenBird - <MAC>`

The DHCP host name is `openbird-<last 6 MAC digits>`.

## Implemented API

All `/bha-api/*` endpoints require HTTP Basic auth. DoorBird-style
`?http-user=…&http-password=…` or `?sessionid=…` also work. Unauthenticated
requests get a `401` with `WWW-Authenticate: Basic realm="DoorBird"`. The
nymea plugin only sends credentials with `getsession.cgi`. Qt caches them per
host and realm and resends them on later 401s, so the realm must stay
constant.

| Endpoint                       | Behaviour                                                          |
|--------------------------------|--------------------------------------------------------------------|
| `getsession.cgi`               | Returns `SESSIONID` (random per boot), `?invalidate=<id>` drops it |
| `info.cgi`                     | Firmware, MAC, relays, device type                                 |
| `open-door.cgi?r=1\|2`         | Pulses the relay for `RELAY_PULSE_MS`                              |
| `light-on.cgi`                 | Switches the light output on for `LIGHT_ON_MS` (no-op if not fitted)|
| `monitor.cgi?check=doorbell`   | Legacy one-shot state, `doorbell=0\|1`                            |
| `monitor.cgi?ring=doorbell,motionsensor` | `multipart/x-mixed-replace` stream (boundary `--ioboundary`), current state on connect, heartbeat every 30 s. Max 4 clients; a new one replaces the oldest. TCP keepalive drops dead clients |
| `favorites.cgi`                | List, `action=save` (new id in the `favoriteid` header), `action=remove` (also drops schedule outputs using it) |
| `schedule.cgi`                 | List (`?input=` filter, 204 if none), POST JSON entry (replaces same input+param), `action=remove` |
| `restart.cgi`                  | Reboots                                                            |
| `image.cgi`, `video.cgi`, `history.cgi`, audio | `503`, there is no camera or audio                 |

### Schedules

Schedule outputs are evaluated on a doorbell press (`input: doorbell`) or
motion (`input: motion`):

- `http` calls the HTTP favorite with the given id. HTTPS is supported, but
  certificates are not checked (same as DoorBird).
- `relay` pulses the given relay.
- `notify` and `sip` are stored but do nothing.

Time windows (`weekdays`, `from-to`) are evaluated in UTC, using NTP
(`NTP_SERVER`, default `pool.ntp.org`). Until the clock is set,
every window counts as active, so a ring is never lost. `once` windows are not
supported. Favorites and schedules are each limited to about 3.9 KB of JSON
(507 if exceeded). They are cleared by a factory reset.

## Checking a device

`tools/check_device.py` runs the nymea plugin's requests against a device:
the 401 challenge, `getsession`, `info`, the event stream, and a favorite +
schedule round trip. It needs only the Python standard library. `--discover`
uses the `zeroconf` package.

```sh
tools/check_device.py --host bha-<mac>.local --user openbird --password changeme
tools/check_device.py --discover --ring      # find via mDNS, then press the button
tools/check_device.py --host ... --open      # also pulses relay 1
```

`--ring` also checks that the test favorite gets called when the button is
pressed. The test adds its output to an existing doorbell schedule and removes
it again afterwards; other schedules are left alone.

Quick test with curl:

```sh
curl -u openbird:changeme http://bha-<mac>.local/bha-api/info.cgi
curl -N -u openbird:changeme "http://bha-<mac>.local/bha-api/monitor.cgi?ring=doorbell,motionsensor"
curl -u openbird:changeme "http://bha-<mac>.local/bha-api/open-door.cgi?r=1"
```

## nymea

Add a "DoorBird" thing and pick the discovered `OpenBird - <MAC>` entry. Then
enter the configured username and password. "Doorbell pressed" fires on button
press and "Open door" (relay 1/2) switches the relay. With a PIR input fitted,
the presence state follows the sensor.
