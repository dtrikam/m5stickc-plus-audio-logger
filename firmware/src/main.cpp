#include <M5StickCPlus.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <driver/i2s.h>
#include <math.h>

// ─── Cloud & Wi-Fi Configuration (Stored in ESP32 NVS — Zero Hardcoded Secrets) ─
// All credentials & endpoints live in ESP32 NVS flash ("wifi" & "cloud" namespaces).
// Provision over USB Serial (`configure_wifi.py`) or via SoftAP Portal (`192.168.4.1`).
const uint16_t DEFAULT_SERVER_PORT = 8765;

// ─── M5StickC Plus Hardware Pins ─────────────────────────────────────────────
#define PIN_MIC_CLK   0
#define PIN_MIC_DATA  34
#define PIN_RED_LED   10

// ─── Audio & Double-Buffer Streaming Settings ────────────────────────────────
// 8,000 Hz 4-bit IMA ADPCM = 4,000 bytes/sec.
// 32,000 bytes = 8.0 seconds per chunk (2 x 32KB ping-pong buffers in static .bss = 0 heap used!).
#define SAMPLE_RATE            16000
#define ADPCM_BYTES_PER_SEC    4000
#define CHUNK_SECONDS          8
#define CLIP_BUFFER_BYTES      (CHUNK_SECONDS * ADPCM_BYTES_PER_SEC) // 32,000 bytes (8.0s)
#define DMA_READ_BYTES         2048  // 1024 16-bit samples (64ms at 16kHz) per slice

// ─── On-Device Flash Log & Offline Queue Settings (SPIFFS ~2.88MB) ──────────
#define LOG_FILE_PATH          "/device.log"
#define LOG_OLD_FILE_PATH      "/device.log.old"
#define MAX_LOG_BYTES          120000
#define VAD_RMS_THRESHOLD      55
#define VAD_HANGOVER_MS        1500

// ─── Global State ────────────────────────────────────────────────────────────
Preferences prefs;
WebServer httpServer(80);
TFT_eSprite canvas = TFT_eSprite(&M5.Lcd);

String wifiSsid = "";
String wifiPass = "";
String serverHost = "";
uint16_t serverPort = DEFAULT_SERVER_PORT;
String deviceToken = "";

volatile bool isRecording = false;
volatile bool summaryRequested = false;
volatile bool bgUploadInProgress = false;
volatile bool pingRequested = false;
bool forceOfflineMode = false;
bool portalActive = false;
bool spiffsReady = false;
bool cloudOnline = false;
bool screenDimmed = false;

int micDownsampleFactor = 1; // Auto-calibrated in initMicrophone() for exact 8000 Hz output
uint32_t measuredMicHz = 8000;

uint32_t recordStartMillis = 0;
uint32_t lastInteractionMillis = 0;
volatile uint32_t chunksSentSession = 0;
volatile uint32_t totalChunksToday = 0;
volatile uint32_t speechChunksToday = 0;
volatile uint32_t offlineQueueCount = 0;
uint32_t offlineFileSeq = 1;
int currentRms = 0;
int peakRmsDisplay = 0;
String statusBanner = "Initializing...";
String sessionId = "";

// ─── Dual-Core Non-Blocking Ping-Pong Audio Buffers & FreeRTOS Queues ────────
int16_t i2sBuffer[DMA_READ_BYTES / 2];
uint8_t adpcmSliceBuffer[DMA_READ_BYTES / 4];
uint8_t pingPongClips[2][CLIP_BUFFER_BYTES]; // Static .bss Buffer 0 & Buffer 1 (leaves 100% of heap free for WiFi!)
int activeCaptureBuf = 0;
size_t activeCaptureBytes = 0;
int activeCapturePeakRms = 0;

struct UploadJob {
    int bufIndex;          // 0 or 1
    size_t byteCount;
    int peakRms;
    char session[24];
    bool isFinalStopChunk;
};

struct LogJob {
    char line[192];
};

QueueHandle_t uploadQueue = NULL;
QueueHandle_t logQueue = NULL;
SemaphoreHandle_t spiffsMutex = NULL;

// ─── IMA ADPCM 4-bit Encoder Tables & State ──────────────────────────────────
static const int8_t IMA_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int16_t IMA_STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

struct AdpcmState {
    int32_t predictor = 0;
    int32_t stepIndex = 0;
};

AdpcmState liveAdpcmState;

uint8_t encodeAdpcmSample(int16_t sample, AdpcmState& st) {
    int32_t step = IMA_STEP_TABLE[st.stepIndex];
    int32_t diff = sample - st.predictor;
    uint8_t nibble = 0;
    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }
    int32_t vpdiff = (step >> 3);
    if (diff >= step) {
        nibble |= 4;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
        nibble |= 2;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
        nibble |= 1;
        vpdiff += step;
    }
    if (nibble & 8) st.predictor -= vpdiff;
    else st.predictor += vpdiff;

    if (st.predictor > 32767) st.predictor = 32767;
    else if (st.predictor < -32768) st.predictor = -32768;

    st.stepIndex += IMA_INDEX_TABLE[nibble];
    if (st.stepIndex < 0) st.stepIndex = 0;
    else if (st.stepIndex > 88) st.stepIndex = 88;

    return nibble & 0x0F;
}

// Encodes input PCM16 samples into 8kHz 4-bit IMA ADPCM using calibrated micDownsampleFactor (1 or 2)
size_t encodePcm16ToAdpcm8k(const int16_t* pcm16, size_t numSamples, uint8_t* outAdpcm, AdpcmState& st) {
    int64_t sum = 0;
    for (size_t i = 0; i < numSamples; i++) sum += pcm16[i];
    int32_t mean = (numSamples > 0) ? (sum / (int64_t)numSamples) : 0;

    size_t outIdx = 0;
    if (micDownsampleFactor == 1) {
        // Hardware PDM already delivers 8,000 samples/sec -> encode every pair of samples into 1 byte
        for (size_t i = 0; i + 1 < numSamples; i += 2) {
            int32_t s0 = ((int32_t)pcm16[i] - mean) * 4;
            if (s0 > 32767) s0 = 32767;
            else if (s0 < -32768) s0 = -32768;

            int32_t s1 = ((int32_t)pcm16[i + 1] - mean) * 4;
            if (s1 > 32767) s1 = 32767;
            else if (s1 < -32768) s1 = -32768;

            uint8_t lowNib = encodeAdpcmSample((int16_t)s0, st);
            uint8_t highNib = encodeAdpcmSample((int16_t)s1, st);
            outAdpcm[outIdx++] = lowNib | (highNib << 4);
        }
    } else {
        // Hardware PDM delivers 16,000 samples/sec -> downsample 2:1 to 8,000 samples/sec
        for (size_t i = 0; i + 3 < numSamples; i += 4) {
            int32_t s0 = (((int32_t)pcm16[i] - mean) + ((int32_t)pcm16[i + 1] - mean)) * 2;
            if (s0 > 32767) s0 = 32767;
            else if (s0 < -32768) s0 = -32768;

            int32_t s1 = (((int32_t)pcm16[i + 2] - mean) + ((int32_t)pcm16[i + 3] - mean)) * 2;
            if (s1 > 32767) s1 = 32767;
            else if (s1 < -32768) s1 = -32768;

            uint8_t lowNib = encodeAdpcmSample((int16_t)s0, st);
            uint8_t highNib = encodeAdpcmSample((int16_t)s1, st);
            outAdpcm[outIdx++] = lowNib | (highNib << 4);
        }
    }
    return outIdx;
}

// ─── Hardware RTC Timestamp Helper ───────────────────────────────────────────
String getRtcTimestamp() {
    RTC_DateTypeDef d;
    RTC_TimeTypeDef t;
    M5.Rtc.GetDate(&d);
    M5.Rtc.GetTime(&t);
    if (d.Year < 2025 || d.Year > 2035) {
        char upBuf[24];
        snprintf(upBuf, sizeof(upBuf), "up+%lus", millis() / 1000);
        return String(upBuf);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             d.Year, d.Month, d.Date, t.Hours, t.Minutes, t.Seconds);
    return String(buf);
}

void syncRtcFromStrings(const String& dateStr, const String& timeStr) {
    if (dateStr.length() >= 10 && timeStr.length() >= 8) {
        RTC_DateTypeDef d;
        RTC_TimeTypeDef t;
        d.Year = dateStr.substring(0, 4).toInt();
        d.Month = dateStr.substring(5, 7).toInt();
        d.Date = dateStr.substring(8, 10).toInt();
        d.WeekDay = 0;
        t.Hours = timeStr.substring(0, 2).toInt();
        t.Minutes = timeStr.substring(3, 5).toInt();
        t.Seconds = timeStr.substring(6, 8).toInt();
        M5.Rtc.SetDate(&d);
        M5.Rtc.SetTime(&t);
    }
}

// ─── Non-Blocking Persistent Flash Logger + Async Cloud Mirror Queue ─────────
void deviceLog(const String& msg, bool mirrorToCloud = true) {
    float vbat = M5.Axp.GetBatVoltage();
    String line = "[" + getRtcTimestamp() + " | " + String(vbat, 2) + "V] " + msg;
    Serial.println(line);

    if (spiffsReady && spiffsMutex != NULL) {
        if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            File f = SPIFFS.open(LOG_FILE_PATH, FILE_APPEND);
            if (f) {
                size_t sz = f.size();
                f.println(line);
                f.close();
                if (sz > MAX_LOG_BYTES) {
                    SPIFFS.remove(LOG_OLD_FILE_PATH);
                    SPIFFS.rename(LOG_FILE_PATH, LOG_OLD_FILE_PATH);
                }
            }
            xSemaphoreGive(spiffsMutex);
        }
    }

    if (mirrorToCloud && logQueue != NULL && !forceOfflineMode) {
        LogJob job;
        strncpy(job.line, line.c_str(), sizeof(job.line) - 1);
        job.line[sizeof(job.line) - 1] = '\0';
        xQueueSend(logQueue, &job, 0); // Non-blocking! Never stalls microphone capture
    }
}

void refreshOfflineQueueCount() {
    if (!spiffsReady || spiffsMutex == NULL) {
        offlineQueueCount = 0;
        return;
    }
    if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        uint32_t cnt = 0;
        File root = SPIFFS.open("/");
        File file = root.openNextFile();
        while (file) {
            String name = String(file.name());
            if (name.startsWith("/q_") || name.startsWith("q_")) {
                cnt++;
            }
            file = root.openNextFile();
        }
        offlineQueueCount = cnt;
        xSemaphoreGive(spiffsMutex);
    }
}

void dumpDeviceLogsToSerial() {
    Serial.println("=== BEGIN ON-DEVICE FLASH LOG (/device.log) ===");
    if (!spiffsReady) {
        Serial.println("[!] SPIFFS not mounted.");
    } else if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        Serial.printf("[SPIFFS] Total: %u KB | Used: %u KB | Queued Offline Clips: %lu | MicRate: %lu Hz\n",
                      SPIFFS.totalBytes() / 1024, SPIFFS.usedBytes() / 1024, offlineQueueCount, measuredMicHz);
        if (SPIFFS.exists(LOG_OLD_FILE_PATH)) {
            File fold = SPIFFS.open(LOG_OLD_FILE_PATH, FILE_READ);
            while (fold && fold.available()) Serial.write(fold.read());
            if (fold) fold.close();
        }
        if (SPIFFS.exists(LOG_FILE_PATH)) {
            File f = SPIFFS.open(LOG_FILE_PATH, FILE_READ);
            while (f && f.available()) Serial.write(f.read());
            if (f) f.close();
        } else {
            Serial.println("(Log file is empty)");
        }
        xSemaphoreGive(spiffsMutex);
    }
    Serial.println("=== END ON-DEVICE FLASH LOG ===");
}

// ─── Sound / LED / Power Helpers ─────────────────────────────────────────────
void setRedLed(bool on) {
    digitalWrite(PIN_RED_LED, on ? LOW : HIGH);
}

bool isUsbPowered() {
    return M5.Axp.GetVBusVoltage() > 4.5f;
}

void wakeScreen() {
    lastInteractionMillis = millis();
    if (screenDimmed) {
        M5.Axp.ScreenBreath(11);
        screenDimmed = false;
    }
}

void checkAutoDimScreen() {
    if (isUsbPowered()) {
        if (screenDimmed) {
            M5.Axp.ScreenBreath(11);
            screenDimmed = false;
        }
        return;
    }
    if (!screenDimmed && (millis() - lastInteractionMillis > 15000)) {
        M5.Axp.ScreenBreath(7);
        screenDimmed = true;
    }
}

void beepTone(int freq, int durationMs) {
    M5.Beep.tone(freq);
    delay(durationMs);
    M5.Beep.mute();
}

// ─── I2S PDM Microphone Setup & Hardware Sample Rate Auto-Calibration ────────
size_t readMicSlice(int16_t* outBuf, size_t maxBytes, int* outRms) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_NUM_0, (void*)outBuf, maxBytes, &bytesRead, 25 / portTICK_PERIOD_MS);
    if (err != ESP_OK || bytesRead == 0) {
        *outRms = 0;
        return 0;
    }
    int count = bytesRead / 2;
    int64_t sum = 0;
    for (int i = 0; i < count; i++) sum += outBuf[i];
    int32_t mean = sum / count;
    uint64_t sqSum = 0;
    for (int i = 0; i < count; i++) {
        int32_t v = outBuf[i] - mean;
        sqSum += (uint64_t)(v * v);
    }
    *outRms = (int)sqrt((double)sqSum / count);
    return bytesRead;
}

void initMicrophone() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ALL_RIGHT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_PIN_NO_CHANGE,
        .ws_io_num = PIN_MIC_CLK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PIN_MIC_DATA
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

    // Drain pre-buffered DMA slices until synchronized to real-time hardware delivery
    int dummyRms = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t tSlice = millis();
        readMicSlice(i2sBuffer, DMA_READ_BYTES, &dummyRms);
        if (millis() - tSlice >= 20) break;
    }

    uint32_t tStart = millis();
    uint32_t samplesCounted = 0;
    while (millis() - tStart < 300) {
        size_t got = readMicSlice(i2sBuffer, DMA_READ_BYTES, &dummyRms);
        samplesCounted += (got / 2);
    }
    uint32_t elapsed = millis() - tStart;
    if (elapsed == 0) elapsed = 1;
    measuredMicHz = (samplesCounted * 1000) / elapsed;
    if (measuredMicHz < 12000) {
        micDownsampleFactor = 1; // Hardware PDM delivers ~8,000 Hz directly
    } else {
        micDownsampleFactor = 2; // Hardware PDM delivers ~16,000 Hz -> 2:1 downsample to 8,000 Hz
    }
}

// ─── Draw UI on 240x135 Color TFT ────────────────────────────────────────────
void drawScreen() {
    canvas.fillSprite(TFT_BLACK);

    // 1. Top Header Bar (y: 0..22)
    canvas.fillRect(0, 0, 240, 22, 0x10A2);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_WHITE, 0x10A2);

    bool onlineNow = (!forceOfflineMode && WiFi.status() == WL_CONNECTED);
    if (onlineNow) {
        canvas.setTextColor(0x07E0, 0x10A2);
        String ssidShort = wifiSsid.length() > 10 ? wifiSsid.substring(0, 10) : wifiSsid;
        canvas.drawString("WiFi:" + ssidShort, 4, 4);
    } else if (forceOfflineMode) {
        canvas.setTextColor(0xFD20, 0x10A2);
        canvas.drawString("MODE:OFFLINE", 4, 4);
    } else {
        canvas.setTextColor(TFT_YELLOW, 0x10A2);
        canvas.drawString("WiFi:Offline", 4, 4);
    }

    if (onlineNow && cloudOnline) {
        canvas.setTextColor(0x07FF, 0x10A2);
        canvas.drawString(bgUploadInProgress ? "UP.." : "VM:OK", 114, 4);
    } else {
        canvas.setTextColor(0xFD20, 0x10A2);
        canvas.drawString("Q:" + String(offlineQueueCount), 114, 4);
    }

    float vbat = M5.Axp.GetBatVoltage();
    int batPct = (int)((vbat - 3.3f) / (4.15f - 3.3f) * 100.0f);
    if (batPct > 100) batPct = 100;
    if (batPct < 0) batPct = 0;
    bool usbOn = isUsbPowered();
    uint16_t batColor = usbOn ? 0x07E0 : (batPct > 25 ? TFT_WHITE : TFT_RED);
    canvas.setTextColor(batColor, 0x10A2);
    char batStr[20];
    if (usbOn) {
        snprintf(batStr, sizeof(batStr), "USB %d%%", batPct);
    } else {
        snprintf(batStr, sizeof(batStr), "%d%% %.1fV", batPct, vbat);
    }
    canvas.drawRightString(batStr, 236, 4, 1);

    // 2. Main Center State Area (y: 26..88)
    if (isRecording) {
        uint16_t cardBg = onlineNow ? 0x4800 : 0x4200;
        canvas.fillRoundRect(6, 26, 228, 60, 6, cardBg);
        if ((millis() / 500) % 2 == 0) {
            canvas.fillCircle(20, 43, 7, onlineNow ? TFT_RED : TFT_ORANGE);
        } else {
            canvas.drawCircle(20, 43, 7, onlineNow ? TFT_RED : TFT_ORANGE);
        }
        canvas.setTextColor(TFT_WHITE, cardBg);
        canvas.drawString(onlineNow ? "REC LIVE" : "REC FLASH", 34, 35, 2);

        uint32_t elapsedSec = (millis() - recordStartMillis) / 1000;
        uint32_t hh = elapsedSec / 3600;
        uint32_t mm = (elapsedSec % 3600) / 60;
        uint32_t ss = elapsedSec % 60;
        char timeStr[20];
        snprintf(timeStr, sizeof(timeStr), "%02lu:%02lu:%02lu", hh, mm, ss);
        canvas.setTextColor(TFT_YELLOW, cardBg);
        canvas.drawRightString(timeStr, 226, 35, 2);

        // Show clear non-blocking listening + background upload status
        String subLine = bgUploadInProgress
            ? ("Listening + BgUpload #" + String(chunksSentSession + 1))
            : ("Listening (" + String(activeCaptureBytes / 1024) + "KB / " + String(CLIP_BUFFER_BYTES / 1024) + "KB)");
        canvas.setTextColor(0xCE79, cardBg);
        canvas.drawString(subLine, 14, 62, 2);
    } else {
        canvas.fillRoundRect(6, 26, 228, 60, 6, 0x0208);
        canvas.fillCircle(20, 43, 6, 0x07E0);
        canvas.setTextColor(TFT_WHITE, 0x0208);
        canvas.drawString(onlineNow ? "READY (ONLINE)" : "READY (OFFLINE)", 34, 35, 2);
        canvas.setTextColor(0x87F0, 0x0208);
        canvas.drawString(bgUploadInProgress ? "Finishing last upload..." : statusBanner, 14, 60, 2);
    }

    // 3. Live Microphone VU Meter Bar (y: 92..108)
    canvas.drawRect(6, 92, 228, 14, 0x4A49);
    int barWidth = map(constrain(peakRmsDisplay, 0, 2500), 0, 2500, 0, 224);
    uint16_t vuColor = 0x07E0;
    if (barWidth > 170) vuColor = TFT_RED;
    else if (barWidth > 100) vuColor = TFT_YELLOW;
    if (barWidth > 0) {
        canvas.fillRect(8, 94, barWidth, 10, vuColor);
    }

    // 4. Bottom Footer Stats Bar (y: 112..135)
    canvas.setTextColor(0xBDF7, TFT_BLACK);
    char statsBuf[64];
    snprintf(statsBuf, sizeof(statsBuf), "Sent:%lu | Queued:%lu | Today:%lu",
             chunksSentSession, offlineQueueCount, totalChunksToday);
    canvas.drawString(statsBuf, 6, 112, 1);

    canvas.setTextColor(0x7BEF, TFT_BLACK);
    canvas.drawString("M5:Rec/Stop  |  Side:Email / Mode", 6, 124, 1);

    canvas.pushSprite(0, 0);
}

// ─── Spill a Captured Buffer to SPIFFS Flash When Offline ────────────────────
void saveBufferToSpiffsQueue(const uint8_t* data, size_t len) {
    if (!spiffsReady || spiffsMutex == NULL || len < 800) return;
    if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (SPIFFS.totalBytes() - SPIFFS.usedBytes() >= len + 32000) {
            char pathBuf[32];
            snprintf(pathBuf, sizeof(pathBuf), "/q_%04lu.adpcm", offlineFileSeq++);
            File qf = SPIFFS.open(pathBuf, FILE_WRITE);
            if (qf) {
                qf.write(data, len);
                qf.close();
            }
        }
        xSemaphoreGive(spiffsMutex);
    }
    refreshOfflineQueueCount();
}

// ─── Cloud Network & String Helpers ──────────────────────────────────────────
String htmlEscape(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c == '\'') out += "&#39;";
        else out += c;
    }
    return out;
}

String urlEncode(const String& s) {
    String out;
    const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[(c >> 4) & 0x0F];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

bool readHttpResponse(WiFiClient& client, String& outResp, const char* stopMarker, uint32_t timeoutMs) {
    outResp = "";
    uint32_t t0 = millis();
    while ((client.connected() || client.available()) && (millis() - t0 < timeoutMs)) {
        while (client.available()) {
            outResp += (char)client.read();
        }
        if (stopMarker && outResp.indexOf(stopMarker) >= 0) {
            // Brief yield so any trailing bytes in the same packet land cleanly
            vTaskDelay(pdMS_TO_TICKS(10));
            while (client.available()) outResp += (char)client.read();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    client.stop();
    return (outResp.indexOf("200 OK") >= 0 || outResp.indexOf("\"ok\": true") >= 0 || outResp.indexOf("\"ok\":true") >= 0);
}

bool writeAllToClient(WiFiClient& client, const uint8_t* buf, size_t len) {
    size_t offset = 0;
    uint32_t lastProgress = millis();
    while (offset < len) {
        if (!client.connected()) return false;
        size_t chunk = min((size_t)1024, len - offset);
        size_t w = client.write(buf + offset, chunk);
        if (w > 0) {
            offset += w;
            lastProgress = millis();
        } else {
            if (millis() - lastProgress > 6000) return false;
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
    return true;
}

bool performCloudPing() {
    if (forceOfflineMode || WiFi.status() != WL_CONNECTED || serverHost.length() == 0) {
        cloudOnline = false;
        return false;
    }
    WiFiClient client;
    client.setTimeout(3);
    if (!client.connect(serverHost.c_str(), serverPort)) {
        cloudOnline = false;
        return false;
    }
    float vbat = M5.Axp.GetBatVoltage();
    String path = String("/api/ping?rec=") + (isRecording ? "1" : "0") +
                  "&vbat=" + String(vbat, 2) +
                  "&usb=" + (isUsbPowered() ? "1" : "0") +
                  "&chunks=" + String(chunksSentSession) +
                  "&queued=" + String(offlineQueueCount) +
                  "&ip=" + urlEncode(WiFi.localIP().toString()) +
                  "&ssid=" + urlEncode(wifiSsid);
    String req = String("GET ") + path + " HTTP/1.1\r\n" +
                 "Host: " + serverHost + "\r\n";
    if (deviceToken.length() > 0) {
        req += "X-Device-Token: " + deviceToken + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    client.print(req);

    String resp;
    if (readHttpResponse(client, resp, "}", 3500)) {
        cloudOnline = true;
        int idx1 = resp.indexOf("\"chunks_today\":");
        if (idx1 >= 0) totalChunksToday = resp.substring(idx1 + 15).toInt();
        int idx2 = resp.indexOf("\"speech_today\":");
        if (idx2 >= 0) speechChunksToday = resp.substring(idx2 + 15).toInt();

        int dIdx = resp.indexOf("\"date_nzst\": \"");
        int tIdx = resp.indexOf("\"time_nzst\": \"");
        if (dIdx >= 0 && tIdx >= 0) {
            String dStr = resp.substring(dIdx + 14, dIdx + 24);
            String tStr = resp.substring(tIdx + 14, tIdx + 22);
            syncRtcFromStrings(dStr, tStr);
        }
        return true;
    }
    cloudOnline = false;
    return false;
}

void performOffloadQueuedFiles() {
    if (!spiffsReady || forceOfflineMode || WiFi.status() != WL_CONNECTED || serverHost.length() == 0 || deviceToken.length() == 0) return;
    refreshOfflineQueueCount();
    if (offlineQueueCount == 0) return;

    while (!forceOfflineMode && WiFi.status() == WL_CONNECTED) {
        String targetFile = "";
        size_t targetSize = 0;

        if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            File root = SPIFFS.open("/");
            File file = root.openNextFile();
            while (file) {
                String fname = String(file.name());
                if (!fname.startsWith("/")) fname = "/" + fname;
                size_t fsize = file.size();
                file.close();
                if (fname.startsWith("/q_") && fsize > 0) {
                    targetFile = fname;
                    targetSize = fsize;
                    break;
                }
                file = root.openNextFile();
            }
            xSemaphoreGive(spiffsMutex);
        }

        if (targetFile.length() == 0) break;

        WiFiClient client;
        client.setTimeout(12);
        if (!client.connect(serverHost.c_str(), serverPort)) {
            break;
        }

        String hdr = String("POST /api/audio HTTP/1.1\r\n") +
                     "Host: " + serverHost + "\r\n" +
                     "Content-Type: application/octet-stream\r\n" +
                     "X-Device-Token: " + deviceToken + "\r\n" +
                     "X-Session-Id: offline_" + targetFile.substring(3) + "\r\n" +
                     "X-Audio-Format: adpcm8k\r\n" +
                     "X-Sample-Rate: 8000\r\n" +
                     "Content-Length: " + String(targetSize) + "\r\n" +
                     "Connection: close\r\n\r\n";
        client.print(hdr);

        if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            File qf = SPIFFS.open(targetFile, FILE_READ);
            if (qf) {
                uint8_t buf[1024];
                while (qf.available()) {
                    size_t r = qf.read(buf, sizeof(buf));
                    if (r > 0) {
                        if (!writeAllToClient(client, buf, r)) break;
                    }
                }
                qf.close();
            }
            xSemaphoreGive(spiffsMutex);
        }

        String resp;
        if (readHttpResponse(client, resp, "\"ok\"", 5000)) {
            if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                SPIFFS.remove(targetFile);
                xSemaphoreGive(spiffsMutex);
            }
            chunksSentSession++;
            totalChunksToday++;
            refreshOfflineQueueCount();
            deviceLog("OFFLOAD_OK: Uploaded & deleted " + targetFile + " (" + String(targetSize) + " B ADPCM)");
        } else {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void performBackgroundSummaryTrigger() {
    if (forceOfflineMode || WiFi.status() != WL_CONNECTED || serverHost.length() == 0 || deviceToken.length() == 0) return;
    statusBanner = "Emailing Summary...";
    deviceLog("SUMMARY_TRIGGER: Requesting daily summary email");

    WiFiClient client;
    client.setTimeout(15);
    if (!client.connect(serverHost.c_str(), serverPort)) {
        statusBanner = "VM Connect Failed";
        deviceLog("SUMMARY_ERR: Failed to connect to Cloud server");
        return;
    }
    client.print(String("POST /api/summarize HTTP/1.1\r\n") +
                 "Host: " + serverHost + "\r\n" +
                 "X-Device-Token: " + deviceToken + "\r\n" +
                 "Content-Length: 0\r\n" +
                 "Connection: close\r\n\r\n");

    String resp;
    if (readHttpResponse(client, resp, "\"ok\"", 15000)) {
        statusBanner = "Email Triggered OK!";
        deviceLog("SUMMARY_OK: Daily summary email triggered");
    } else {
        statusBanner = "Summary Error";
        deviceLog("SUMMARY_ERR: Server returned error on /api/summarize");
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (!isRecording) {
        statusBanner = "Press [M5] to Start Rec";
    }
}

// ─── Core 0 Background Network Worker Task ───────────────────────────────────
void networkWorkerTask(void* pvParameters) {
    UploadJob upJob;
    LogJob lgJob;

    for (;;) {
        // 1. Drain any pending audio upload jobs first (highest network priority)
        if (xQueueReceive(uploadQueue, &upJob, 0) == pdTRUE) {
            bgUploadInProgress = true;
            const uint8_t* dataPtr = pingPongClips[upJob.bufIndex];
            size_t dataLen = upJob.byteCount;

            if (forceOfflineMode || WiFi.status() != WL_CONNECTED || serverHost.length() == 0 || deviceToken.length() == 0) {
                saveBufferToSpiffsQueue(dataPtr, dataLen);
                deviceLog("OFFLINE_SAVED: Buffered " + String(dataLen) + " B ADPCM to SPIFFS flash", false);
            } else {
                WiFiClient client;
                client.setTimeout(12);
                if (!client.connect(serverHost.c_str(), serverPort)) {
                    cloudOnline = false;
                    saveBufferToSpiffsQueue(dataPtr, dataLen);
                    deviceLog("STREAM_SPILL_TO_FLASH: Saved " + String(dataLen) + " B to SPIFFS", false);
                } else {
                    cloudOnline = true;
                    String hdr = String("POST /api/audio HTTP/1.1\r\n") +
                                 "Host: " + serverHost + "\r\n" +
                                 "Content-Type: application/octet-stream\r\n" +
                                 "X-Device-Token: " + deviceToken + "\r\n" +
                                 "X-Session-Id: " + String(upJob.session) + "\r\n" +
                                 "X-Audio-Format: adpcm8k\r\n" +
                                 "X-Sample-Rate: 8000\r\n" +
                                 "Content-Length: " + String(dataLen) + "\r\n" +
                                 "Connection: close\r\n\r\n";
                    client.print(hdr);
                    writeAllToClient(client, dataPtr, dataLen);

                    String resp;
                    if (readHttpResponse(client, resp, "\"ok\"", 4500)) {
                        chunksSentSession++;
                        totalChunksToday++;
                        deviceLog("CHUNK_OK: #" + String(chunksSentSession) +
                                  " Session=" + String(upJob.session) +
                                  " Bytes=" + String(dataLen) +
                                  " PeakRMS=" + String(upJob.peakRms));
                    } else {
                        saveBufferToSpiffsQueue(dataPtr, dataLen);
                        deviceLog("CHUNK_RETRY_SPILL: Saved " + String(dataLen) + " B to SPIFFS", false);
                    }
                }
            }

            if (upJob.isFinalStopChunk) {
                uint32_t totalRecSec = (millis() - recordStartMillis) / 1000;
                deviceLog("RECORD_STOP: Session=" + String(upJob.session) +
                          " TotalSec=" + String(totalRecSec) +
                          " Chunks=" + String(chunksSentSession));
                performCloudPing();
            }

            bgUploadInProgress = false;
            continue;
        }

        // 2. Drain any queued cloud log lines
        if (xQueueReceive(logQueue, &lgJob, 0) == pdTRUE) {
            if (!forceOfflineMode && WiFi.status() == WL_CONNECTED && serverHost.length() > 0 && deviceToken.length() > 0) {
                WiFiClient c;
                c.setTimeout(2);
                if (c.connect(serverHost.c_str(), serverPort)) {
                    size_t l = strlen(lgJob.line);
                    c.print(String("POST /api/device-log HTTP/1.1\r\n") +
                            "Host: " + serverHost + "\r\n" +
                            "X-Device-Token: " + deviceToken + "\r\n" +
                            "Content-Type: text/plain\r\n" +
                            "Content-Length: " + String(l) + "\r\n" +
                            "Connection: close\r\n\r\n" +
                            String(lgJob.line));
                    c.stop();
                }
            }
            continue;
        }

        // 3. Handle on-demand Summary Email request
        if (summaryRequested) {
            summaryRequested = false;
            performBackgroundSummaryTrigger();
            continue;
        }

        // 4. Handle periodic cloud ping, auto Wi-Fi reconnect & offline SPIFFS offload
        if (pingRequested) {
            pingRequested = false;
            if (!forceOfflineMode && WiFi.status() != WL_CONNECTED && wifiSsid.length() > 0) {
                WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
                uint32_t tRec = millis();
                while (WiFi.status() != WL_CONNECTED && (millis() - tRec < 4500)) {
                    vTaskDelay(pdMS_TO_TICKS(150));
                }
                if (WiFi.status() == WL_CONNECTED) {
                    deviceLog("WIFI_RECONNECTED: SSID='" + wifiSsid + "' IP=" + WiFi.localIP().toString());
                }
            }
            if (!forceOfflineMode && WiFi.status() == WL_CONNECTED) {
                performCloudPing();
                if (!isRecording && offlineQueueCount > 0) {
                    performOffloadQueuedFiles();
                }
            }
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ─── Enqueue Current Capture Buffer and Immediately Swap to Alternate Buffer ─
void flushAndSwapCaptureBuffer(bool isStopEvent) {
    if (activeCaptureBytes >= 600) {
        UploadJob job;
        job.bufIndex = activeCaptureBuf;
        job.byteCount = activeCaptureBytes;
        job.peakRms = activeCapturePeakRms;
        strncpy(job.session, sessionId.c_str(), sizeof(job.session) - 1);
        job.session[sizeof(job.session) - 1] = '\0';
        job.isFinalStopChunk = isStopEvent;
        xQueueSend(uploadQueue, &job, 0);

        // Immediately swap to the alternate ping-pong buffer (0 -> 1 or 1 -> 0) with zero gap!
        activeCaptureBuf = 1 - activeCaptureBuf;
    } else if (isStopEvent) {
        uint32_t totalRecSec = (millis() - recordStartMillis) / 1000;
        deviceLog("RECORD_STOP: Session=" + sessionId +
                  " TotalSec=" + String(totalRecSec) +
                  " Chunks=" + String(chunksSentSession));
        pingRequested = true;
    }

    activeCaptureBytes = 0;
    activeCapturePeakRms = 0;
}

// ─── Web Server (Local On-Device Log Viewer + Wi-Fi Setup Portal) ────────────
void sendCorsHeaders() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    httpServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpServer.sendHeader("Access-Control-Allow-Private-Network", "true");
}

void setupHttpRoutes() {
    httpServer.on("/", HTTP_OPTIONS, []() {
        sendCorsHeaders();
        httpServer.send(204);
    });

    httpServer.on("/", HTTP_GET, []() {
        sendCorsHeaders();
        String safeSsid = htmlEscape(wifiSsid);
        String safeHost = htmlEscape(serverHost);
        String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                      "<title>M5StickC Plus Audio Logger</title>"
                      "<style>body{font-family:sans-serif;background:#0f172a;color:#fff;padding:20px;max-width:700px;margin:0 auto;}"
                      "a{color:#38bdf8;}input,button{width:100%;padding:12px;margin:6px 0 14px;border-radius:8px;border:none;font-size:16px;box-sizing:border-box;}"
                      "label{font-size:14px;color:#cbd5e1;font-weight:bold;}button{background:#2563eb;color:#fff;font-weight:bold;cursor:pointer;}</style></head><body>"
                      "<h2>🎙️ M5StickC Plus Audio Logger</h2>"
                      "<p>State: <strong>" + String(isRecording ? "RECORDING LIVE" : "STANDBY") + "</strong> | Mic Rate: " + String(measuredMicHz) + " Hz</p>"
                      "<p>SPIFFS Storage: " + String(SPIFFS.usedBytes() / 1024) + " KB used / " + String(SPIFFS.totalBytes() / 1024) + " KB total (" + String(offlineQueueCount) + " queued offline clips)</p>"
                      "<p><a href='/logs'>📄 View On-Device Flash Logs (/device.log)</a></p>"
                      "<form method='POST' action='/save'>"
                      "<label>2.4GHz Wi-Fi SSID:</label><input name='s' value='" + safeSsid + "' required>"
                      "<label>Wi-Fi Password:</label><input name='p' type='password' placeholder='Enter password (leave blank to keep current)'>"
                      "<label>Cloud Server Host / IP:</label><input name='h' value='" + safeHost + "' placeholder='e.g. your-server.example.com'>"
                      "<label>Cloud Server Port:</label><input name='pt' type='number' value='" + String(serverPort) + "'>"
                      "<label>Device Auth Token:</label><input name='t' type='password' placeholder='Enter token (leave blank to keep current)'>"
                      "<button type='submit'>Save Settings &amp; Reboot</button></form></body></html>";
        httpServer.send(200, "text/html", html);
    });

    httpServer.on("/logs", HTTP_GET, []() {
        sendCorsHeaders();
        if (!spiffsReady || !SPIFFS.exists(LOG_FILE_PATH)) {
            httpServer.send(200, "text/plain", "(No on-device logs yet)");
            return;
        }
        if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            File f = SPIFFS.open(LOG_FILE_PATH, FILE_READ);
            httpServer.streamFile(f, "text/plain");
            f.close();
            xSemaphoreGive(spiffsMutex);
        } else {
            httpServer.send(503, "text/plain", "Busy writing log");
        }
    });

    httpServer.on("/save", HTTP_POST, []() {
        sendCorsHeaders();
        String s = httpServer.arg("s");
        String p = httpServer.arg("p");
        String h = httpServer.arg("h");
        String pt = httpServer.arg("pt");
        String tok = httpServer.arg("t");
        s.trim();
        h.trim();
        if (s.length() > 0) {
            prefs.begin("wifi", false);
            prefs.putString("ssid", s);
            if (p.length() > 0 || s != wifiSsid) {
                prefs.putString("pass", p);
            }
            prefs.end();
        }
        if (h.length() > 0 || pt.length() > 0 || tok.length() > 0) {
            prefs.begin("cloud", false);
            if (h.length() > 0) prefs.putString("host", h);
            if (pt.toInt() > 0) prefs.putUShort("port", (uint16_t)pt.toInt());
            if (tok.length() > 0) prefs.putString("token", tok);
            prefs.end();
        }
        deviceLog("CFG_SAVED: Updated NVS settings via Web Portal (SSID='" + (s.length() > 0 ? s : wifiSsid) + "')", false);
        httpServer.send(200, "text/html", "<h2>Saved to NVS! Rebooting device...</h2>");
        delay(1200);
        ESP.restart();
    });

    httpServer.begin();
}

void startWiFiPortal() {
    if (portalActive) return;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("M5-AudioLogger", "");
    portalActive = true;
    setupHttpRoutes();
    deviceLog("PORTAL_START: AP 'M5-AudioLogger' started at 192.168.4.1", false);
}

void startRecordingSession(const char* sourceLabel) {
    isRecording = true;
    recordStartMillis = millis();
    chunksSentSession = 0;
    activeCaptureBytes = 0;
    activeCapturePeakRms = 0;
    liveAdpcmState = AdpcmState();
    sessionId = "s_" + String(millis());
    setRedLed(true);
    beepTone(1200, 80);
    delay(40);
    beepTone(1800, 100);
    statusBanner = "Recording Live";
    deviceLog(String("RECORD_START: ") + sourceLabel + " Session=" + sessionId, !forceOfflineMode);
    pingRequested = true;
    drawScreen();
}

void stopRecordingSession(const char* sourceLabel) {
    isRecording = false;
    setRedLed(false);
    beepTone(1600, 70);
    delay(40);
    beepTone(1100, 90);
    statusBanner = "Press [M5] to Start Rec";
    flushAndSwapCaptureBuffer(true);
    drawScreen();
}

void checkSerialCommands() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    wakeScreen();

    if (line.equalsIgnoreCase("SCAN")) {
        Serial.println("[WIFI_SCAN] Scanning 2.4GHz networks...");
        int n = WiFi.scanNetworks();
        for (int i = 0; i < n; i++) {
            Serial.printf("[WIFI_NET] %d: SSID='%s' RSSI=%d dBm\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
        }
        Serial.println("[WIFI_SCAN] Done.");
    } else if (line.equalsIgnoreCase("LOGS")) {
        dumpDeviceLogsToSerial();
    } else if (line.equalsIgnoreCase("CLEARLOGS")) {
        if (spiffsReady && xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            SPIFFS.remove(LOG_FILE_PATH);
            SPIFFS.remove(LOG_OLD_FILE_PATH);
            xSemaphoreGive(spiffsMutex);
        }
        deviceLog("LOGS_CLEARED: On-device flash logs reset");
    } else if (line.equalsIgnoreCase("OFFLINE")) {
        forceOfflineMode = true;
        deviceLog("MODE_SET: Forced OFFLINE store-and-forward mode", false);
        drawScreen();
    } else if (line.equalsIgnoreCase("ONLINE")) {
        forceOfflineMode = false;
        deviceLog("MODE_SET: Restored ONLINE cloud mode");
        pingRequested = true;
        drawScreen();
    } else if (line.equalsIgnoreCase("OFFLOAD")) {
        pingRequested = true;
    } else if (line.equalsIgnoreCase("SUMMARY")) {
        summaryRequested = true;
    } else if (line.startsWith("WIFI:")) {
        int firstColon = line.indexOf(':');
        int secondColon = line.indexOf(':', firstColon + 1);
        if (secondColon > firstColon) {
            String newSsid = line.substring(firstColon + 1, secondColon);
            String newPass = line.substring(secondColon + 1);
            prefs.begin("wifi", false);
            prefs.putString("ssid", newSsid);
            prefs.putString("pass", newPass);
            prefs.end();
            wifiSsid = newSsid;
            wifiPass = newPass;
            Serial.printf("[WIFI_CFG] Saved SSID='%s'. Connecting now...\n", wifiSsid.c_str());
            WiFi.mode(WIFI_STA);
            WiFi.setSleep(false);
            portalActive = false;
            WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
            uint32_t t0 = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - t0 < 12000)) {
                delay(250);
                Serial.print(".");
            }
            Serial.println();
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("[WIFI_OK] Connected! IP=%s\n", WiFi.localIP().toString().c_str());
                setupHttpRoutes();
                pingRequested = true;
                deviceLog("WIFI_OK: Connected to '" + wifiSsid + "' IP=" + WiFi.localIP().toString());
            } else {
                Serial.println("[WIFI_ERR] Connection failed.");
                deviceLog("WIFI_ERR: Failed connecting to '" + wifiSsid + "'", false);
                startWiFiPortal();
            }
            drawScreen();
        }
    } else if (line.startsWith("CLOUD:")) {
        int c1 = line.indexOf(':');
        int c2 = line.indexOf(':', c1 + 1);
        int c3 = line.indexOf(':', c2 + 1);
        if (c2 > c1 && c3 > c2) {
            String newHost = line.substring(c1 + 1, c2);
            uint16_t newPort = (uint16_t)line.substring(c2 + 1, c3).toInt();
            String newToken = line.substring(c3 + 1);
            newHost.trim();
            newToken.trim();
            if (newPort == 0) newPort = DEFAULT_SERVER_PORT;
            prefs.begin("cloud", false);
            prefs.putString("host", newHost);
            prefs.putUShort("port", newPort);
            prefs.putString("token", newToken);
            prefs.end();
            serverHost = newHost;
            serverPort = newPort;
            deviceToken = newToken;
            Serial.printf("[CLOUD_CFG] Saved Host=%s:%u TokenSet=%d\n", serverHost.c_str(), serverPort, deviceToken.length() > 0 ? 1 : 0);
            if (WiFi.status() == WL_CONNECTED && performCloudPing()) {
                Serial.println("[CLOUD_OK] Cloud server reachable and authenticated.");
                statusBanner = "Press [M5] to Start Rec";
                deviceLog("CLOUD_CFG: Connected Host=" + serverHost + ":" + String(serverPort));
                pingRequested = true;
            } else {
                Serial.println("[CLOUD_WARN] Saved to NVS (cloud offline or Wi-Fi disconnected).");
            }
            drawScreen();
        } else {
            Serial.println("[CLOUD_ERR] Expected CLOUD:<host>:<port>:<token>");
        }
    } else if (line.equalsIgnoreCase("STATUS")) {
        refreshOfflineQueueCount();
        Serial.printf("[STATUS] WiFi=%d SSID='%s' IP=%s CloudHost=%s:%u TokenSet=%d Cloud=%d OfflineMode=%d Rec=%d BgUp=%d MicHz=%lu Downsample=%d Heap=%uB Chunks=%lu Queued=%lu FlashFree=%uKB\n",
                      (int)WiFi.status(), wifiSsid.c_str(), WiFi.localIP().toString().c_str(),
                      serverHost.c_str(), serverPort, deviceToken.length() > 0 ? 1 : 0,
                      (int)cloudOnline, (int)forceOfflineMode, (int)isRecording, (int)bgUploadInProgress,
                      measuredMicHz, micDownsampleFactor, ESP.getFreeHeap(),
                      chunksSentSession, offlineQueueCount,
                      spiffsReady ? (SPIFFS.totalBytes() - SPIFFS.usedBytes()) / 1024 : 0);
    } else if (line.equalsIgnoreCase("REC")) {
        if (!isRecording) startRecordingSession("Serial");
    } else if (line.equalsIgnoreCase("STOP")) {
        if (isRecording) stopRecordingSession("Serial");
    }
}

// ─── Arduino Setup & Main Loop (Core 1 = Real-Time Audio, Core 0 = Network) ──
void setup() {
    M5.begin(true, true, true);
    Serial.begin(115200);
    pinMode(PIN_RED_LED, OUTPUT);
    setRedLed(false);

    M5.Axp.ScreenBreath(11);
    M5.Lcd.setRotation(1);
    canvas.createSprite(240, 135);
    lastInteractionMillis = millis();

    spiffsMutex = xSemaphoreCreateMutex();
    uploadQueue = xQueueCreate(2, sizeof(UploadJob));
    logQueue = xQueueCreate(12, sizeof(LogJob));

    spiffsReady = SPIFFS.begin(true);
    refreshOfflineQueueCount();
    initMicrophone();

    prefs.begin("wifi", true);
    wifiSsid = prefs.getString("ssid", "");
    wifiPass = prefs.getString("pass", "");
    prefs.end();

    prefs.begin("cloud", true);
    serverHost = prefs.getString("host", "");
    serverPort = prefs.getUShort("port", DEFAULT_SERVER_PORT);
    deviceToken = prefs.getString("token", "");
    prefs.end();

    statusBanner = wifiSsid.length() > 0 ? ("Connecting " + wifiSsid) : "Setup: AP 192.168.4.1";
    drawScreen();

    // Launch Core 0 Background Network Worker Task
    xTaskCreatePinnedToCore(
        networkWorkerTask,
        "NetWorker",
        8192,
        NULL,
        1,
        NULL,
        0 // Pin to Core 0 so Core 1 (loop) has 100% uninterrupted real-time microphone capture
    );

    deviceLog("BOOT: Dual-Core Streaming Ready (Mic=" + String(measuredMicHz) +
              "Hz ds=" + String(micDownsampleFactor) +
              " Flash=" + String(SPIFFS.totalBytes() / 1024) + "KB)", false);

    if (wifiSsid.length() == 0) {
        statusBanner = "Setup: AP 192.168.4.1";
        deviceLog("WIFI_UNCONFIGURED: Join AP 'M5-AudioLogger' (192.168.4.1) or use USB configure_wifi.py", false);
        startWiFiPortal();
    } else {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false);
        delay(80);
        WiFi.setSleep(false); // Keep radio awake so local HTTP server responds quickly
        WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
        uint32_t t0 = millis();
        uint32_t lastRetry = t0;
        while (WiFi.status() != WL_CONNECTED && (millis() - t0 < 15000)) {
            if ((WiFi.status() == WL_CONNECT_FAILED || WiFi.status() == WL_NO_SSID_AVAIL) && (millis() - lastRetry > 3500)) {
                lastRetry = millis();
                WiFi.disconnect(false);
                delay(50);
                WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
            }
            checkSerialCommands();
            delay(200);
        }

        if (WiFi.status() == WL_CONNECTED) {
            setupHttpRoutes();
            if (serverHost.length() == 0 || deviceToken.length() == 0) {
                statusBanner = "Set Cloud via USB/Web";
                deviceLog("CLOUD_UNCONFIGURED: Run configure_wifi.py cloud or open http://" + WiFi.localIP().toString(), false);
            } else {
                performCloudPing();
                statusBanner = "Press [M5] to Start Rec";
                deviceLog("WIFI_CONNECTED: SSID='" + wifiSsid + "' IP=" + WiFi.localIP().toString() +
                          " CloudVM=" + String(cloudOnline ? "ONLINE" : "OFFLINE"));
                pingRequested = true;
            }
        } else {
            statusBanner = "Offline Ready (Flash)";
            deviceLog("WIFI_OFFLINE: Starting in Offline Store-and-Forward mode", false);
            startWiFiPortal();
        }
    }

    drawScreen();
}

uint32_t lastUiRefreshMillis = 0;
uint32_t lastPingMillis = 0;
uint32_t lastSpeechMillis = 0;

void loop() {
    M5.update();
    checkSerialCommands();
    httpServer.handleClient();
    checkAutoDimScreen();

    // Button A (Front M5 Button): Toggle Recording ON / OFF
    if (M5.BtnA.wasPressed()) {
        wakeScreen();
        if (!isRecording) {
            startRecordingSession("ButtonA");
        } else {
            stopRecordingSession("ButtonA");
        }
    }

    // Button B (Side Button): Short press -> Email Summary; Long press (1.5s) -> Toggle Offline/Online Mode
    if (M5.BtnB.pressedFor(1500)) {
        wakeScreen();
        forceOfflineMode = !forceOfflineMode;
        beepTone(1000, 150);
        deviceLog(String("MODE_TOGGLE: Hold BtnB -> ") + (forceOfflineMode ? "OFFLINE MODE" : "ONLINE MODE"), !forceOfflineMode);
        if (!forceOfflineMode && WiFi.status() == WL_CONNECTED) {
            pingRequested = true;
        }
        drawScreen();
        delay(600);
    } else if (M5.BtnB.wasReleased()) {
        wakeScreen();
        beepTone(1800, 80);
        summaryRequested = true;
    }

    // Continuous Non-Blocking Audio Capture on Core 1
    int sliceRms = 0;
    size_t got = readMicSlice(i2sBuffer, DMA_READ_BYTES, &sliceRms);
    if (got > 0) {
        currentRms = sliceRms;
        if (sliceRms > peakRmsDisplay) peakRmsDisplay = sliceRms;
        else peakRmsDisplay = (peakRmsDisplay * 3 + sliceRms) / 4;

        if (isRecording) {
            bool onlineNow = (!forceOfflineMode && WiFi.status() == WL_CONNECTED);
            if (sliceRms >= VAD_RMS_THRESHOLD) {
                lastSpeechMillis = millis();
            }
            bool shouldCapture = onlineNow ||
                                 ((lastSpeechMillis > 0) && ((millis() - lastSpeechMillis) <= VAD_HANGOVER_MS));

            if (shouldCapture) {
                if (sliceRms > activeCapturePeakRms) activeCapturePeakRms = sliceRms;
                size_t encodedLen = encodePcm16ToAdpcm8k(i2sBuffer, got / 2, adpcmSliceBuffer, liveAdpcmState);
                if (activeCaptureBytes + encodedLen > CLIP_BUFFER_BYTES) {
                    // Current 8s buffer is full! Hand it to Core 0 uploader and immediately continue in the other buffer!
                    flushAndSwapCaptureBuffer(false);
                }
                memcpy(pingPongClips[activeCaptureBuf] + activeCaptureBytes, adpcmSliceBuffer, encodedLen);
                activeCaptureBytes += encodedLen;
            }
        }
    }

    if (millis() - lastUiRefreshMillis > 250) {
        lastUiRefreshMillis = millis();
        drawScreen();
    }

    if (!forceOfflineMode && wifiSsid.length() > 0 && serverHost.length() > 0 && (millis() - lastPingMillis > 20000)) {
        lastPingMillis = millis();
        pingRequested = true;
    }
}
