#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <U8g2lib.h>

// Create display object for SH1106 I2C (ESP32)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// I2S and SD Configurations
#define SD_CS_PIN 5
#define SAMPLE_RATE 48000
#define SAMPLE_BUFFER_SIZE 512
#define CHUNK_DURATION_MS 30000
#define LED_PIN 2

// Global Variables
bool isIntroRecording = false;
File introFile;
uint32_t introBytesWritten = 0;
unsigned long introStartTime = 0;
int introCount = 1;
String currentIntroFilePath = "";

File audioFile;
int16_t raw_samples[SAMPLE_BUFFER_SIZE];
uint32_t totalBytesWritten = 0;
int meetingCount = 1;
bool isRecording = false;
bool stopRequested = false;
unsigned long chunkStartTime = 0;
String currentFilePath = "";

// Upload status for display
String lastUploadStatus = "N/A";

// OLED display buffer
#define MAX_LINES 5
String displayBuffer[MAX_LINES];
int lineCount = 0;

struct WAVHeader {
  char riff[4];
  uint32_t fileSize;
  char wave[4];
  char fmtChunkMarker[4];
  uint32_t fmtChunkSize;
  uint16_t audioFormat;
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char dataChunkMarker[4];
  uint32_t dataChunkSize;
};

WAVHeader header;

static const i2s_config_t i2s_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
  .sample_rate = SAMPLE_RATE,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 4,
  .dma_buf_len = 1024,
  .use_apll = false,
  .tx_desc_auto_clear = false,
  .fixed_mclk = 0
};

static const i2s_pin_config_t i2s_mic_pins = {
  .bck_io_num = GPIO_NUM_14,
  .ws_io_num = GPIO_NUM_15,
  .data_out_num = I2S_PIN_NO_CHANGE,
  .data_in_num = GPIO_NUM_32
};

WebServer server(80);

// Custom print function to mirror to OLED
void customPrint(const String &message) {
  Serial.println(message); // Print to Serial Monitor

  // Add message to display buffer
  if (lineCount < MAX_LINES) {
    displayBuffer[lineCount] = message;
    lineCount++;
  } else {
    // Shift lines up and add new message at the bottom
    for (int i = 0; i < MAX_LINES - 1; i++) {
      displayBuffer[i] = displayBuffer[i + 1];
    }
    displayBuffer[MAX_LINES - 1] = message;
  }

  // Update OLED display
  u8g2.clearBuffer();
  for (int i = 0; i < lineCount && i < MAX_LINES; i++) {
    u8g2.drawStr(0, i * 12, displayBuffer[i].c_str()); // 12 pixels per line
  }
  u8g2.sendBuffer();
}

// ===== Upload Task Function =====
void uploadTask(void* parameter) {
  String filePath = *((String*)parameter);
  delete (String*)parameter;

  String filename = filePath;
  if (filename.startsWith("/")) filename = filename.substring(1);
  String requestURL = "http://192.168.1.119:3000/api/v1/presigned-url?filename=" + filename;

  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(requestURL)) {
    customPrint("❌ Failed to initialize HTTP connection for presigned URL.");
    lastUploadStatus = "HTTP Error";
    vTaskDelete(NULL);
    return;
  }

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error == DeserializationError::Ok && doc.containsKey("url")) {
      String presignedUrl = doc["url"].as<String>();
      if (presignedUrl.length() == 0) {
        customPrint("❌ Empty presigned URL");
        lastUploadStatus = "HTTP Error";
        http.end();
        vTaskDelete(NULL);
        return;
      }

      File file = SD.open(filePath);
      if (file) {
        HTTPClient putClient;
        putClient.setTimeout(15000);
        if (!putClient.begin(presignedUrl)) {
          customPrint("❌ Failed to initialize HTTP connection for upload.");
          lastUploadStatus = "HTTP Error";
          file.close();
          http.end();
          vTaskDelete(NULL);
          return;
        }

        putClient.addHeader("Content-Type", "audio/wav");
        int code = putClient.sendRequest("PUT", &file, file.size());
        customPrint("📤 Upload " + filename + " -> HTTP " + String(code));
        if (code > 0 && (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED)) {
          lastUploadStatus = "Upload Successfully";
        } else {
          lastUploadStatus = "HTTP Error";
        }
        putClient.end();
        file.close();
      } else {
        customPrint("❌ Failed to open file for upload.");
        lastUploadStatus = "HTTP Error";
      }
    } else {
      customPrint("❌ JSON parse failed or 'url' missing.");
      lastUploadStatus = "HTTP Error";
    }
  } else {
    customPrint("❌ Failed to get presigned URL. HTTP " + String(httpCode));
    lastUploadStatus = "HTTP Error";
  }
  http.end();
  vTaskDelete(NULL);
}

// ===== Wi-Fi Setup =====
void connectWiFi() {
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  wifiManager.setTimeout(180);
  delay(1000);

  customPrint("Starting WiFi configuration AP...");
  if (!wifiManager.startConfigPortal("Textify")) {
    customPrint("Failed to start configuration portal and hit timeout");
    ESP.restart();
  }
  digitalWrite(LED_PIN, HIGH);
  customPrint("Connected to WiFi!");
  customPrint("IP Address: " + WiFi.localIP().toString());
}

// ===== WAV File Header Functions =====
void writeWAVHeaderPlaceholder(File &file) {
  memcpy(header.riff, "RIFF", 4);
  header.fileSize = 0;
  memcpy(header.wave, "WAVE", 4);
  memcpy(header.fmtChunkMarker, "fmt ", 4);
  header.fmtChunkSize = 16;
  header.audioFormat = 1;
  header.numChannels = 1;
  header.sampleRate = SAMPLE_RATE;
  header.bitsPerSample = 16;
  header.byteRate = SAMPLE_RATE * 2;
  header.blockAlign = 2;
  memcpy(header.dataChunkMarker, "data", 4);
  header.dataChunkSize = 0;

  file.write((uint8_t *)&header, sizeof(header));
}

void updateWAVHeader(File &file, uint32_t totalAudioBytes) {
  uint32_t fileSize = 36 + totalAudioBytes;
  file.seek(4);
  file.write((uint8_t *)&fileSize, sizeof(fileSize));
  file.seek(40);
  file.write((uint8_t *)&totalAudioBytes, sizeof(totalAudioBytes));
  file.seek(0, SeekEnd);
}

// ===== Intro Recording Functions =====
void startIntroRecording() {
  currentIntroFilePath = "/intro/intro" + String(introCount) + ".wav";
  introFile = SD.open(currentIntroFilePath, FILE_WRITE);
  if (!introFile) {
    customPrint("❌ Failed to open intro file for recording");
    return;
  }
  writeWAVHeaderPlaceholder(introFile);
  introBytesWritten = 0;
  introStartTime = millis();
  isIntroRecording = true;
  customPrint("🎙 Intro recording started: " + currentIntroFilePath);
}

// ===== Meeting Recording Functions =====
void startRecording() {
  currentFilePath = "/meetings/meeting" + String(meetingCount) + ".wav";
  audioFile = SD.open(currentFilePath, FILE_WRITE);
  if (!audioFile) {
    customPrint("❌ Failed to open file for recording");
    return;
  }
  writeWAVHeaderPlaceholder(audioFile);
  totalBytesWritten = 0;
  chunkStartTime = millis();
  isRecording = true;
  customPrint("🎙 Recording started: " + currentFilePath);
}

void stopRecording() {
  if (!isRecording) {
    customPrint("❌ Not recording");
    return;
  }

  updateWAVHeader(audioFile, totalBytesWritten);
  audioFile.close();
  isRecording = false;
  customPrint("📁 Recording stopped");

  String* path = new String(currentFilePath);
  xTaskCreatePinnedToCore(uploadTask, "UploadTask", 10000, path, 1, NULL, 0);
  meetingCount++;
  
  if (!stopRequested) {
    startRecording();
  } else {
    stopRequested = false;
    customPrint("🛑 Recording stopped after final chunk.");
  }
}

void stopIntroRecording() {
  if (!isIntroRecording) {
    customPrint("❌ No intro recording in progress");
    return;
  }

  updateWAVHeader(introFile, introBytesWritten);
  introFile.close();
  isIntroRecording = false;
  customPrint("📁 Intro recording stopped");

  String* path = new String(currentIntroFilePath);
  xTaskCreatePinnedToCore(uploadTask, "IntroUploadTask", 10000, path, 1, NULL, 0);
  introCount++;
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Initialize display first
  Wire.begin(21, 22); // Explicitly initialize I2C with SDA and SCL pins
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.setContrast(128);

  customPrint("Initializing...");

  if (!SD.begin(SD_CS_PIN)) {
    customPrint("❌ SD card initialization failed!");
    while (true) delay(100);
  }
  customPrint("✅ SD card initialized");

  if (!SD.exists("/intro")) SD.mkdir("/intro");
  if (!SD.exists("/meetings")) SD.mkdir("/meetings");

  connectWiFi();

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &i2s_mic_pins);
  customPrint("✅ I2S initialized");

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain", "ESP32 Audio Recorder API");
  });

  server.on("/start", HTTP_GET, []() {
    if (!isRecording) {
      stopRequested = false;
      startRecording();
      server.send(200, "application/json", "{\"status\":\"started\"}");
    } else {
      server.send(200, "application/json", "{\"status\":\"already_recording\"}");
    }
  });

  server.on("/stop", HTTP_GET, []() {
    if (isRecording) {
      stopRequested = true;
      server.send(200, "application/json", "{\"status\":\"stopping\"}");
    } else {
      server.send(200, "application/json", "{\"status\":\"not_recording\"}");
    }
  });

  server.on("/status", HTTP_GET, []() {
    String status = isRecording ? "recording" : (isIntroRecording ? "intro recording" : "stopped");
    String wifiStatus = WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected";
    String sdStatus = SD.begin(SD_CS_PIN) ? "Initialized" : "Not Initialized";
    String response = "<h1>ESP32 Audio Recorder Status</h1>"
                     "<p>Wi-Fi Status: " + wifiStatus + " (IP: " + WiFi.localIP().toString() + ")</p>"
                     "<p>SD Card Status: " + sdStatus + "</p>"
                     "<p>Recording Status: " + status + "</p>"
                     "<p>Meeting Count: " + String(meetingCount) + "</p>"
                     "<p>Last Upload Status: " + lastUploadStatus + "</p>";
    server.send(200, "text/html", response);
  });

  server.on("/start-intro", HTTP_GET, []() {
    if (!isIntroRecording) {
      startIntroRecording();
      server.send(200, "application/json", "{\"status\":\"intro_started\"}");
    } else {
      server.send(200, "application/json", "{\"status\":\"intro_already_recording\"}");
    }
  });

  server.on("/stop-intro", HTTP_GET, []() {
    if (isIntroRecording) {
      stopIntroRecording();
      server.send(200, "application/json", "{\"status\":\"intro_stopped\"}");
    } else {
      server.send(200, "application/json", "{\"status\":\"no_intro_recording\"}");
    }
  });

  server.begin();
  customPrint("HTTP server started");
}

// ===== Loop =====
void loop() {
  server.handleClient();

  // Handle serial commands
  if (Serial.available()) {
    char cmd = Serial.read();
    switch (cmd) {
      case 's':
        if (!isRecording) {
          startRecording();
        }
        break;
      case 'p':
        if (isRecording) {
          stopRecording();
        }
        break;
      case 'r':
        meetingCount = 1;
        customPrint("✅ Meeting count reset to 1");
        break;
    }
  }

  if (isRecording) {
    size_t bytes_read = 0;
    i2s_read(I2S_NUM_0, raw_samples, sizeof(raw_samples), &bytes_read, portMAX_DELAY);
    if (bytes_read > 0) {
      audioFile.write((uint8_t *)raw_samples, bytes_read);
      totalBytesWritten += bytes_read;
    }

    if (millis() - chunkStartTime >= CHUNK_DURATION_MS) {
      stopRecording();
    }
  }

  if (isIntroRecording) {
    size_t bytes_read = 0;
    i2s_read(I2S_NUM_0, raw_samples, sizeof(raw_samples), &bytes_read, portMAX_DELAY);
    if (bytes_read > 0) {
      introFile.write((uint8_t *)raw_samples, bytes_read);
      introBytesWritten += bytes_read;
    }
  }

  digitalWrite(LED_PIN, WiFi.status() == WL_CONNECTED ? HIGH : LOW);

  delay(1);
}