# Textify-Device
# ESP32 I2S Audio Recorder

**Description**
Reads audio from an I2S microphone, writes to WAV-formatted files on an SD card, chunks recordings, and uploads via presigned S3 URLs.

**Features**
- Intro and meeting recording modes
- Automatic chunking every 30 seconds
- HTTP API to start/stop recordings
- Wi-Fi credential portal via WiFiManager

**Hardware**
- ESP32
- I2S microphone (e.g., INMP441)
- SD card module
- LED on GPIO 2 for status
- Power supply
- OLED display

**Software Structure**
Refer to the `include/` directory for module headers and `src/main.ino` for the implementation.

**Usage**
1. Power on the ESP32 via supply for not dependent on any system; connect to the `ESP32_Config` network to configure Wi-Fi.
2. Use HTTP GET requests to `/start`, `/stop`, `/start-intro`, and `/stop-intro`.
3. WAV files are saved under `/meetings/` and `/intro/` on the SD card.
4. OLED display shows the status
