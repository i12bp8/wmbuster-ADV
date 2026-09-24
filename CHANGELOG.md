# Changelog

A release is made by pushing a tag `vX.Y.Z` whose number matches `WMB_VERSION` in
`src/version.h`, or by running the Build workflow on GitHub with that tag. GitHub
Actions then builds and tests the firmware and publishes the release with the section
below for that version. Tags with a suffix, such as `v2.1.0-beta.1`, are published as
pre-releases.

## 2.0.0

A complete rewrite: better reception, wmbusmeters-grade decoding, a new interface and
many new features.

### Reception
* **SX1262 (Cap LoRa-1262):** the antenna switch is now actually turned on. Earlier
  builds configured the IO expander wrong. Boosted RX gain, and the receiver drops
  false syncs quickly and stops right after each frame.
* **CC1101 (Hydra RF):** frames are no longer cut off at 61 bytes, and frames of any
  length are received.
* **Modes:** T1, C1 (frame A and B) and S1, with optional hopping between 868.95 and
  868.30 MHz.
* **Radio task:** the radio runs on its own, so the screen, Wi-Fi and SD card no longer
  make it miss telegrams.

### Decoding
* All 122 wmbusmeters drivers, decoded exactly like wmbusmeters: all 385 of its test
  telegrams give the same fields and values.
* Encryption: AES modes 5, 7 and 10, ELL, DES, Diehl IZAR/PRIOS and Qundis walk-by.
* Meters without a driver still get a basic decode.

### Device
* Meter list with values, signal and age; a live feed; statistics and settings.
* **Hunt** mode with a signal gauge, history graph and proximity beeps for finding a
  meter.
* Meter keys and names typed on the keyboard, starred meters, a raw telegram view.
* A QR code to connect your phone, and four themes.

### Integrations
* Web UI on the device's own access point or your network: live feed, meters, full
  decodes, telegram analyzer, key import, settings, log downloads, GeoJSON export and
  firmware updates over Wi-Fi.
* MQTT with login, wmbusmeters-compatible JSON and Home Assistant discovery.
* ntfy notifications for starred meters, over HTTPS with certificate checks.
* USB serial output as log lines, JSON or `rtl_wmbus` lines for wmbusmeters on a PC,
  plus a command line.
* SD card logs (raw telegrams, decoded JSON, wardriving CSV) and key import from
  `/keys.txt`.
* GNSS positions and clock (Cap LoRa-1262).

### Upgrading from 1.x
* The flash layout changed to make room for updates over Wi-Fi. The first install must
  be a full flash of `wmbuster-adv-full.bin` at offset 0x0.
* Settings and meter keys saved by 1.x are not carried over. Enter the keys again, or
  put them in `/keys.txt` on the SD card.

## 1.0.0

Initial release with zero-touch hardware auto-detection (LoRa Cap + Hydra RF), automated
3-pass heuristic wM-Bus decoding, and hot-swap UI.
