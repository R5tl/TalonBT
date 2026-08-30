# Talon — WiFi-enabled original Xbox controller (ESP32-S3)

Talon makes an ESP32-S3 present itself as an **original Xbox "Duke" controller**
(Microsoft `045E:0202`, XID class `0x58/0x42`) on the Xbox controller port,
while joining your WiFi and serving a phone-friendly **web UI** — a controller
you press from a browser. Sibling project of
[Falcon](../Falcon), the Xbox Video Camera emulator, and built on the same
skeleton (ESP-IDF + esp_tinyusb + a custom application class driver).

## How it works

The Duke is an XID device: one interface (class `0x58`, subclass `0x42`) with two
32-byte interrupt endpoints polled every 4 ms — EP2 IN carries the 20-byte input
report, EP2 OUT the 6-byte rumble report. On EP0 the Xbox reads a vendor **XID
descriptor** (`bmRequestType 0xC1, GET_DESCRIPTOR, wValue 0x4200`) and
**capabilities** (`bRequest 0x01`), and uses HID-style `GET_REPORT`/`SET_REPORT`.

- **`xid_descriptors.c`** — byte-faithful device/config descriptors (no strings,
  EP0 size 8, exactly like the real pad) + XID descriptor/capability blobs.
- **`xid_class.c`** — custom TinyUSB class driver: claims the XID interface,
  keeps an input report armed on EP2 IN at all times (refreshed per completion),
  receives rumble on EP2 OUT and EP0.
- **`talon_input.c`** — the shared controller state (spinlock-guarded).
- **`wifi_net.c` / `webui.c` / `index.html`** — WiFi station + HTTP API + web UI.

## Finding it

Talon advertises **mDNS**, so once it's on your network it's at
**http://talon.local/** (and appears as host `talon` in your router's device
list). The web page has a ⚙ link to the WiFi setup page.

## WiFi setup (like Kratos)

Credentials live in NVS. With none stored, Talon starts an open SoftAP
**`Talon-Setup`** with a captive portal — join it from a phone and the setup
page pops up. You can:

- **Scan + join** a network (enter the password), or
- **WPS** — press your router's WPS button, then tap *Start WPS*.

Negotiated/entered credentials are saved and rejoined automatically on boot.
Holding the **BOOT** button (~3 s) forgets WiFi and returns to setup mode.
For a personal build you can also compile credentials in (`main/wifi_creds.h`,
seeded into NVS on first boot).

## Physical controller passthrough

The controller page reads the browser **Gamepad API**: plug a controller into
the phone/PC viewing the page and it drives the Xbox directly — the whole state
is streamed to `/api/state` at ~30 Hz. (This is the local-browser gamepad, not
a controller paired to the ESP32 — see "Bluetooth" in the repo notes.)

## Cerbios IGR

If the console runs the **Cerbios** BIOS, Talon exposes its default In-Game
Reset combos as one-tap shortcuts (collapsible section on the controller page,
or `/api/igr?a=...`): `dash` (LT+RT+Start+Back), `game`, `full`, `cycle`,
`shutdown`, `screen`. Combos and their nibble encoding come from
CerbiosToolInternal (`Config.cs`).

## Web API

Everything is `GET`, so `curl` can drive the pad:

```
/api/press?b=down&ms=120       tap a control (press, hold ms, release)
/api/btn?b=a&v=1               hold (v=1) / release (v=0)
/api/axis?a=lx&v=-32768        analog: lt/rt 0..255, lx/ly/rx/ry -32768..32767
/api/state?v=d,a,b,x,y,bl,wh,lt,rt,lx,ly,rx,ry   whole report at once
/api/release                   release everything
/api/igr?a=dash                Cerbios IGR shortcut
/api/status                    JSON diagnostics (mode, ssid, wps, USB, rumble)
/api/scan                      visible networks (JSON)
/api/wifi/save?ssid=&pass=     save credentials + join
/api/wifi/forget               erase credentials, back to setup AP
/api/wps/start                 WPS push-button join
```

Controls: `up down left right start back ls rs a b x y black white lt rt lx ly rx ry`.

## Build & flash

ESP-IDF ≥ 5.5, ESP32-S3. The **native USB port is the controller**, so
flash/log over the **UART bridge (COM3)**:

```bash
cp main/wifi_creds.h.example main/wifi_creds.h   # fill in SSID/password
idf.py set-target esp32s3
idf.py -p COM3 build flash monitor
```

USB comes up first and WiFi only after enumeration (or an 8 s timeout) —
Falcon showed radio bring-up can disturb the Xbox's timing-strict enumeration.
The dwc2 controller runs in **slave mode** (`CONFIG_TINYUSB_MODE_SLAVE=y`);
the S3's OTG DMA engine corrupts EP0 SETUP under load (proven on Falcon).
