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

// ─── Display, Brightness & Customizable Watch Face Settings (NVS "ui") ────────
// screenMode: 0 = Audio Logger, 1 = Watch Face, 2 = Screen Off (Stealth)
uint8_t screenMode = 0;
uint8_t lcdBrightness = 85;        // 10..100% (M5StickCPlus AXP192 ScreenBreath scale)
bool autoDimEnabled = true;
bool watchFlip = false;            // false = Rotation 1, true = Rotation 3 (180 deg wrist flip)
uint8_t watchStyle = 0;            // 0 = Digital Bold, 1 = Minimal Stealth, 2 = Analog+Digital, 3 = Retro 7-Seg
uint8_t watchAccentIdx = 0;        // 0..5 accent colors
bool watch24h = false;
bool watchShowSeconds = true;
bool watchShowDate = true;
bool watchShowBattery = true;
bool watchShowRecDot = true;
bool watchLockRec = false;         // Lock BtnA against pocket presses while in Watch Face mode
String watchLabel = "M5 AUDIO";

uint32_t lastAppliedCloudCmdSeq = 0;
volatile bool uiApplyRequested = false;
volatile bool uiSaveRequested = false;
String uiToastText = "";
uint32_t uiToastUntil = 0;
uint32_t lastAxpBtnPollMillis = 0;
volatile float cachedBatVoltage = 4.10f;
volatile bool cachedUsbPowered = false;

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
    float vbat = cachedBatVoltage;
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

// ─── Sound / LED / Display / Power Helpers ───────────────────────────────────
static const uint16_t WATCH_ACCENTS[6] = {
    0x07FF, // 0: Cyan
    0x262B, // 1: Mint Green
    0xFD20, // 2: Amber Gold
    0xF9EB, // 3: Coral Red
    0xAA9E, // 4: Violet
    0xFFFF  // 5: Ice White
};

uint16_t getWatchAccentColor() {
    return WATCH_ACCENTS[constrain((int)watchAccentIdx, 0, 5)];
}

int calcWeekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[constrain(m, 1, 12) - 1] + d) % 7;
}

String sanitizeWatchLabel(const String& raw) {
    String out;
    for (size_t i = 0; i < raw.length() && out.length() < 16; i++) {
        char c = raw[i];
        if (c >= 32 && c <= 126 && c != '<' && c != '>' && c != '"' && c != '\'') {
            out += c;
        }
    }
    out.trim();
    return out;
}

void saveUiSettingsToNvs() {
    if (!prefs.begin("ui", false)) return;
    if (prefs.getUChar("mode", 255) != screenMode) prefs.putUChar("mode", screenMode);
    if (prefs.getUChar("bright", 255) != lcdBrightness) prefs.putUChar("bright", lcdBrightness);
    if (prefs.getBool("adim", !autoDimEnabled) != autoDimEnabled) prefs.putBool("adim", autoDimEnabled);
    if (prefs.getBool("flip", !watchFlip) != watchFlip) prefs.putBool("flip", watchFlip);
    if (prefs.getUChar("wstyle", 255) != watchStyle) prefs.putUChar("wstyle", watchStyle);
    if (prefs.getUChar("wcol", 255) != watchAccentIdx) prefs.putUChar("wcol", watchAccentIdx);
    if (prefs.getBool("w24", !watch24h) != watch24h) prefs.putBool("w24", watch24h);
    if (prefs.getBool("wsec", !watchShowSeconds) != watchShowSeconds) prefs.putBool("wsec", watchShowSeconds);
    if (prefs.getBool("wdate", !watchShowDate) != watchShowDate) prefs.putBool("wdate", watchShowDate);
    if (prefs.getBool("wbat", !watchShowBattery) != watchShowBattery) prefs.putBool("wbat", watchShowBattery);
    if (prefs.getBool("wdot", !watchShowRecDot) != watchShowRecDot) prefs.putBool("wdot", watchShowRecDot);
    if (prefs.getBool("wlock", !watchLockRec) != watchLockRec) prefs.putBool("wlock", watchLockRec);
    if (prefs.getString("wtag", "\x01") != watchLabel) prefs.putString("wtag", watchLabel);
    prefs.end();
}

void loadUiSettingsFromNvs() {
    if (!prefs.begin("ui", true)) {
        saveUiSettingsToNvs();
        return;
    }
    screenMode = constrain((int)prefs.getUChar("mode", 0), 0, 2);
    lcdBrightness = constrain((int)prefs.getUChar("bright", 85), 10, 100);
    autoDimEnabled = prefs.getBool("adim", true);
    watchFlip = prefs.getBool("flip", false);
    watchStyle = constrain((int)prefs.getUChar("wstyle", 0), 0, 3);
    watchAccentIdx = constrain((int)prefs.getUChar("wcol", 0), 0, 5);
    watch24h = prefs.getBool("w24", false);
    watchShowSeconds = prefs.getBool("wsec", true);
    watchShowDate = prefs.getBool("wdate", true);
    watchShowBattery = prefs.getBool("wbat", true);
    watchShowRecDot = prefs.getBool("wdot", true);
    watchLockRec = prefs.getBool("wlock", false);
    if (prefs.isKey("wtag")) {
        watchLabel = sanitizeWatchLabel(prefs.getString("wtag", "M5 AUDIO"));
    }
    prefs.end();
}

void applyDisplayHardware() {
    M5.Lcd.setRotation(watchFlip ? 3 : 1);
    if (screenMode == 2) {
        M5.Axp.SetLDO2(false);
        screenDimmed = true;
    } else {
        M5.Axp.SetLDO2(true);
        uint8_t targetBright = screenDimmed ? (uint8_t)min((int)lcdBrightness, 22) : lcdBrightness;
        M5.Axp.ScreenBreath(constrain((int)targetBright, 10, 100));
    }
}

void showUiToast(const String& msg, uint32_t durationMs = 1500) {
    uiToastText = msg;
    uiToastUntil = millis() + durationMs;
}

void setRedLed(bool on) {
    digitalWrite(PIN_RED_LED, on ? LOW : HIGH);
}

void refreshPowerTelemetryOnCore1() {
    float v = M5.Axp.GetBatVoltage();
    if (v > 2.0f && v < 5.0f) {
        cachedBatVoltage = v;
    }
    cachedUsbPowered = (M5.Axp.GetVBusVoltage() > 4.5f);
}

bool isUsbPowered() {
    return cachedUsbPowered;
}

void wakeScreen() {
    lastInteractionMillis = millis();
    if (screenMode == 2) return; // Respect explicit Screen Off stealth mode
    if (screenDimmed) {
        screenDimmed = false;
        applyDisplayHardware();
    }
}

void checkAutoDimScreen() {
    if (screenMode == 2) return;
    if (!autoDimEnabled || isUsbPowered()) {
        if (screenDimmed) {
            screenDimmed = false;
            applyDisplayHardware();
        }
        return;
    }
    if (!screenDimmed && (millis() - lastInteractionMillis > 15000)) {
        screenDimmed = true;
        applyDisplayHardware();
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
void drawLoggerScreen() {
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
    int batPct = constrain((int)((vbat - 3.3f) / (4.15f - 3.3f) * 100.0f), 0, 100);
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
    canvas.drawString("M5:Rec  Side:Email/Net  Pwr:Watch/Bright", 6, 124, 1);
}

void drawWatchScreen() {
    canvas.fillSprite(TFT_BLACK);
    canvas.setTextSize(1);
    uint16_t accent = getWatchAccentColor();

    RTC_DateTypeDef d;
    RTC_TimeTypeDef t;
    M5.Rtc.GetDate(&d);
    M5.Rtc.GetTime(&t);
    bool timeValid = (d.Year >= 2025 && d.Year <= 2035);

    float vbat = M5.Axp.GetBatVoltage();
    int batPct = constrain((int)((vbat - 3.3f) / (4.15f - 3.3f) * 100.0f), 0, 100);
    bool usbOn = isUsbPowered();
    char batBuf[16];
    if (usbOn) snprintf(batBuf, sizeof(batBuf), "USB %d%%", batPct);
    else snprintf(batBuf, sizeof(batBuf), "%d%%", batPct);

    int dispHr = t.Hours;
    const char* ampm = "";
    if (!watch24h) {
        ampm = (t.Hours >= 12) ? "PM" : "AM";
        dispHr = t.Hours % 12;
        if (dispHr == 0) dispHr = 12;
    }

    char hmStr[8] = "--:--";
    char secStr[6] = "";
    char dateBuf[24] = "Syncing Clock...";
    static const char* WDAYS[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* MONTHS[13] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    if (timeValid) {
        if (watch24h) snprintf(hmStr, sizeof(hmStr), "%02d:%02d", dispHr, t.Minutes);
        else snprintf(hmStr, sizeof(hmStr), "%d:%02d", dispHr, t.Minutes);
        snprintf(secStr, sizeof(secStr), "%02d", t.Seconds);
        int wd = calcWeekday(d.Year, d.Month, d.Date);
        int mo = constrain((int)d.Month, 1, 12);
        snprintf(dateBuf, sizeof(dateBuf), "%s, %02d %s %04d", WDAYS[wd], d.Date, MONTHS[mo], d.Year);
    }

    bool recBlink = ((millis() / 600) % 2 == 0);
    bool showDot = (isRecording && watchShowRecDot);

    if (watchStyle == 1) {
        // Style 1: Minimal Stealth
        if (showDot) {
            if (recBlink) canvas.fillCircle(12, 12, 4, forceOfflineMode ? TFT_ORANGE : TFT_RED);
            else canvas.drawCircle(12, 12, 4, forceOfflineMode ? TFT_ORANGE : TFT_RED);
        }
        if (watchLabel.length() > 0) {
            canvas.setTextColor(0x7BEF, TFT_BLACK);
            canvas.drawString(watchLabel, showDot ? 22 : 8, 6, 1);
        }
        if (watchShowBattery) {
            canvas.setTextColor(usbOn ? 0x07E0 : (batPct > 20 ? 0x9CF3 : TFT_RED), TFT_BLACK);
            canvas.drawRightString(batBuf, 232, 6, 1);
        }
        if (timeValid) {
            canvas.setTextColor(TFT_WHITE, TFT_BLACK);
            canvas.drawCentreString(hmStr, 120, 32, 6);
            if (watchShowSeconds || !watch24h) {
                char subT[16];
                if (watchShowSeconds && !watch24h) snprintf(subT, sizeof(subT), ":%s %s", secStr, ampm);
                else if (watchShowSeconds) snprintf(subT, sizeof(subT), ":%s", secStr);
                else snprintf(subT, sizeof(subT), "%s", ampm);
                canvas.setTextColor(accent, TFT_BLACK);
                canvas.drawCentreString(subT, 120, 84, 2);
            }
            if (watchShowDate) {
                canvas.setTextColor(0xBDF7, TFT_BLACK);
                canvas.drawCentreString(dateBuf, 120, 105, 2);
            }
            if (watchShowSeconds) {
                int barW = (t.Seconds * 232) / 59;
                canvas.fillRect(4, 130, barW, 3, accent);
            }
        } else {
            canvas.setTextColor(accent, TFT_BLACK);
            canvas.drawCentreString("Syncing Clock...", 120, 56, 2);
        }
    } else if (watchStyle == 2) {
        // Style 2: Analog Dial + Digital Sidebar
        int cx = 64, cy = 67, rad = 54;
        canvas.drawCircle(cx, cy, rad, accent);
        canvas.drawCircle(cx, cy, rad - 1, 0x2104);
        for (int a = 0; a < 12; a++) {
            float radAng = (a * 30.0f - 90.0f) * 0.01745329f;
            int rIn = (a % 3 == 0) ? rad - 9 : rad - 5;
            canvas.drawLine(cx + (int)(cosf(radAng) * rIn), cy + (int)(sinf(radAng) * rIn),
                            cx + (int)(cosf(radAng) * (rad - 2)), cy + (int)(sinf(radAng) * (rad - 2)),
                            (a % 3 == 0) ? TFT_WHITE : 0x7BEF);
        }
        if (timeValid) {
            float hrAng = (((t.Hours % 12) + t.Minutes / 60.0f) * 30.0f - 90.0f) * 0.01745329f;
            float minAng = ((t.Minutes + t.Seconds / 60.0f) * 6.0f - 90.0f) * 0.01745329f;
            float secAng = (t.Seconds * 6.0f - 90.0f) * 0.01745329f;
            int hx = cx + (int)(cosf(hrAng) * 28), hy = cy + (int)(sinf(hrAng) * 28);
            int mx = cx + (int)(cosf(minAng) * 42), my = cy + (int)(sinf(minAng) * 42);
            canvas.drawLine(cx, cy, hx, hy, TFT_WHITE);
            canvas.drawLine(cx + 1, cy, hx + 1, hy, TFT_WHITE);
            canvas.drawLine(cx, cy, mx, my, 0xCE79);
            if (watchShowSeconds) {
                canvas.drawLine(cx, cy, cx + (int)(cosf(secAng) * 46), cy + (int)(sinf(secAng) * 46), accent);
            }
        }
        canvas.fillCircle(cx, cy, 4, accent);

        int rx = 128;
        if (showDot) {
            if (recBlink) canvas.fillCircle(rx + 4, 14, 4, forceOfflineMode ? TFT_ORANGE : TFT_RED);
            else canvas.drawCircle(rx + 4, 14, 4, forceOfflineMode ? TFT_ORANGE : TFT_RED);
        }
        if (watchLabel.length() > 0) {
            canvas.setTextColor(accent, TFT_BLACK);
            canvas.drawString(watchLabel.substring(0, 10), showDot ? rx + 13 : rx, 7, 2);
        }
        if (timeValid) {
            canvas.setTextColor(TFT_WHITE, TFT_BLACK);
            canvas.drawString(hmStr, rx, 36, 4);
            char subRight[16] = "";
            if (watchShowSeconds && !watch24h) snprintf(subRight, sizeof(subRight), ":%s %s", secStr, ampm);
            else if (watchShowSeconds) snprintf(subRight, sizeof(subRight), ":%s", secStr);
            else if (!watch24h) snprintf(subRight, sizeof(subRight), "%s", ampm);
            if (subRight[0]) {
                canvas.setTextColor(accent, TFT_BLACK);
                canvas.drawString(subRight, rx, 66, 2);
            }
            if (watchShowDate) {
                int wd = calcWeekday(d.Year, d.Month, d.Date);
                int mo = constrain((int)d.Month, 1, 12);
                char shortDate[18];
                snprintf(shortDate, sizeof(shortDate), "%s %02d %s", WDAYS[wd], d.Date, MONTHS[mo]);
                canvas.setTextColor(0xBDF7, TFT_BLACK);
                canvas.drawString(shortDate, rx, 88, 2);
            }
        } else {
            canvas.setTextColor(0xBDF7, TFT_BLACK);
            canvas.drawString("Syncing...", rx, 48, 2);
        }
        if (watchShowBattery) {
            canvas.setTextColor(usbOn ? 0x07E0 : 0x9CF3, TFT_BLACK);
            canvas.drawString(String(batBuf) + (watchLockRec ? " LOCK" : ""), rx, 112, 1);
        }
    } else if (watchStyle == 3) {
        // Style 3: Retro 7-Segment LED Clock
        canvas.fillRect(0, 0, 240, 20, 0x0841);
        if (showDot) {
            if (recBlink) canvas.fillCircle(10, 10, 4, forceOfflineMode ? TFT_ORANGE : TFT_RED);
            else canvas.drawCircle(10, 10, 4, forceOfflineMode ? TFT_ORANGE : TFT_RED);
        }
        canvas.setTextColor(accent, 0x0841);
        canvas.drawString(watchLabel.length() > 0 ? watchLabel : "7-SEG", showDot ? 20 : 6, 3, 2);
        if (watchShowBattery) {
            canvas.setTextColor(usbOn ? 0x07E0 : TFT_WHITE, 0x0841);
            canvas.drawRightString(String(batBuf) + (watchLockRec ? " [L]" : ""), 234, 6, 1);
        }
        if (timeValid) {
            canvas.setTextColor(accent, TFT_BLACK);
            if (watchShowSeconds || !watch24h) {
                canvas.drawString(hmStr, 10, 34, 7);
                if (watchShowSeconds) {
                    canvas.drawString(secStr, 174, 34, 4);
                }
                if (!watch24h) {
                    canvas.setTextColor(0xCE79, TFT_BLACK);
                    canvas.drawString(ampm, 174, 64, 2);
                }
            } else {
                canvas.drawCentreString(hmStr, 120, 34, 7);
            }
            if (watchShowDate) {
                canvas.fillRoundRect(8, 99, 224, 28, 6, 0x1082);
                canvas.setTextColor(TFT_WHITE, 0x1082);
                canvas.drawCentreString(dateBuf, 120, 105, 2);
            }
        } else {
            canvas.setTextColor(accent, TFT_BLACK);
            canvas.drawCentreString("SYNCING RTC...", 120, 56, 2);
        }
    } else {
        // Style 0: Digital Bold (Default)
        canvas.fillRect(0, 0, 240, 22, 0x10A2);
        if (showDot) {
            if (recBlink) canvas.fillCircle(11, 11, 4, forceOfflineMode ? TFT_ORANGE : TFT_RED);
            else canvas.drawCircle(11, 11, 4, forceOfflineMode ? TFT_ORANGE : TFT_RED);
        }
        String topTxt = watchLabel.length() > 0 ? watchLabel : "WATCH";
        if (watchLockRec) topTxt += " [LOCK]";
        canvas.setTextColor(TFT_WHITE, 0x10A2);
        canvas.drawString(topTxt, showDot ? 21 : 6, 4, 2);
        if (watchShowBattery) {
            canvas.setTextColor(usbOn ? 0x07E0 : (batPct > 25 ? TFT_WHITE : TFT_RED), 0x10A2);
            canvas.drawRightString(batBuf, 234, 4, 2);
        }
        if (timeValid) {
            canvas.setTextColor(TFT_WHITE, TFT_BLACK);
            if (watchShowSeconds || !watch24h) {
                canvas.drawString(hmStr, 12, 34, 6);
                if (watchShowSeconds) {
                    canvas.setTextColor(accent, TFT_BLACK);
                    canvas.drawString(":" + String(secStr), 168, 34, 4);
                }
                if (!watch24h) {
                    canvas.setTextColor(0xCE79, TFT_BLACK);
                    canvas.drawString(ampm, 174, 64, 2);
                }
            } else {
                canvas.drawCentreString(hmStr, 120, 34, 6);
            }
            if (watchShowDate) {
                canvas.fillRoundRect(8, 98, 224, 28, 6, 0x0841);
                canvas.drawRoundRect(8, 98, 224, 28, 6, accent);
                canvas.setTextColor(TFT_WHITE, 0x0841);
                canvas.drawCentreString(dateBuf, 120, 104, 2);
            }
        } else {
            canvas.setTextColor(accent, TFT_BLACK);
            canvas.drawCentreString("Waiting for RTC Sync", 120, 56, 2);
        }
    }
}

void drawScreen() {
    if (screenMode == 2) return; // Screen Off stealth mode

    if (screenMode == 1) {
        drawWatchScreen();
    } else {
        drawLoggerScreen();
    }

    if (uiToastText.length() > 0 && millis() < uiToastUntil) {
        canvas.fillRoundRect(24, 102, 192, 26, 6, 0x000F);
        canvas.drawRoundRect(24, 102, 192, 26, 6, 0x07FF);
        canvas.setTextColor(TFT_WHITE, 0x000F);
        canvas.drawCentreString(uiToastText, 120, 107, 2);
    } else if (uiToastText.length() > 0 && millis() >= uiToastUntil) {
        uiToastText = "";
    }

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

bool jsonHasKey(const String& json, const char* key) {
    String pat = String("\"") + key + "\"";
    return json.indexOf(pat) >= 0;
}

int jsonGetInt(const String& json, const char* key, int fallback) {
    String pat = String("\"") + key + "\":";
    int idx = json.indexOf(pat);
    if (idx < 0) return fallback;
    int p = idx + pat.length();
    while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
    if (p < (int)json.length() && json[p] == '"') p++;
    return json.substring(p).toInt();
}

bool jsonGetBool(const String& json, const char* key, bool fallback) {
    String pat = String("\"") + key + "\":";
    int idx = json.indexOf(pat);
    if (idx < 0) return fallback;
    int p = idx + pat.length();
    while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
    String tail = json.substring(p, min((int)json.length(), p + 6));
    if (tail.startsWith("true") || tail.startsWith("1")) return true;
    if (tail.startsWith("false") || tail.startsWith("0")) return false;
    return fallback;
}

String jsonGetStr(const String& json, const char* key, const String& fallback) {
    String pat = String("\"") + key + "\":";
    int idx = json.indexOf(pat);
    if (idx < 0) return fallback;
    int p = json.indexOf('"', idx + pat.length());
    if (p < 0) return fallback;
    int q = json.indexOf('"', p + 1);
    while (q > 0 && json[q - 1] == '\\') {
        q = json.indexOf('"', q + 1);
    }
    if (q < 0) return fallback;
    String raw = json.substring(p + 1, q);
    raw.replace("\\\"", "\"");
    raw.replace("\\\\", "\\");
    return raw;
}

void applyDisplayFromJson(const String& body) {
    if (jsonHasKey(body, "mode")) screenMode = constrain(jsonGetInt(body, "mode", screenMode), 0, 2);
    if (jsonHasKey(body, "bright")) lcdBrightness = constrain(jsonGetInt(body, "bright", lcdBrightness), 10, 100);
    if (jsonHasKey(body, "adim")) autoDimEnabled = jsonGetBool(body, "adim", autoDimEnabled);
    if (jsonHasKey(body, "flip")) watchFlip = jsonGetBool(body, "flip", watchFlip);
    if (jsonHasKey(body, "wstyle")) watchStyle = constrain(jsonGetInt(body, "wstyle", watchStyle), 0, 3);
    if (jsonHasKey(body, "wcol")) watchAccentIdx = constrain(jsonGetInt(body, "wcol", watchAccentIdx), 0, 5);
    if (jsonHasKey(body, "w24")) watch24h = jsonGetBool(body, "w24", watch24h);
    if (jsonHasKey(body, "wsec")) watchShowSeconds = jsonGetBool(body, "wsec", watchShowSeconds);
    if (jsonHasKey(body, "wdate")) watchShowDate = jsonGetBool(body, "wdate", watchShowDate);
    if (jsonHasKey(body, "wbat")) watchShowBattery = jsonGetBool(body, "wbat", watchShowBattery);
    if (jsonHasKey(body, "wdot")) watchShowRecDot = jsonGetBool(body, "wdot", watchShowRecDot);
    if (jsonHasKey(body, "wlock")) watchLockRec = jsonGetBool(body, "wlock", watchLockRec);
    if (jsonHasKey(body, "wtag")) watchLabel = sanitizeWatchLabel(jsonGetStr(body, "wtag", watchLabel));
}

String jsonEscape(const String& s) {
    String out;
    out.reserve(s.length() + 4);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '\\' || c == '"') out += '\\';
        if ((unsigned char)c >= 32) out += c;
    }
    return out;
}

String buildDisplayStateJson() {
    float vbat = M5.Axp.GetBatVoltage();
    int batPct = constrain((int)((vbat - 3.3f) / (4.15f - 3.3f) * 100.0f), 0, 100);
    return String("{\"ok\":true,\"mode\":") + String(screenMode) +
           ",\"bright\":" + String(lcdBrightness) +
           ",\"adim\":" + (autoDimEnabled ? "true" : "false") +
           ",\"flip\":" + (watchFlip ? "true" : "false") +
           ",\"wstyle\":" + String(watchStyle) +
           ",\"wcol\":" + String(watchAccentIdx) +
           ",\"w24\":" + (watch24h ? "true" : "false") +
           ",\"wsec\":" + (watchShowSeconds ? "true" : "false") +
           ",\"wdate\":" + (watchShowDate ? "true" : "false") +
           ",\"wbat\":" + (watchShowBattery ? "true" : "false") +
           ",\"wdot\":" + (watchShowRecDot ? "true" : "false") +
           ",\"wlock\":" + (watchLockRec ? "true" : "false") +
           ",\"wtag\":\"" + jsonEscape(watchLabel) + "\"" +
           ",\"rec\":" + (isRecording ? "true" : "false") +
           ",\"usb\":" + (isUsbPowered() ? "true" : "false") +
           ",\"batPct\":" + String(batPct) +
           ",\"vbat\":" + String(vbat, 2) + "}";
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
    float vbat = cachedBatVoltage;
    String path = String("/api/ping?rec=") + (isRecording ? "1" : "0") +
                  "&vbat=" + String(vbat, 2) +
                  "&usb=" + (isUsbPowered() ? "1" : "0") +
                  "&chunks=" + String(chunksSentSession) +
                  "&queued=" + String(offlineQueueCount) +
                  "&ip=" + urlEncode(WiFi.localIP().toString()) +
                  "&ssid=" + urlEncode(wifiSsid) +
                  "&mode=" + String(screenMode) +
                  "&bright=" + String(lcdBrightness) +
                  "&adim=" + (autoDimEnabled ? "1" : "0") +
                  "&flip=" + (watchFlip ? "1" : "0") +
                  "&wstyle=" + String(watchStyle) +
                  "&wcol=" + String(watchAccentIdx) +
                  "&w24=" + (watch24h ? "1" : "0") +
                  "&wsec=" + (watchShowSeconds ? "1" : "0") +
                  "&wdate=" + (watchShowDate ? "1" : "0") +
                  "&wbat=" + (watchShowBattery ? "1" : "0") +
                  "&wdot=" + (watchShowRecDot ? "1" : "0") +
                  "&wlock=" + (watchLockRec ? "1" : "0") +
                  "&wtag=" + urlEncode(watchLabel) +
                  "&dseq=" + String(lastAppliedCloudCmdSeq);
    String req = String("GET ") + path + " HTTP/1.1\r\n" +
                 "Host: " + serverHost + "\r\n";
    if (deviceToken.length() > 0) {
        req += "X-Device-Token: " + deviceToken + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    client.print(req);

    String resp;
    if (readHttpResponse(client, resp, NULL, 3500)) {
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

        int cmdIdx = resp.indexOf("\"display_cmd\":");
        if (cmdIdx >= 0) {
            int braceOpen = resp.indexOf('{', cmdIdx);
            int braceClose = (braceOpen >= 0) ? resp.indexOf('}', braceOpen) : -1;
            if (braceOpen >= 0 && braceClose > braceOpen) {
                String cmdJson = resp.substring(braceOpen, braceClose + 1);
                uint32_t cmdSeq = (uint32_t)max(0, jsonGetInt(cmdJson, "seq", 0));
                if (cmdSeq > lastAppliedCloudCmdSeq) {
                    applyDisplayFromJson(cmdJson);
                    lastAppliedCloudCmdSeq = cmdSeq;
                    uiSaveRequested = true;
                    uiApplyRequested = true;
                    pingRequested = true; // ACK new settings back immediately
                }
            }
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

// ─── Web Server (Local Display/Watch Studio + Log Viewer + NVS Portal) ───────
void sendCorsHeaders() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    httpServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpServer.sendHeader("Access-Control-Allow-Headers", "Content-Type, Accept");
    httpServer.sendHeader("Access-Control-Allow-Private-Network", "true");
}

static const char PORTAL_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>M5StickC Plus Studio &amp; Setup</title>
<style>
  :root{--bg:#0b1120;--card:#131c31;--card2:#0f172a;--b:#1e293b;--t:#f1f5f9;--m:#94a3b8;--a:#38bdf8;--g:#22c55e;}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--t);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;padding:14px;line-height:1.45}
  .wrap{max-width:740px;margin:0 auto;display:grid;gap:14px}
  .card{background:var(--card);border:1px solid var(--b);border-radius:14px;padding:16px}
  h1{margin:0 0 4px;font-size:1.25rem}
  h2{margin:0 0 12px;font-size:1.05rem;display:flex;align-items:center;justify-content:space-between;gap:8px}
  .sub{color:var(--m);font-size:.86rem;margin:0}
  .badge{font-size:.74rem;padding:3px 9px;border-radius:999px;background:rgba(56,189,248,.14);color:var(--a);border:1px solid rgba(56,189,248,.3)}
  .badge.ok{background:rgba(34,197,94,.14);color:#4ade80;border-color:rgba(34,197,94,.3)}
  .grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
  @media(max-width:560px){.grid2{grid-template-columns:1fr}}
  .seg{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:14px}
  .seg-btn{padding:10px 8px;border-radius:10px;border:1px solid var(--b);background:var(--card2);color:var(--m);font-weight:600;font-size:.86rem;cursor:pointer}
  .seg-btn.active{background:rgba(56,189,248,.18);color:#fff;border-color:var(--a)}
  .row{display:flex;align-items:center;justify-content:space-between;gap:10px;margin:10px 0}
  label.lbl{font-size:.84rem;color:#cbd5e1;font-weight:600;display:block;margin-bottom:4px}
  input[type=range]{width:100%;accent-color:var(--a);cursor:pointer;height:28px}
  .presets{display:flex;gap:6px;flex-wrap:wrap;margin:6px 0 12px}
  .chip{padding:5px 11px;border-radius:999px;border:1px solid var(--b);background:var(--card2);color:#cbd5e1;font-size:.78rem;cursor:pointer}
  .chip.active,.chip:hover{border-color:var(--a);color:#fff}
  .swatches{display:flex;gap:10px;flex-wrap:wrap;margin:6px 0 12px}
  .sw{width:30px;height:30px;border-radius:50%;border:2px solid transparent;cursor:pointer;box-shadow:0 0 0 1px rgba(255,255,255,.15)}
  .sw.active{border-color:#fff;transform:scale(1.12)}
  .chk-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px}
  @media(max-width:520px){.chk-grid{grid-template-columns:1fr}}
  .chk{display:flex;align-items:center;gap:8px;background:var(--card2);border:1px solid var(--b);padding:9px 11px;border-radius:10px;font-size:.84rem;cursor:pointer}
  .chk input{width:17px;height:17px;accent-color:var(--a);margin:0}
  input[type=text],input[type=number],input[type=password],select{width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--b);background:var(--card2);color:var(--t);font-size:.94rem;margin:0 0 10px}
  .btn{width:100%;padding:11px 14px;border-radius:10px;border:none;background:#2563eb;color:#fff;font-weight:700;font-size:.95rem;cursor:pointer}
  .btn-sec{display:inline-block;padding:9px 14px;border-radius:10px;background:var(--card2);border:1px solid var(--b);color:var(--a);text-decoration:none;font-size:.86rem;font-weight:600}
  .prev-wrap{display:flex;justify-content:center;padding:10px;background:#050811;border:1px solid var(--b);border-radius:12px;margin-bottom:12px}
  canvas{width:240px;height:135px;border-radius:6px;border:2px solid #334155;background:#000}
  .meta{font-size:.84rem;color:var(--m);display:flex;flex-wrap:wrap;gap:12px}
  .meta strong{color:var(--t)}
</style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <div style="display:flex;justify-content:space-between;align-items:flex-start;gap:10px;flex-wrap:wrap">
      <div>
        <h1>🎙️ M5StickC Plus Studio</h1>
        <p class="sub">Direct On-Device Display, Watch Face &amp; Provisioning Portal</p>
      </div>
      <a class="btn-sec" href="/logs">📄 Flash Logs</a>
    </div>
    <div class="meta" style="margin-top:12px" id="devMeta">Loading device status...</div>
  </div>

  <div class="card">
    <h2><span>🎛️ Screen &amp; Brightness Control</span><span class="badge ok" id="saveBadge">Synced</span></h2>
    <label class="lbl">Active Screen Mode (or tap Bottom-Left Power button)</label>
    <div class="seg">
      <button type="button" class="seg-btn" data-mode="0">🎙️ Audio Logger</button>
      <button type="button" class="seg-btn" data-mode="1">⌚ Watch Face</button>
      <button type="button" class="seg-btn" data-mode="2">🌙 Screen Off</button>
    </div>

    <div class="row" style="margin-bottom:2px">
      <label class="lbl" style="margin:0">Backlight Brightness</label>
      <span class="badge" id="brightLbl">85%</span>
    </div>
    <input type="range" id="brightRng" min="10" max="100" step="1" value="85">
    <div class="presets">
      <button type="button" class="chip" data-bright="25">25% Night</button>
      <button type="button" class="chip" data-bright="55">55% Indoor</button>
      <button type="button" class="chip" data-bright="85">85% Crisp</button>
      <button type="button" class="chip" data-bright="100">100% Max</button>
    </div>

    <div class="chk-grid">
      <label class="chk"><input type="checkbox" id="adimChk">Auto-dim on battery (15s)</label>
      <label class="chk"><input type="checkbox" id="flipChk">Flip screen 180&deg; (left/right wrist)</label>
    </div>
  </div>

  <div class="card">
    <h2><span>⌚ Watch Face Customizer</span><span class="badge" id="styleBadge">Preview</span></h2>
    <div class="prev-wrap"><canvas id="watchPrev" width="240" height="135"></canvas></div>

    <div class="grid2">
      <div>
        <label class="lbl">Watch Style</label>
        <select id="wstyleSel">
          <option value="0">Digital Bold</option>
          <option value="1">Minimal Stealth</option>
          <option value="2">Analog + Digital</option>
          <option value="3">Retro 7-Segment</option>
        </select>
      </div>
      <div>
        <label class="lbl">Custom Watch Label (max 16 chars)</label>
        <input type="text" id="wtagInp" maxlength="16" placeholder="M5 AUDIO">
      </div>
    </div>

    <label class="lbl">Accent Color</label>
    <div class="swatches" id="swatchBox">
      <button type="button" class="sw" data-col="0" style="background:#00e5ff" title="Cyan"></button>
      <button type="button" class="sw" data-col="1" style="background:#22c55e" title="Mint"></button>
      <button type="button" class="sw" data-col="2" style="background:#f59e0b" title="Amber"></button>
      <button type="button" class="sw" data-col="3" style="background:#f43f5e" title="Coral"></button>
      <button type="button" class="sw" data-col="4" style="background:#a855f7" title="Violet"></button>
      <button type="button" class="sw" data-col="5" style="background:#ffffff" title="White"></button>
    </div>

    <div class="chk-grid">
      <label class="chk"><input type="checkbox" id="w24Chk">24-hour format</label>
      <label class="chk"><input type="checkbox" id="wsecChk">Show seconds</label>
      <label class="chk"><input type="checkbox" id="wdateChk">Show date &amp; day</label>
      <label class="chk"><input type="checkbox" id="wbatChk">Show battery %</label>
      <label class="chk"><input type="checkbox" id="wdotChk">Subtle REC dot</label>
      <label class="chk"><input type="checkbox" id="wlockChk">Lock M5 rec btn in Watch</label>
    </div>
  </div>

  <div class="card">
    <h2><span>📶 Wi-Fi &amp; Cloud NVS Settings</span><span class="badge">Reboots</span></h2>
    <form method="POST" action="/save">
      <label class="lbl">2.4 GHz Wi-Fi SSID</label><input type="text" name="s" id="ssidInp" required>
      <label class="lbl">Wi-Fi Password</label><input type="password" name="p" placeholder="Leave blank to keep current">
      <label class="lbl">Cloud Server Host / IP</label><input type="text" name="h" id="hostInp" placeholder="e.g. your-server.example.com">
      <label class="lbl">Cloud Server Port</label><input type="number" name="pt" id="portInp" value="8765">
      <label class="lbl">Device Auth Token</label><input type="password" name="t" placeholder="Leave blank to keep current">
      <button type="submit" class="btn">Save Credentials &amp; Reboot</button>
    </form>
  </div>
</div>

<script>
const COLS = ['#00e5ff','#22c55e','#f59e0b','#f43f5e','#a855f7','#ffffff'];
let st = {mode:0,bright:85,adim:true,flip:false,wstyle:0,wcol:0,w24:false,wsec:true,wdate:true,wbat:true,wdot:true,wlock:false,wtag:'M5 AUDIO',rec:false,batPct:95,usb:true};
const $ = id => document.getElementById(id);

function drawPreview(){
  const cv = $('watchPrev'), ctx = cv.getContext('2d');
  ctx.fillStyle='#000'; ctx.fillRect(0,0,240,135);
  const acc = COLS[st.wcol] || COLS[0];
  const d = new Date();
  let hr = d.getHours(), mn = String(d.getMinutes()).padStart(2,'0'), sc = String(d.getSeconds()).padStart(2,'0');
  const ap = hr>=12?'PM':'AM';
  const dh = st.w24 ? String(hr).padStart(2,'0') : String((hr%12)||12);
  const hm = dh+':'+mn;
  const ds = d.toLocaleDateString(undefined,{weekday:'short',day:'2-digit',month:'short',year:'numeric'});
  const batTxt = st.usb?('USB '+st.batPct+'%'):(st.batPct+'%');
  if(st.wstyle == 1){
    if(st.wdot){ctx.beginPath();ctx.arc(12,12,4,0,6.3);ctx.fillStyle=st.rec?'#ef4444':'#334155';ctx.fill();}
    ctx.fillStyle='#64748b';ctx.font='10px sans-serif';ctx.textAlign='left';ctx.fillText(st.wtag||'',st.wdot?22:8,15);
    if(st.wbat){ctx.textAlign='right';ctx.fillStyle='#4ade80';ctx.fillText(batTxt,232,15);}
    ctx.fillStyle='#fff';ctx.font='bold 42px monospace';ctx.textAlign='center';ctx.fillText(hm,120,72);
    ctx.fillStyle=acc;ctx.font='13px monospace';
    ctx.fillText((st.wsec?(':'+sc+' '):'')+(!st.w24?ap:''),120,94);
    if(st.wdate){ctx.fillStyle='#94a3b8';ctx.font='13px sans-serif';ctx.fillText(ds,120,116);}
    if(st.wsec){ctx.fillStyle=acc;ctx.fillRect(4,130,(d.getSeconds()/59)*232,3);}
  } else if(st.wstyle == 2){
    const cx=64,cy=67,r=52;
    ctx.strokeStyle=acc;ctx.lineWidth=2;ctx.beginPath();ctx.arc(cx,cy,r,0,6.3);ctx.stroke();
    ctx.lineWidth=1.5;
    const hA=((hr%12)+d.getMinutes()/60)*Math.PI/6-Math.PI/2;
    const mA=(d.getMinutes()+d.getSeconds()/60)*Math.PI/30-Math.PI/2;
    const sA=d.getSeconds()*Math.PI/30-Math.PI/2;
    ctx.strokeStyle='#fff';ctx.beginPath();ctx.moveTo(cx,cy);ctx.lineTo(cx+Math.cos(hA)*28,cy+Math.sin(hA)*28);ctx.stroke();
    ctx.strokeStyle='#cbd5e1';ctx.beginPath();ctx.moveTo(cx,cy);ctx.lineTo(cx+Math.cos(mA)*40,cy+Math.sin(mA)*40);ctx.stroke();
    if(st.wsec){ctx.strokeStyle=acc;ctx.beginPath();ctx.moveTo(cx,cy);ctx.lineTo(cx+Math.cos(sA)*44,cy+Math.sin(sA)*44);ctx.stroke();}
    ctx.fillStyle=acc;ctx.font='bold 12px sans-serif';ctx.textAlign='left';ctx.fillText((st.wtag||'WATCH').slice(0,10),128,20);
    ctx.fillStyle='#fff';ctx.font='bold 24px monospace';ctx.fillText(hm,128,56);
    ctx.fillStyle=acc;ctx.font='13px monospace';ctx.fillText((st.wsec?(':'+sc+' '):'')+(!st.w24?ap:''),128,76);
    if(st.wdate){ctx.fillStyle='#cbd5e1';ctx.font='12px sans-serif';ctx.fillText(ds.slice(0,12),128,98);}
    if(st.wbat){ctx.fillStyle='#4ade80';ctx.font='11px monospace';ctx.fillText(batTxt+(st.wlock?' LOCK':''),128,118);}
  } else {
    ctx.fillStyle=st.wstyle==3?'#0a120d':'#10203a'; ctx.fillRect(0,0,240,22);
    if(st.wdot){ctx.beginPath();ctx.arc(11,11,4,0,6.3);ctx.fillStyle=st.rec?'#ef4444':'#475569';ctx.fill();}
    ctx.fillStyle='#fff';ctx.font='bold 12px sans-serif';ctx.textAlign='left';
    ctx.fillText((st.wtag||'WATCH')+(st.wlock?' [LOCK]':''),st.wdot?21:8,15);
    if(st.wbat){ctx.textAlign='right';ctx.fillStyle='#4ade80';ctx.fillText(batTxt,234,15);}
    ctx.fillStyle=(st.wstyle==3)?acc:'#fff';
    ctx.font=(st.wstyle==3)?'bold 40px monospace':'bold 42px sans-serif';
    ctx.textAlign=(st.wsec||!st.w24)?'left':'center';
    ctx.fillText(hm,(st.wsec||!st.w24)?12:120,76);
    if(st.wsec){ctx.fillStyle=acc;ctx.font='bold 20px monospace';ctx.textAlign='left';ctx.fillText(':'+sc,168,56);}
    if(!st.w24){ctx.fillStyle='#cbd5e1';ctx.font='bold 13px sans-serif';ctx.textAlign='left';ctx.fillText(ap,174,78);}
    if(st.wdate){
      ctx.fillStyle='#0b1628';ctx.fillRect(8,98,224,28);
      ctx.strokeStyle=acc;ctx.strokeRect(8,98,224,28);
      ctx.fillStyle='#fff';ctx.font='bold 13px sans-serif';ctx.textAlign='center';ctx.fillText(ds,120,117);
    }
  }
}

function syncControls(){
  document.querySelectorAll('.seg-btn').forEach(b=>b.classList.toggle('active', Number(b.dataset.mode)===Number(st.mode)));
  $('brightRng').value = st.bright;
  $('brightLbl').textContent = st.bright + '%';
  document.querySelectorAll('.chip').forEach(c=>c.classList.toggle('active', Number(c.dataset.bright)===Number(st.bright)));
  $('adimChk').checked = !!st.adim;
  $('flipChk').checked = !!st.flip;
  $('wstyleSel').value = String(st.wstyle);
  document.querySelectorAll('.sw').forEach(s=>s.classList.toggle('active', Number(s.dataset.col)===Number(st.wcol)));
  $('w24Chk').checked = !!st.w24;
  $('wsecChk').checked = !!st.wsec;
  $('wdateChk').checked = !!st.wdate;
  $('wbatChk').checked = !!st.wbat;
  $('wdotChk').checked = !!st.wdot;
  $('wlockChk').checked = !!st.wlock;
  if(document.activeElement !== $('wtagInp')) $('wtagInp').value = st.wtag || '';
  drawPreview();
}

async function pushDisplay(saveFlag=true){
  $('saveBadge').textContent = saveFlag ? 'Saving...' : 'Previewing...';
  try{
    const payload = {...st, save: !!saveFlag};
    const r = await fetch('/api/display',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});
    const d = await r.json();
    if(d.ok){ st = {...st, ...d}; $('saveBadge').textContent = saveFlag ? 'Saved NVS ✓' : 'Live ✓'; syncControls(); }
  }catch(e){ $('saveBadge').textContent = 'Offline'; }
}

async function loadInfo(){
  try{
    const r = await fetch('/api/info');
    const d = await r.json();
    st = {...st, ...d};
    $('devMeta').innerHTML = `<span>State: <strong>${d.rec?'RECORDING':'STANDBY'}</strong></span><span>Mic: <strong>${d.micHz} Hz</strong></span><span>Flash: <strong>${d.spiffsUsedKb}/${d.spiffsTotalKb} KB (${d.queued} queued)</strong></span><span>IP: <strong>${d.ip||'AP 192.168.4.1'}</strong></span>`;
    $('ssidInp').value = d.ssid || '';
    $('hostInp').value = d.host || '';
    $('portInp').value = d.port || 8765;
    syncControls();
  }catch(e){}
}

document.querySelectorAll('.seg-btn').forEach(b=>b.addEventListener('click',()=>{st.mode=Number(b.dataset.mode);syncControls();pushDisplay(true);}));
$('brightRng').addEventListener('input',e=>{st.bright=Number(e.target.value);syncControls();pushDisplay(false);});
$('brightRng').addEventListener('change',e=>{st.bright=Number(e.target.value);syncControls();pushDisplay(true);});
document.querySelectorAll('.chip').forEach(c=>c.addEventListener('click',()=>{st.bright=Number(c.dataset.bright);syncControls();pushDisplay(true);}));
$('adimChk').addEventListener('change',e=>{st.adim=e.target.checked;pushDisplay(true);});
$('flipChk').addEventListener('change',e=>{st.flip=e.target.checked;pushDisplay(true);});
$('wstyleSel').addEventListener('change',e=>{st.wstyle=Number(e.target.value);syncControls();pushDisplay(true);});
document.querySelectorAll('.sw').forEach(s=>s.addEventListener('click',()=>{st.wcol=Number(s.dataset.col);syncControls();pushDisplay(true);}));
$('w24Chk').addEventListener('change',e=>{st.w24=e.target.checked;syncControls();pushDisplay(true);});
$('wsecChk').addEventListener('change',e=>{st.wsec=e.target.checked;syncControls();pushDisplay(true);});
$('wdateChk').addEventListener('change',e=>{st.wdate=e.target.checked;syncControls();pushDisplay(true);});
$('wbatChk').addEventListener('change',e=>{st.wbat=e.target.checked;syncControls();pushDisplay(true);});
$('wdotChk').addEventListener('change',e=>{st.wdot=e.target.checked;syncControls();pushDisplay(true);});
$('wlockChk').addEventListener('change',e=>{st.wlock=e.target.checked;syncControls();pushDisplay(true);});
let tagT;
$('wtagInp').addEventListener('input',e=>{st.wtag=e.target.value.slice(0,16);drawPreview();clearTimeout(tagT);tagT=setTimeout(()=>pushDisplay(true),450);});
loadInfo();
setInterval(drawPreview,1000);
</script>
</body>
</html>)rawliteral";

bool routesStarted = false;

void setupHttpRoutes() {
    if (routesStarted) return;
    routesStarted = true;

    httpServer.onNotFound([]() {
        sendCorsHeaders();
        if (httpServer.method() == HTTP_OPTIONS) {
            httpServer.send(204);
            return;
        }
        httpServer.send(404, "text/plain", "Not found");
    });

    httpServer.on("/", HTTP_OPTIONS, []() {
        sendCorsHeaders();
        httpServer.send(204);
    });

    httpServer.on("/", HTTP_GET, []() {
        sendCorsHeaders();
        httpServer.send_P(200, "text/html", PORTAL_HTML);
    });

    httpServer.on("/api/info", HTTP_GET, []() {
        sendCorsHeaders();
        String base = buildDisplayStateJson();
        base.remove(base.length() - 1); // Trim trailing '}'
        String ipStr = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "192.168.4.1";
        base += String(",\"ssid\":\"") + jsonEscape(wifiSsid) + "\"" +
                ",\"host\":\"" + jsonEscape(serverHost) + "\"" +
                ",\"port\":" + String(serverPort) +
                ",\"ip\":\"" + jsonEscape(ipStr) + "\"" +
                ",\"micHz\":" + String(measuredMicHz) +
                ",\"spiffsUsedKb\":" + String(spiffsReady ? SPIFFS.usedBytes() / 1024 : 0) +
                ",\"spiffsTotalKb\":" + String(spiffsReady ? SPIFFS.totalBytes() / 1024 : 0) +
                ",\"queued\":" + String(offlineQueueCount) + "}";
        httpServer.send(200, "application/json", base);
    });

    httpServer.on("/api/display", HTTP_OPTIONS, []() {
        sendCorsHeaders();
        httpServer.send(204);
    });

    httpServer.on("/api/display", HTTP_GET, []() {
        sendCorsHeaders();
        httpServer.send(200, "application/json", buildDisplayStateJson());
    });

    httpServer.on("/api/display", HTTP_POST, []() {
        sendCorsHeaders();
        String plain = httpServer.arg("plain");
        plain.trim();
        bool doSave = true;
        if (plain.startsWith("{")) {
            applyDisplayFromJson(plain);
            if (jsonHasKey(plain, "save")) doSave = jsonGetBool(plain, "save", true);
        } else {
            if (httpServer.hasArg("mode")) screenMode = constrain(httpServer.arg("mode").toInt(), 0, 2);
            if (httpServer.hasArg("bright")) lcdBrightness = constrain(httpServer.arg("bright").toInt(), 10, 100);
            if (httpServer.hasArg("adim")) autoDimEnabled = (httpServer.arg("adim") == "1" || httpServer.arg("adim") == "true");
            if (httpServer.hasArg("flip")) watchFlip = (httpServer.arg("flip") == "1" || httpServer.arg("flip") == "true");
            if (httpServer.hasArg("wstyle")) watchStyle = constrain(httpServer.arg("wstyle").toInt(), 0, 3);
            if (httpServer.hasArg("wcol")) watchAccentIdx = constrain(httpServer.arg("wcol").toInt(), 0, 5);
            if (httpServer.hasArg("w24")) watch24h = (httpServer.arg("w24") == "1" || httpServer.arg("w24") == "true");
            if (httpServer.hasArg("wsec")) watchShowSeconds = (httpServer.arg("wsec") == "1" || httpServer.arg("wsec") == "true");
            if (httpServer.hasArg("wdate")) watchShowDate = (httpServer.arg("wdate") == "1" || httpServer.arg("wdate") == "true");
            if (httpServer.hasArg("wbat")) watchShowBattery = (httpServer.arg("wbat") == "1" || httpServer.arg("wbat") == "true");
            if (httpServer.hasArg("wdot")) watchShowRecDot = (httpServer.arg("wdot") == "1" || httpServer.arg("wdot") == "true");
            if (httpServer.hasArg("wlock")) watchLockRec = (httpServer.arg("wlock") == "1" || httpServer.arg("wlock") == "true");
            if (httpServer.hasArg("wtag")) watchLabel = sanitizeWatchLabel(httpServer.arg("wtag"));
            if (httpServer.hasArg("save")) doSave = (httpServer.arg("save") != "0" && httpServer.arg("save") != "false");
        }
        screenDimmed = false;
        lastInteractionMillis = millis();
        applyDisplayHardware();
        drawScreen();
        if (doSave) {
            saveUiSettingsToNvs();
            pingRequested = true;
        }
        httpServer.send(200, "application/json", buildDisplayStateJson());
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
    } else if (line.startsWith("BRIGHT:")) {
        int b = constrain(line.substring(7).toInt(), 10, 100);
        lcdBrightness = (uint8_t)b;
        if (screenMode == 2) screenMode = 0;
        screenDimmed = false;
        saveUiSettingsToNvs();
        applyDisplayHardware();
        showUiToast("Bright: " + String(lcdBrightness) + "%");
        drawScreen();
        pingRequested = true;
        Serial.printf("[UI_CFG] Brightness=%u%%\n", lcdBrightness);
    } else if (line.startsWith("MODE:")) {
        screenMode = constrain(line.substring(5).toInt(), 0, 2);
        screenDimmed = false;
        saveUiSettingsToNvs();
        applyDisplayHardware();
        showUiToast(screenMode == 1 ? "Watch Mode" : (screenMode == 0 ? "Logger Mode" : ""));
        drawScreen();
        pingRequested = true;
        Serial.printf("[UI_CFG] Mode=%u\n", screenMode);
    } else if (line.equalsIgnoreCase("STATUS")) {
        refreshOfflineQueueCount();
        Serial.printf("[STATUS] WiFi=%d SSID='%s' IP=%s CloudHost=%s:%u TokenSet=%d Cloud=%d OfflineMode=%d Rec=%d BgUp=%d Mode=%u Bright=%u Style=%u Flip=%d LockRec=%d MicHz=%lu Downsample=%d Heap=%uB Chunks=%lu Queued=%lu FlashFree=%uKB\n",
                      (int)WiFi.status(), wifiSsid.c_str(), WiFi.localIP().toString().c_str(),
                      serverHost.c_str(), serverPort, deviceToken.length() > 0 ? 1 : 0,
                      (int)cloudOnline, (int)forceOfflineMode, (int)isRecording, (int)bgUploadInProgress,
                      screenMode, lcdBrightness, watchStyle, (int)watchFlip, (int)watchLockRec,
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

    refreshPowerTelemetryOnCore1();
    loadUiSettingsFromNvs();
    applyDisplayHardware();
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

    if (uiApplyRequested) {
        uiApplyRequested = false;
        screenDimmed = false;
        lastInteractionMillis = millis();
        if (uiSaveRequested) {
            uiSaveRequested = false;
            saveUiSettingsToNvs();
        }
        applyDisplayHardware();
        showUiToast(screenMode == 1 ? "Watch Updated" : (screenMode == 0 ? "Logger Updated" : ""));
        drawScreen();
    }

    // Button A (Front M5 Button): Toggle Recording ON / OFF (optional pocket lock in Watch mode)
    if (M5.BtnA.wasPressed()) {
        wakeScreen();
        if (screenMode == 1 && watchLockRec) {
            showUiToast("Locked in Watch", 1600);
            drawScreen();
        } else if (!isRecording) {
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

    // Bottom-Left Power Button (AXP192 PEK, polled every 50ms):
    //   Short tap (0x02) -> Cycle Screen Mode: Audio Logger -> Watch Face -> Screen Off
    //   1s Hold   (0x01) -> Cycle Brightness: 40% -> 65% -> 85% -> 100%
    if (millis() - lastAxpBtnPollMillis >= 50) {
        lastAxpBtnPollMillis = millis();
        uint8_t axpBtn = M5.Axp.GetBtnPress();
        if (axpBtn == 0x02) {
            screenMode = (screenMode + 1) % 3;
            screenDimmed = false;
            lastInteractionMillis = millis();
            saveUiSettingsToNvs();
            applyDisplayHardware();
            if (screenMode == 0) showUiToast("Audio Logger");
            else if (screenMode == 1) showUiToast("Watch Face");
            drawScreen();
            pingRequested = true;
            deviceLog(String("UI_BTN: Screen Mode -> ") +
                      (screenMode == 0 ? "LOGGER" : (screenMode == 1 ? "WATCH" : "SCREEN_OFF")), !forceOfflineMode);
        } else if (axpBtn == 0x01) {
            if (screenMode == 2) {
                screenMode = 1;
            } else {
                if (lcdBrightness < 55) lcdBrightness = 65;
                else if (lcdBrightness < 80) lcdBrightness = 85;
                else if (lcdBrightness < 96) lcdBrightness = 100;
                else lcdBrightness = 40;
            }
            screenDimmed = false;
            lastInteractionMillis = millis();
            saveUiSettingsToNvs();
            applyDisplayHardware();
            showUiToast("Bright: " + String(lcdBrightness) + "%");
            drawScreen();
            pingRequested = true;
            deviceLog("UI_BTN: Brightness -> " + String(lcdBrightness) + "%", !forceOfflineMode);
        }
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
        refreshPowerTelemetryOnCore1();
        drawScreen();
    }

    if (!forceOfflineMode && wifiSsid.length() > 0 && serverHost.length() > 0 && (millis() - lastPingMillis > 20000)) {
        lastPingMillis = millis();
        pingRequested = true;
    }
}
