# esp-time-advertisement

An ESP32 (built and tested with ESP-IDF v6.2) sends beacons whose sole purpose
is to carry the 802.11v **Time Advertisement** (element 69) and **Time Zone**
(element 98) elements. The time is fetched via NTP over an uplink Wi-Fi network.

## Motivation

On off-the-shelf Wi-Fi access points the 802.11v Time Advertisement extension
usually cannot simply be enabled. It is only available where the AP's
configuration is fully accessible, e.g. OpenWrt with hostapd
(`time_advertisement=2`, `time_zone=...`). This project is a workaround that
broadcasts Time Advertisement anyway, independent of the existing
infrastructure.

It is used in the [wlanscout+](https://github.com/Oposum01/wlanscout) project: when the MagSafe sensor is booted from a
power bank only, it determines its time from these beacons.

## Hardware

Built and tested on an AZ-Delivery ESP32 Dev Kit C V4:

- ESP32-WROOM-32 module (ESP32 chip revision v3.1, 4 MB flash)
- CP2102 USB-UART bridge, powered and flashed via Micro USB
- BOOT button (GPIO0) used to step the TX power, see below
- No pin headers needed, nothing else is connected

Other boards with a classic ESP32 should work as well; if the BOOT button is
not on GPIO0, adjust `BUTTON_GPIO` in `main/txpower.c`. Other ESP32 variants
(S2, S3, C3, ...) have not been tested. A matching case is in `3d-print/`, see
[Enclosure](#enclosure).

## Operation

1. Connect to an uplink as a station (e.g. `vogtland.freifunk.net`, open; with
   several configured, the next one is tried if one fails),
   DHCP, SNTP. An NTP server announced via DHCP (option 42) is tried first.
2. Release the DHCP lease (DHCPRELEASE; ESP-IDF itself does not do this on
   disconnect), disconnect, pick the beacon channel (fixed or by scan) and send
   a beacon via `esp_wifi_80211_tx()` every 1000 TU (1.024 s).
3. After `NTP_SYNC_INTERVAL_MIN` (default 6 h) stop the beacons, resync, resume.
   If a resync fails, the beacons continue with the free-running clock. Before
   the first successful sync no beacons are sent; the sync is retried every 120 s.

No SoftAP is running. The beacons are raw frames from the driver and nobody
answers authentication or association requests, so no client can join.

## Configuration

Uplink and NTP are configured in `main/config.h` (not versioned, see
`.gitignore`). Start from the template:

```sh
cp main/config.h.example main/config.h
```

| Define | Default |
|---|---|
| `UPLINK_NETWORKS` | `{ "vogtland.freifunk.net", "" }`: list of `{ SSID, password }`, tried in order until a sync succeeds; empty password ⇒ open network, otherwise WPA2-PSK (or better) |
| `DHCP_HOSTNAME` | `ESP32-Time-Advertisement` |
| `NTP_SERVER_1` / `NTP_SERVER_2` | `pool.ntp.org` / `ptbtime1.ptb.de` (2 empty ⇒ only one) |
| `NTP_SERVER_FROM_DHCP` | `1` ⇒ NTP server from DHCP option 42 first, the two above as fallback |
| `NTP_SYNC_INTERVAL_MIN` | `6 * 60` (6 h) |
| `PROBE_RESPONSES` | `0` (off), `1` ⇒ probe requests are answered |
| `SSID_NETWORKNAME` | `ESP32-Time-Advertisement` |
| `BEACON_TIMEZONE` | `CET-1CEST,M3.5.0,M10.5.0/3` (POSIX TZ, incl. DST rule) |
| `WIFI_COUNTRY` | `DE` (regulatory domain, usable channels) |
| `RADIO_CHANNEL` | `0` ⇒ automatic, `1`–`13` ⇒ fixed |
| `BEACON_INTERVAL_TU` | `1000` (15–65535) |
| `TX_POWER_DBM` | `20` (2–20) |

POSIX TZ strings for other zones (by IANA name, e.g. `Europe/Berlin`) are
listed in [posix_tz_db](https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv);
the format is described in the
[glibc manual](https://sourceware.org/glibc/manual/latest/html_node/TZ-Variable.html).

Only the timeouts are set via `idf.py menuconfig` → *Time Advertisement*:
connect + DHCP 20 s, NTP 50 s, retry after a failed sync 120 s.

The beacon interval can be set from 15 to 65535 TU (16-bit field, max. ≈ 67 s).
Note that 1 TU = 1024 µs, so 1000 TU ≈ 0.98 beacons per second. The default
of 1000 TU instead of the usual 100 TU is deliberate: every beacon occupies
airtime on the channel (≈ 0.3 ms at 6 Mbit/s), so the longer interval keeps
the load on the channel low. Receivers only need an occasional beacon to get
the time.

The NTP timeout covers all servers: each one gets 15 s before the next is
tried.

### Channel selection (`RADIO_CHANNEL 0`)

After every NTP sync the ESP scans all channels allowed by `WIFI_COUNTRY` and
counts the BSSIDs per primary channel. Of channels 1, 6 and 11 the one
with the fewest BSSIDs is used; on a tie the order 1, 6, 11 decides. The
current channel is kept unless another one has strictly fewer BSSIDs. Overlap
with neighbouring channels is not taken into account.

### TX power (`TX_POWER_DBM`)

A short press on the BOOT button (GPIO0) lowers the maximum TX power by 1 dB,
from 2 dBm back to 20 dBm. Holding it for 3 s resets it to `TX_POWER_DBM`.
The value is stored in NVS and survives reboots; changing
`TX_POWER_DBM` in `config.h` overrides it. The ESP32 only supports a few power
levels, so not every step changes the output. The log shows the requested
value and the value the driver reports.

## Build / flash

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py build flash monitor
```

## Beacon contents

Sent as 11g only (OFDM, 6 Mbit/s), no 802.11b.

- Timestamp: 0. The hardware zeroes this field in raw frames whose BSSID is
  not the interface's own MAC (it inserts its TSF only for its own MAC).
- Hence the Time Value (UTC at TSF 0) is simply the current UTC time and is
  set freshly in every beacon.
- SA/BSSID: locally administered address derived from the STA MAC
  (`esp_derive_local_mac`), so frames addressed to the time beacon do not hit
  the STA interface.
- Capabilities: ESS, Privacy, Short Slot Time
- RSN: WPA2-PSK/CCMP, advertised only (no key exists, since nobody answers
  authentication)
- SSID, Supported Rates (6(B) 9 12(B) 18 24(B) 36 48 54), DS Parameter Set, TIM,
  ERP Information
- Extended Capabilities: bit 27 (UTC TSF Offset)
- Time Advertisement: Timing Capabilities = 2, Time Value, Time Error,
  Time Update Counter (+1 per successful NTP sync, starts at 0 with the first
  sync after reboot)
- Time Error (IEEE 802.11-2024 9.4.2.59: standard deviation of the error in
  the Time Value, in ms): 50 ms right after a sync plus, for the time since
  then, 20 ppm while the clock drift is unknown (≈ 480 ms after 6 h) or 5 ppm
  once it is measured and compensated (≈ 160 ms after 6 h). This is a
  conservative estimate, not a measurement.

## Clock drift

The ESP32 crystal deviates by some ppm, i.e. up to a few hundred ms over the
6 h between syncs. At every NTP sync the firmware compares the NTP time with
the time it would have advertised and logs the difference
(`clock: advertised time was off by …`). If at least 30 min passed since the
previous sync, the drift is derived from it, averaged with the previous value
and stored in NVS, so it is known right after a reboot. The advertised time
is then the last NTP time plus the elapsed monotonic time, corrected by this
drift. Values above 100 ppm are discarded as bad NTP answers.
- Time Zone: POSIX TZ string

## Probe responses (`PROBE_RESPONSES`)

Off by default: beacons only. Phones and laptops scan actively with short
dwell times and therefore practically never find the network; passive scanners
(monitor mode) do see it.

With `1`, promiscuous mode (management frames only) is active during the
beacon phase. Probe requests with the wildcard SSID or our own SSID and a
broadcast or our own BSSID are answered with a probe response. It carries the
same elements as the beacon except TIM, including Time Advertisement and Time
Zone.

## Enclosure

`3d-print/` contains a printable case (base and lid) for the ESP32 DevKit v4,
derived from "Slim case for ESP32 Devkit V4"
(https://www.thingiverse.com/thing:6542481) by scross01, licensed under the
Creative Commons Attribution license (CC BY). The files in `3d-print/` are
licensed under CC BY as well, not under the GPL of the firmware.

Changes to the original:

- Text added on both parts (base and lid)
- Cut-outs for the pin headers closed, since the board used here has no pin
  headers soldered on

## Licence

**GNU General Public License v2.0 only** (GPL-2.0-only). The full text is
in [`LICENSE`](LICENSE). Every source file carries the licence as an
[SPDX](https://spdx.org/licenses/GPL-2.0-only.html) tag instead of the
long notice.

Copyright (C) 2026 Dr.-Ing. Simon Wunderlich Systementwicklung &
Beratung (<https://www.simonwunderlich.de/>). Written by Jan Fuchs
<jf [at] simonwunderlich . de> as work for Simon Wunderlich. Source:
<https://github.com/Oposum01/esp32-802.11v-time-advertisement>.

**Except the case.** The files in `3d-print/` are derived from a CC BY
design and licensed under CC BY, not the GPL, see [Enclosure](#enclosure).
