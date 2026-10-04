# esp32cam-gdrive

Upload a photo from an ESP32 camera board to Google Drive on a timer.
Based on [gsampallo/esp32cam-gdrive](https://github.com/gsampallo/esp32cam-gdrive).

The board wakes from deep sleep every `UPLOAD_INTERVAL_SEC` seconds, takes a photo,
uploads it, and goes back to sleep.

## Setup

1. **Arduino IDE board support**: File > Preferences > *Additional boards manager URLs*, add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
   (the old `raw.githubusercontent.com/.../gh-pages/...` link is dead). Then Tools > Board >
   Boards Manager, install **esp32 by Espressif Systems**.
2. **Google script**: create a new project at script.google.com, paste in `upload.gs`,
   Deploy > New deployment > Web app, *Execute as: Me*, *Who has access: Anyone*.
   Authorize it, then copy the web-app URL. The ID is the part between `/macros/s/` and `/exec`.
3. **Credentials**: copy `secrets.h.example` to `secrets.h` and fill in your WiFi and script ID.
   `secrets.h` is git-ignored.
4. **Board**: in `esp32cam-gdrive.ino`, uncomment the `CAMERA_MODEL_...` line that matches your board.
5. **Arduino IDE settings**: pick the matching board (e.g. "AI Thinker ESP32-CAM"; for S3 boards
   enable PSRAM "OPI PSRAM" or as listed for your board), then upload.
   AI-Thinker boards have no USB chip: wire GPIO0 to GND while flashing, then unplug it and reset.
