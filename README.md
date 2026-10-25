# OpenBird

Door station firmware for the **Olimex ESP32-PoE-ISO**, compatible with the
DoorBird LAN API. It works with the existing nymea DoorBird plugin, including
discovery and doorbell events.

- Ethernet (LAN8720) with DHCP, powered via PoE
- Doorbell button input → `doorbell:H` / `doorbell:L` events on `monitor.cgi`
- Door opener relay output → `open-door.cgi?r=1`
- Optional: second relay (`r=2`), PIR motion input (`motionsensor` events), light output (`light-on.cgi`)

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

Create `src/config_local.h` (git-ignored) to override the defaults:

```c
#define OPENBIRD_USER     "doorbird"
#define OPENBIRD_PASSWORD "a-long-secret"
#define RELAY_ACTIVE_HIGH 0
#define RELAY_PULSE_MS    2000
```

## Build & flash

```sh
pio run -t upload
pio device monitor
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
requests get a `401` with `WWW-Authenticate: Basic realm="DoorBird"`.

| Endpoint                       | Behaviour                                                          |
|--------------------------------|--------------------------------------------------------------------|
| `getsession.cgi`               | Returns `SESSIONID` (random per boot)                              |
| `info.cgi`                     | Firmware, MAC, relays, device type                                 |
| `open-door.cgi?r=1\|2`         | Pulses the relay for `RELAY_PULSE_MS`                              |
| `light-on.cgi`                 | Switches the light output on for `LIGHT_ON_MS` (no-op if not fitted)|
| `monitor.cgi?ring=doorbell,motionsensor` | `multipart/x-mixed-replace` stream (boundary `--ioboundary`), current state on connect, heartbeat every 30 s, max 4 clients |
| `favorites.cgi`, `schedule.cgi`| Return empty lists; saves are accepted but not stored              |
| `restart.cgi`                  | Reboots                                                            |
| `image.cgi`, `video.cgi`, `history.cgi`, audio | `503`, there is no camera or audio                 |

Quick test:

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
