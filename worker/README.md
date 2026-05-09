# Push-to-Claude Worker

> **Modified from upstream.** This file documents the Worker as deployed
> in this repo. The `/ask` handler's STT step has been switched from
> OpenAI Whisper to Cloudflare Workers AI's `@cf/openai/whisper` model,
> so there's no `OPENAI_API_KEY` step below. Everything else is the
> upstream `cardputer-claude-os` Worker.

A small Cloudflare Worker that exposes a voice + text chat endpoint for
a hardware client. The device records WAV audio (or types text) and
POSTs it here; the Worker transcribes the audio with Whisper (running
on Workers AI), calls Claude Haiku 4.5 with the last few turns of
conversation context, and returns a reply.

```
Device ──► Cloudflare Worker ──► Workers AI: @cf/openai/whisper (STT)
                  │
                  ├──────────► Anthropic /v1/messages (Claude)
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

- A Cloudflare account (free tier is fine; Workers AI is billed by "neurons" with a daily free allowance — see _Cost notes_ below)
- An [Anthropic API key](https://console.anthropic.com/)
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

The device-side setup is hardware-specific. For the M5Stack Core2 client
in this repo, see the **Configure the device** and **Build and flash**
sections of the [top-level README](../README.md). You'll paste the
`https://push-to-claude.<your-subdomain>.workers.dev` URL and the same
`DEVICE_SECRET` into `device/src/config.h`.

## Local development

```bash
npx wrangler dev
```

Wrangler boots a local proxy at `http://127.0.0.1:8787` with live reload.
For local secrets, create `worker/.dev.vars` (gitignored):

```
ANTHROPIC_API_KEY=sk-ant-...
DEVICE_SECRET=...
```

## Tail production logs

```bash
npx wrangler tail
```

## Cost notes

- **Whisper on Workers AI** (`@cf/openai/whisper`) is billed by neurons.
  Cloudflare's free tier currently includes 10,000 neurons / day; a 6-second
  voice clip uses a small fraction of that. Check Cloudflare's [Workers AI
  pricing](https://developers.cloudflare.com/workers-ai/platform/pricing/)
  for current numbers.
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

Cloudflare's Workers AI data-handling and Anthropic's data-retention
policies apply to whatever you send through them. Read theirs.
