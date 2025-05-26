#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <WebServer.h>
#include <WiFiManager.h>

#define SD_CS_PIN 5
#define SAMPLE_RATE 48000
#define SAMPLE_BUFFER_SIZE 512
#define CHUNK_DURATION_MS 30000
#define LED_PIN 2

// FreeRTOS task stack sizes
#define RECORD_STACK_SIZE 8192
#define UPLOAD_STACK_SIZE 8192

// Recording state
bool isIntroRecording = false;
bool isRecording = false;
bool stopRequested = false;

// Intro recording variables
File introFile;
uint32_t introBytesWritten = 0;
int introCount = 1;
String currentIntroFilePath;

// Meeting recording variables
File audioFile;
uint32_t totalBytesWritten = 0;
int meetingCount = 1;
String currentFilePath;
unsigned long chunkStartTime = 0;

// WAV header structure
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
} header;

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

// I2S configuration
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
static const i2s_pin_config_t i2s_pins = {
  .bck_io_num = GPIO_NUM_14,
  .ws_io_num = GPIO_NUM_15,
  .data_out_num = I2S_PIN_NO_CHANGE,
  .data_in_num = GPIO_NUM_32
};

// Buffer for samples
int16_t raw_samples[SAMPLE_BUFFER_SIZE];

WebServer server(80);

// Task: continuous I2S reading and chunk handling
void recordTask(void *pvParameters) {
  size_t bytes_read;
  while (true) {
    if (isRecording) {
      i2s_read(I2S_NUM_0, raw_samples, sizeof(raw_samples), &bytes_read, portMAX_DELAY);
      if (bytes_read > 0) {
        audioFile.write((uint8_t *)raw_samples, bytes_read);
        totalBytesWritten += bytes_read;
      }
      if (millis() - chunkStartTime >= CHUNK_DURATION_MS) {
        stopRecording();
        if (!stopRequested) startRecording();
        else stopRequested = false;
      }
    }
    if (isIntroRecording) {
      i2s_read(I2S_NUM_0, raw_samples, sizeof(raw_samples), &bytes_read, portMAX_DELAY);
      if (bytes_read > 0) {
        introFile.write((uint8_t *)raw_samples, bytes_read);
        introBytesWritten += bytes_read;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// Task: upload WAV via presigned URL
void uploadTask(void *parameter) {
  String filePath = *((String *)parameter);
  delete (String *)parameter;

  String filename = filePath;
  if (filename.startsWith("/")) filename.remove(0, 1);
  String requestURL = String("https://3125ccb2-54a5-4d27-9aef-532ef1bdaccc-00-226tm68i8l9bh.sisko.replit.dev/api/v1/presigned-url?filename=") + filename;

  HTTPClient http;
  http.begin(requestURL);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, payload) == DeserializationError::Ok && doc.containsKey("url")) {
      String presignedUrl = doc["url"].as<String>();
      File file = SD.open("/" + filename, FILE_READ);
      if (file) {
        HTTPClient putClient;
        putClient.begin(presignedUrl);
        putClient.addHeader("Content-Type", "audio/wav");
        int code = putClient.sendRequest("PUT", (Stream *)&file, file.size());
        Serial.printf("\n📤 Uploaded %s -> HTTP %d\n", filename.c_str(), code);
        putClient.end();
        file.close();
      } else {
        Serial.printf("\n❌ Couldn't open %s for upload\n", filename.c_str());
      }
    } else {
      Serial.println("\n❌ Invalid JSON in presigned response");
    }
  } else {
    Serial.printf("\n❌ Presigned URL request failed: HTTP %d\n", httpCode);
  }
  http.end();
  vTaskDelete(NULL);
}

void connectWiFi() {
  WiFiManager wm;
  wm.resetSettings();
  wm.setTimeout(180);
  if (!wm.startConfigPortal("Textify")) ESP.restart();
  digitalWrite(LED_PIN, HIGH);
}

void startRecording() {
  currentFilePath = "/meetings/meeting" + String(meetingCount) + ".wav";
  audioFile = SD.open(currentFilePath, FILE_WRITE);
  if (!audioFile) {
    Serial.println("❌ Failed to open meeting file");
    return;
  }
  writeWAVHeaderPlaceholder(audioFile);
  totalBytesWritten = 0;
  chunkStartTime = millis();
  isRecording = true;
  Serial.print("🎙 Meeting recording started: ");
  Serial.println(currentFilePath);
}

void stopRecording() {
  if (!isRecording) return;
  updateWAVHeader(audioFile, totalBytesWritten);
  audioFile.close();
  isRecording = false;
  Serial.println("🛑 Meeting recording stopped.");

  // Schedule upload
  String *p = new String(currentFilePath);
  xTaskCreatePinnedToCore(uploadTask, "UploadTask", UPLOAD_STACK_SIZE, p, 1, NULL, 0);
  meetingCount++;
}

void startIntroRecording() {
  currentIntroFilePath = "/intro/intro" + String(introCount) + ".wav";
  introFile = SD.open(currentIntroFilePath, FILE_WRITE);
  if (!introFile) {
    Serial.println("❌ Failed to open intro file");
    return;
  }
  writeWAVHeaderPlaceholder(introFile);
  introBytesWritten = 0;
  isIntroRecording = true;
  Serial.print("🎙 Intro recording started: ");
  Serial.println(currentIntroFilePath);
}

void stopIntroRecording() {
  if (!isIntroRecording) return;
  updateWAVHeader(introFile, introBytesWritten);
  introFile.close();
  isIntroRecording = false;
  Serial.println("🛑 Intro recording stopped.");

  String *p = new String(currentIntroFilePath);
  xTaskCreatePinnedToCore(uploadTask, "UploadIntro", UPLOAD_STACK_SIZE, p, 1, NULL, 0);
  introCount++;
}

void initServer() {
  server.on("/start", HTTP_GET, []() {
    if (!isRecording) {
      stopRequested = false;
      startRecording();
      server.send(200, "application/json", "{\"status\":\"started\"}");
    } else {
      server.send(200, "application/json", "{\"status\":\"busy\"}");
    }
  });
  server.on("/stop", HTTP_GET, []() {
    if (isRecording) {
      stopRequested = true;
      server.send(200, "application/json", "{\"status\":\"stopping\"}");
    } else {
      server.send(200, "application/json", "{\"status\":\"idle\"}");
    }
  });
  server.on("/start-intro", HTTP_GET, []() {
    if (!isIntroRecording) {
      startIntroRecording();
      server.send(200, "application/json", "{\"status\":\"intro_started\"}");
    } else {
      server.send(200, "application/json", "{\"status\":\"intro_busy\"}");
    }
  });
  server.on("/stop-intro", HTTP_GET, []() {
    if (isIntroRecording) {
      stopIntroRecording();
      server.send(200, "application/json", "{\"status\":\"intro_stopped\"}");
    } else {
      server.send(200, "application/json", "{\"status\":\"no_intro\"}");
    }
  });
  server.on("/status", HTTP_GET, []() {
    String st = isRecording ? "recording" : "stopped";
    String js = "{\"status\":\"" + st + "\",\"count\":" + String(meetingCount) + "}";
    server.send(200, "application/json", js);
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("❌ SD card initialization failed!");
    while (true) vTaskDelay(portMAX_DELAY);
  }
  if (!SD.exists("/intro")) SD.mkdir("/intro");
  if (!SD.exists("/meetings")) SD.mkdir("/meetings");

  connectWiFi();

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &i2s_pins);

  initServer();

  xTaskCreatePinnedToCore(recordTask, "RecordTask", RECORD_STACK_SIZE, NULL, 2, NULL, 1);
}

void loop() {
  server.handleClient();

  // Serial commands to start/stop recording
  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 's') {
      if (!isRecording) {
        stopRequested = false;
        startRecording();
        Serial.println("✅ Recording started via Serial.");
      } else {
        Serial.println("⚠️ Already recording.");
      }
    } else if (cmd == 'p') {
      if (isRecording) {
        stopRequested = true;
        Serial.println("🛑 Stop requested via Serial.");
      } else {
        Serial.println("⚠️ Not currently recording.");
      }
    }
  }

  digitalWrite(LED_PIN, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
  delay(1);
}
