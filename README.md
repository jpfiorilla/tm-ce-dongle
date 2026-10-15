# tm-ce-dongle

Firmware for the LilyGO T-Dongle-S3 that turns it into a USB drive for Nintendont, built so it can
later sync the Training Mode CE save (`saves/GTME.raw`) over Wi-Fi before the Wii sees the drive.

## Milestone 1 (this commit): plain USB drive
The dongle's microSD is presented to the host as a USB mass-storage drive. No Wi-Fi yet. The goal is
to find out whether TM-CE plays acceptably from it through regular Nintendont, since the ESP32-S3 is USB 1.1 (full-speed) only.

Build: `arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=4M" --export-binaries --output-dir out .`
(ESP32 Arduino core 3.3.x). Flash `out/tmce_dongle.ino.merged.bin` at offset 0x0.

Based on LilyGO's `examples/usb_mass_storage` (MIT).
