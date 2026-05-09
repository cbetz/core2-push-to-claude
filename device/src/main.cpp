// Push-to-Claude on the M5Stack Core2.
//
// Tap A to record up to 6s of audio, then we POST it as a WAV body
// to the Cloudflare Worker (vendored under ../../worker), which runs
// Whisper for STT and Claude Haiku 4.5 for the reply. The result
// renders on the LCD. Tap A again to ask another question — the
// Worker keeps the last few turns in KV per device-secret. Tap C to
// clear that memory (POST /reset). Tap B during recording to abort.
//
// State machine:
//   Idle      → A          → Recording
//   Recording → 6s elapsed  → Uploading
//   Recording → B           → Idle
//   Uploading → 200         → Showing
//   Uploading → !200        → Error
//   Showing   → A           → Idle
//   Error     → A           → Idle

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "config.h"

// ----- Theme (matches the upstream Cardputer port) ----------------
static constexpr uint32_t COL_BLACK  = 0x000000;
static constexpr uint32_t COL_ORANGE = 0xCC785C;
static constexpr uint32_t COL_CREAM  = 0xF0EEE6;
static constexpr uint32_t COL_DARK   = 0x1F1F1F;
static constexpr uint32_t COL_GRAY   = 0x777777;
static constexpr uint32_t COL_GREEN  = 0x00FF00;
static constexpr uint32_t COL_RED    = 0xFF0000;

// ----- Audio constants --------------------------------------------
// 16 kHz / 16-bit signed / mono — what Whisper and the upstream
// Worker expect. Don't change without updating the Worker too.
static constexpr uint32_t SAMPLE_RATE  = 16000;
static constexpr uint32_t MAX_SECONDS  = 6;
static constexpr size_t   SAMPLE_COUNT = SAMPLE_RATE * MAX_SECONDS;
static constexpr size_t   PCM_BYTES    = SAMPLE_COUNT * sizeof(int16_t);
static constexpr size_t   WAV_HEADER   = 44;

// ----- Layout (320x240 in landscape) ------------------------------
static constexpr int SCREEN_W   = 320;
static constexpr int SCREEN_H   = 240;
static constexpr int CHROME_TOP = 24;
static constexpr int CHROME_BOT = 24;

enum class Mode { Idle, Recording, Uploading, Showing, Error };

static Mode    mode             = Mode::Idle;
static int16_t *audio_buf       = nullptr;
static String  last_transcript;
static String  last_reply;
static String  last_error;
static bool    wifi_ok          = false;

// ---- UI helpers --------------------------------------------------
static void drawChrome(const char *title, const char *hint) {
    auto &d = M5.Display;
    d.fillScreen(COL_BLACK);
    d.fillRect(0, 0, SCREEN_W, CHROME_TOP, COL_DARK);
    d.fillRect(0, CHROME_TOP, SCREEN_W, 1, COL_ORANGE);
    d.setTextSize(1);
    d.setTextColor(COL_ORANGE, COL_DARK);
    d.drawString(title, 8, 6);

    d.fillRect(0, SCREEN_H - CHROME_BOT, SCREEN_W, CHROME_BOT, COL_DARK);
    d.setTextColor(COL_GRAY, COL_DARK);
    int hw = d.textWidth(hint);
    d.drawString(hint, (SCREEN_W - hw) / 2, SCREEN_H - CHROME_BOT + 7);
}

static void drawCentered(const char *s, int y, uint32_t color, uint8_t size) {
    auto &d = M5.Display;
    d.setTextSize(size);
    d.setTextColor(color, COL_BLACK);
    int w = d.textWidth(s);
    d.drawString(s, (SCREEN_W - w) / 2, y);
}

// Greedy word-wrap. Returns the y of the next free line.
static int drawWrapped(const String &text, int x, int y, int max_w,
                       int line_h, uint32_t color, uint8_t size = 1) {
    auto &d = M5.Display;
    d.setTextSize(size);
    d.setTextColor(color, COL_BLACK);

    String cur;
    int idx = 0;
    int n = text.length();
    while (idx < n) {
        int sp = text.indexOf(' ', idx);
        String word = (sp < 0) ? text.substring(idx) : text.substring(idx, sp);
        String cand = cur.length() ? cur + " " + word : word;
        if ((int)d.textWidth(cand.c_str()) <= max_w) {
            cur = cand;
        } else {
            if (cur.length()) {
                d.drawString(cur.c_str(), x, y);
                y += line_h;
                cur = "";
            }
            // Word longer than one line — hard-cut.
            while ((int)d.textWidth(word.c_str()) > max_w && word.length() > 1) {
                int cut = word.length();
                while (cut > 1 &&
                       (int)d.textWidth(word.substring(0, cut).c_str()) > max_w) {
                    cut--;
                }
                d.drawString(word.substring(0, cut).c_str(), x, y);
                y += line_h;
                word = word.substring(cut);
            }
            cur = word;
        }
        idx = (sp < 0) ? n : sp + 1;
    }
    if (cur.length()) {
        d.drawString(cur.c_str(), x, y);
        y += line_h;
    }
    return y;
}

static void drawIdle() {
    drawChrome("Push to Claude", "A ask    C reset memory");
    drawCentered("Ask Claude", 70, COL_CREAM, 3);
    drawCentered("Tap A to record", 130, COL_GRAY, 1);
    drawCentered(wifi_ok ? "WiFi: online" : "WiFi: OFFLINE",
                 170, wifi_ok ? COL_GREEN : COL_RED, 1);
}

static void drawRecordingFrame() {
    drawChrome("Recording", "B cancel");
    drawCentered("Recording", 60, COL_ORANGE, 3);
    drawCentered("speak now (6s)", 130, COL_GRAY, 1);
}

static void drawRecordingPulse(uint32_t phase) {
    static constexpr int N  = 5;
    static constexpr int R  = 7;
    static constexpr int SP = 28;
    int total_w = (N - 1) * SP;
    int x0 = (SCREEN_W - total_w) / 2;
    int y  = 175;
    auto &d = M5.Display;
    d.fillRect(x0 - R, y - R, total_w + 2 * R, 2 * R + 2, COL_BLACK);
    for (int i = 0; i < N; i++) {
        bool on = (phase % N) == (uint32_t)i;
        d.fillCircle(x0 + i * SP, y, R, on ? COL_ORANGE : COL_DARK);
    }
}

static void drawUploading(const char *msg) {
    drawChrome("Uploading", "");
    drawCentered("Thinking", 80, COL_CREAM, 3);
    drawCentered(msg, 150, COL_GRAY, 1);
}

static void drawShowing() {
    drawChrome("Claude says", "A ask again");
    int y = CHROME_TOP + 8;
    if (last_transcript.length()) {
        y = drawWrapped(String("> ") + last_transcript, 10, y,
                        SCREEN_W - 20, 12, COL_GRAY, 1);
        y += 6;
    }
    drawWrapped(last_reply, 10, y, SCREEN_W - 20, 18, COL_CREAM, 2);
}

static void drawError() {
    drawChrome("Error", "A retry");
    drawCentered("Something went wrong", 60, COL_RED, 2);
    drawWrapped(last_error, 10, 110, SCREEN_W - 20, 12, COL_CREAM, 1);
}

// ---- WiFi --------------------------------------------------------
static bool connectWifi(uint32_t timeout_ms) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) {
        delay(200);
    }
    wifi_ok = (WiFi.status() == WL_CONNECTED);
    return wifi_ok;
}

// ---- WAV header --------------------------------------------------
// 44-byte canonical PCM header for 16 kHz / 16-bit / mono.
static void buildWavHeader(uint8_t h[WAV_HEADER], uint32_t pcm_bytes) {
    auto put32 = [&](int o, uint32_t v) {
        h[o]     =  v        & 0xFF;
        h[o + 1] = (v >> 8)  & 0xFF;
        h[o + 2] = (v >> 16) & 0xFF;
        h[o + 3] = (v >> 24) & 0xFF;
    };
    auto put16 = [&](int o, uint16_t v) {
        h[o]     =  v        & 0xFF;
        h[o + 1] = (v >> 8)  & 0xFF;
    };
    memcpy(h + 0, "RIFF", 4);
    put32(4, 36 + pcm_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, SAMPLE_RATE);
    put32(28, SAMPLE_RATE * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(h + 36, "data", 4);
    put32(40, pcm_bytes);
}

// ---- Forward decls -----------------------------------------------
static void doUpload();

// ---- Recording ---------------------------------------------------
static void doRecord() {
    mode = Mode::Recording;
    drawRecordingFrame();

    if (!M5.Mic.record(audio_buf, SAMPLE_COUNT, SAMPLE_RATE, false)) {
        last_error = "M5.Mic.record() failed to start";
        mode = Mode::Error;
        drawError();
        return;
    }

    uint32_t t0 = millis();
    uint32_t phase = (uint32_t)-1;
    while (M5.Mic.isRecording()) {
        M5.update();
        if (M5.BtnB.wasPressed()) {
            M5.Mic.end();
            M5.Mic.begin();
            mode = Mode::Idle;
            drawIdle();
            return;
        }
        uint32_t p = (millis() - t0) / 200;
        if (p != phase) {
            phase = p;
            drawRecordingPulse(phase);
        }
        delay(20);
    }
    doUpload();
}

// ---- Upload to /ask ----------------------------------------------
static void doUpload() {
    mode = Mode::Uploading;
    drawUploading("uploading audio...");

    if (WiFi.status() != WL_CONNECTED && !connectWifi(8000)) {
        last_error = "WiFi disconnected; reconnect timed out";
        mode = Mode::Error;
        drawError();
        return;
    }

    size_t total = WAV_HEADER + PCM_BYTES;
    uint8_t *upload_buf = (uint8_t *)ps_malloc(total);
    if (!upload_buf) {
        last_error = "out of memory for upload buffer";
        mode = Mode::Error;
        drawError();
        return;
    }
    buildWavHeader(upload_buf, PCM_BYTES);
    memcpy(upload_buf + WAV_HEADER, audio_buf, PCM_BYTES);

    WiFiClientSecure client;
    client.setInsecure();   // TODO: pin Cloudflare CA before public release.
    HTTPClient http;
    http.begin(client, String(WORKER_BASE) + "/ask");
    http.addHeader("Content-Type", "audio/wav");
    http.addHeader("x-device-secret", DEVICE_SECRET);
    http.setTimeout(30000);

    drawUploading("waiting for Claude...");
    int code = http.POST(upload_buf, total);
    String body = http.getString();
    http.end();
    free(upload_buf);

    if (code <= 0) {
        last_error = String("HTTP error: ") + http.errorToString(code);
        mode = Mode::Error;
        drawError();
        return;
    }
    if (code != 200) {
        last_error = String("HTTP ") + code + ": " + body.substring(0, 200);
        mode = Mode::Error;
        drawError();
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        last_error = String("bad JSON: ") + err.c_str();
        mode = Mode::Error;
        drawError();
        return;
    }

    const char *t = doc["transcript"].is<const char *>() ? doc["transcript"].as<const char *>() : "";
    const char *r = doc["response"].is<const char *>()   ? doc["response"].as<const char *>()   : "";
    last_transcript = t ? t : "";
    last_reply      = r ? r : "";

    if (!last_reply.length()) {
        const char *e = doc["error"].is<const char *>() ? doc["error"].as<const char *>() : "empty response";
        last_error = String("worker: ") + e;
        mode = Mode::Error;
        drawError();
        return;
    }

    mode = Mode::Showing;
    drawShowing();
}

// ---- /reset ------------------------------------------------------
static void doReset() {
    drawUploading("clearing memory...");
    if (WiFi.status() != WL_CONNECTED && !connectWifi(8000)) {
        mode = Mode::Idle;
        drawIdle();
        return;
    }
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, String(WORKER_BASE) + "/reset");
    http.addHeader("x-device-secret", DEVICE_SECRET);
    http.POST((uint8_t *)nullptr, 0);
    http.end();
    mode = Mode::Idle;
    drawIdle();
}

// ---- Arduino entry points ----------------------------------------
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);
    M5.Display.setBrightness(160);
    M5.Display.fillScreen(COL_BLACK);

    // Core2 shares I2S between speaker and mic — release the speaker
    // so M5.Mic can claim the bus.
    M5.Speaker.end();
    M5.Mic.begin();

    drawCentered("booting...", 100, COL_CREAM, 2);

    audio_buf = (int16_t *)ps_malloc(PCM_BYTES);
    if (!audio_buf) {
        last_error = "PSRAM allocation failed";
        mode = Mode::Error;
        drawError();
        return;
    }

    drawCentered("connecting WiFi...", 130, COL_GRAY, 1);
    connectWifi(15000);
    drawIdle();
}

void loop() {
    M5.update();
    switch (mode) {
        case Mode::Idle:
            if (M5.BtnA.wasPressed())      doRecord();
            else if (M5.BtnC.wasPressed()) doReset();
            break;
        case Mode::Showing:
        case Mode::Error:
            if (M5.BtnA.wasPressed()) {
                mode = Mode::Idle;
                drawIdle();
            }
            break;
        default:
            // Recording and Uploading drive their own inner loops.
            break;
    }
    delay(10);
}
