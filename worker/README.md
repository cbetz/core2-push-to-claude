# Push-to-Claude Worker

A small Cloudflare Worker that turns the Cardputer into a voice + text
chat client for Claude. The device records WAV audio (or types text)
and POSTs it here; the Worker runs Whisper for STT, calls Claude
Haiku 4.5 with the last few turns of conversation context, and returns
a reply for the device to render on its 240×135 LCD.

```
Cardputer-Adv ──► Cloudflare Worker ──► OpenAI Whisper (STT)
                          │
                          └──────────► Anthropic /v1/messages (Claude)
                          │
                          └─ Workers KV: per-device 8-message history (24h TTL)
```

## Endpoints

| Method | Path        | Body            | Returns                       |
| ------ | ----------- | --------------- | ----------------------------- |
| `POST` | `/ask`      | raw WAV audio   | `{ transcript, response }`    |
| `POST` | `/ask-text` | JSON `{prompt}` | `{ transcript, response }`    |
| `POST` | `/reset`    | empty           | `{ ok: true, cleared: true }` |
| `GET`  | `/`         | —               | health probe                  |

All write endpoints require an `x-device-secret` header that matches
the Worker's `DEVICE_SECRET` secret.

## One-time setup

You'll need:

- A Cloudflare account (free tier is fine for this volume)
- An [Anthropic API key](https://console.anthropic.com/)
- An [OpenAI API key](https://platform.openai.com/api-keys) (for Whisper STT — only needed if you want voice; you can skip if you only use `/ask-text`)
- Node.js 18+ on your laptop

### 1. Install Wrangler and log in

```bash
cd worker
npm install
npx wrangler login
```

### 2. Create a KV namespace for conversation history

```bash
npx wrangler kv namespace create HISTORY
```

Wrangler prints something like:

```
[[kv_namespaces]]
binding = "HISTORY"
id = "abc123def456..."
```

Copy the `id` into `worker/wrangler.toml`, replacing `REPLACE_WITH_YOUR_KV_NAMESPACE_ID`.

### 3. Set the secrets

```bash
npx wrangler secret put ANTHROPIC_API_KEY   # paste your Anthropic key
npx wrangler secret put OPENAI_API_KEY      # paste your OpenAI key
npx wrangler secret put DEVICE_SECRET       # paste any random 32+ char string
```

Generate a `DEVICE_SECRET` with:

```bash
openssl rand -base64 32
```

Save the same `DEVICE_SECRET` — you'll paste it into the device config in the next section.

### 4. Deploy

```bash
npx wrangler deploy
```

Wrangler prints your Worker URL, e.g.
`https://push-to-claude.<your-subdomain>.workers.dev`. Save that too.

### 5. Point the device at your Worker

On your laptop, in the cloned repo:

```bash
cp buddy/device/apps/config.example.py buddy/device/apps/config.py
```

Edit `buddy/device/apps/config.py`:

```python
WORKER_BASE = "https://push-to-claude.<your-subdomain>.workers.dev"
DEVICE_SECRET = "<the same DEVICE_SECRET you put on the Worker>"
```

Then push the apps to the Cardputer:

```bash
python3 .claude/skills/m5-onboard/scripts/install_apps.py --port <PORT> --src buddy
```

Boot the device → pick **Push to Claude** from the launcher → tap SPACE to start recording.

## Local development

```bash
npx wrangler dev
```

Wrangler boots a local proxy at `http://127.0.0.1:8787` with live reload.
For local secrets, create `worker/.dev.vars` (gitignored):

```
ANTHROPIC_API_KEY=sk-ant-...
OPENAI_API_KEY=sk-...
DEVICE_SECRET=...
```

## Tail production logs

```bash
npx wrangler tail
```

## Cost notes

- **Whisper** (`whisper-1`) is $0.006 / minute of audio. The device caps
  recordings at 6 s, so each `/ask` is ~$0.0006.
- **Claude Haiku 4.5** is around $1 / MTok input, $5 / MTok output as of
  this writing. With a 250-token output cap and short prompts, each turn
  is well under a cent.
- **Workers** free tier: 100k requests/day. **KV** free tier: 100k
  reads/day, 1k writes/day. Plenty for personal use.

## Privacy

Conversation history is stored in Workers KV, keyed by `DEVICE_SECRET`,
with a 24-hour TTL. Hit `POST /reset` (the launcher binds this to a key
combo on the device) to clear it sooner. Whisper transcripts are not
stored anywhere by this Worker — they pass through to Claude and back.

Anthropic and OpenAI's data-retention policies apply to whatever you
send them. Read theirs.
