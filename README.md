# Push-to-Claude for M5Stack Core2

A handheld Claude voice client for the [M5Stack Core2](https://docs.m5stack.com/en/core/core2). Hold a button, speak a question, get a 1–3 sentence reply on the LCD. Conversation memory is kept for 24 hours per device so follow-ups retain context.

This is a port of the **Push-to-Claude** application from [`dakshaymehta/cardputer-claude-os`](https://github.com/dakshaymehta/cardputer-claude-os) (itself forked from [`moremas/build-with-claude`](https://github.com/moremas/build-with-claude)) — moved from MicroPython on the Cardputer to C++/Arduino on the Core2, using [M5Unified](https://github.com/m5stack/M5Unified). The Cloudflare Worker that does STT + Claude chat with conversation memory is derived from the upstream's, with one substantive change: STT runs on Cloudflare Workers AI's `@cf/openai/whisper` model instead of OpenAI's hosted Whisper API, so there's no OpenAI account or key in the loop.

```
M5Stack Core2 ──► Cloudflare Worker ──► Workers AI: @cf/openai/whisper (STT)
                         │
                         ├──────────► Anthropic /v1/messages (Claude Haiku 4.5)
                         │
                         └─ Workers KV: per-device 8-message history (24h TTL)
```

## Hardware

- **M5Stack Core2** — ESP32, 320×240 IPS touchscreen, built-in PDM mic (SPM1423), I2S speaker, 16 MB flash, 8 MB PSRAM, USB-C.

What's different from the Cardputer port:

| Aspect | Cardputer | Core2 |
|---|---|---|
| MCU | ESP32-S3 | ESP32 |
| Screen | 240×135 | 320×240 (touch) |
| Input | QWERTY keyboard | 3 capacitive buttons + touch |
| Mic | PDM (Adv variant only) | PDM, built in |
| App stack | MicroPython / UIFlow | C++ / Arduino / M5Unified |
| Text-input mode | Native keyboard | Skipped in v1 (no keyboard) |

The Core2 has no physical keyboard, so this v1 ships **voice-only** — `/ask` is wired up, `/ask-text` is reachable on the Worker but the device has no UI to drive it yet. Adding an on-screen touch keyboard is a natural follow-up.

## Related projects

There's existing work in adjacent corners of this space — none of it overlaps directly, but worth knowing about:

- **[`anthropics/claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy)** — Anthropic's official BLE companion firmware for M5StickC Plus. Shows permission prompts, recent messages, and a "desk pet" animation. Different hardware (StickC Plus vs Core2), different mode (BLE status companion vs cloud voice query), and **complementary to this project**, not a competitor. If a future version of this firmware adds a buddy-mode, it should target the BLE wire protocol in that repo's `REFERENCE.md` so it integrates natively with Claude Code Desktop / Claude Cowork.

- **[`robo8080/M5Unified_StackChan_ChatGPT`](https://github.com/robo8080/M5Unified_StackChan_ChatGPT)** (and the `_Global` multilingual fork, plus the wider [StackChan ecosystem](https://github.com/m5stack/StackChan)) — the dominant voice-assistant project on Core2. Same stack as this one (PlatformIO + Arduino + M5Unified), but uses OpenAI ChatGPT directly from the device with the API key on SD card, and is built around the StackChan servo/avatar form factor. If you want avatar + TTS + servo motion on Core2, start there; if you want Claude with cloud-relayed conversation memory and no on-device secrets, this project's a better fit.

- **[M5Stack OpenAI Voice Assistant](https://docs.m5stack.com/en/guide/realtime/openai/m5cores3)** — first-party firmware from M5Stack, but for CoreS3 only. There's no equivalent shipped for Core2 ([community thread](https://community.m5stack.com/topic/7853/m5stack-core2-ai-voice-assistant)).

As far as I can tell at the time of writing, this is the first Claude (rather than ChatGPT) voice client for M5Stack Core2. If you find earlier work that should be credited here, please open an issue.

## Repository layout

```
.
├── LICENSE                  Apache 2.0
├── NOTICE                   Attribution chain
├── device/                  PlatformIO project — runs on the Core2
│   ├── platformio.ini
│   └── src/
│       ├── main.cpp
│       └── config.h.example  (copy to config.h, fill in secrets)
└── worker/                  Derived from cardputer-claude-os (STT swapped to Workers AI)
    ├── README.md             ← deploy guide (upstream's, with the STT change called out)
    ├── package.json
    ├── wrangler.toml
    └── src/worker.js
```

## Quick start

You need three things up:

1. **A Cloudflare Worker** (Whisper + Claude relay).
2. **WiFi creds + the Worker URL + a device secret** in `device/src/config.h`.
3. **PlatformIO** to build and flash the Core2.

### 1. Deploy the Worker

Follow `worker/README.md` (it's the upstream deploy guide — Cardputer-flavored, but the Worker code is hardware-agnostic). Briefly:

```bash
cd worker
npm install
npx wrangler login
npx wrangler kv namespace create HISTORY    # paste the id into wrangler.toml
npx wrangler secret put DEVICE_SECRET       # any random string; you'll paste it on the device too
npx wrangler secret put ANTHROPIC_API_KEY
npx wrangler deploy
```

The deploy prints a URL like `https://push-to-claude.<your-subdomain>.workers.dev`. That's `WORKER_BASE` for the device.

### 2. Configure the device

```bash
cp device/src/config.h.example device/src/config.h
$EDITOR device/src/config.h
```

Fill in:

- `WIFI_SSID`, `WIFI_PASS`
- `WORKER_BASE` (the URL Wrangler printed)
- `DEVICE_SECRET` (must match what you set on the Worker)

`device/src/config.h` is gitignored.

### 3. Build and flash

```bash
cd device
pio run -t upload
pio device monitor
```

On boot the Core2 connects to WiFi and shows the idle screen. Tap **button A** (left capacitive button below the screen) to record. After ~6 s of audio the device uploads, Whisper transcribes, Claude replies, and the response renders on screen. Tap **A** again to ask another question — the Worker remembers the last few turns. Hold **button C** to clear the conversation memory (`/reset`).

## Status

v1, voice-only. Known TODOs:

- [ ] On-screen touch keyboard for `/ask-text` mode.
- [ ] Tap-to-stop recording (currently fixed 6 s).
- [ ] Speak the reply through the I2S speaker (TTS).
- [ ] BLE companion mode — should target [Anthropic's official BLE protocol](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md) (Nordic UART Service UUIDs, JSON schemas) so it integrates with Claude Code Desktop / Claude Cowork, rather than the upstream cardputer-claude-os ad-hoc protocol.
- [ ] Battery-friendly idle (deep sleep when LCD is off).

## License & attribution

Apache 2.0. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).

The Cloudflare Worker in `worker/` is derived from [`dakshaymehta/cardputer-claude-os`](https://github.com/dakshaymehta/cardputer-claude-os) with one substantive modification (STT swapped from OpenAI Whisper to Cloudflare Workers AI Whisper); the change is documented in [`NOTICE`](NOTICE) and at the top of `worker/src/worker.js` and `worker/README.md`. All other Worker logic — auth, conversation memory, wire format — is the upstream authors' work. The Core2 device port under `device/` is original to this repo.
