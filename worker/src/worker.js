// Push-to-Claude relay.
//
// Endpoints:
//   POST /ask         — body is raw WAV audio. Whisper STT -> Claude.
//   POST /ask-text    — body is JSON {prompt}. Claude direct.
//   POST /reset       — clear this device's stored conversation history.
//   GET  /            — health probe.
//
// All authenticated by `x-device-secret` matching env.DEVICE_SECRET.
//
// Conversation memory: last ~8 messages (4 turns) per device-secret are
// kept in Workers KV (binding "HISTORY") with a 24h TTL. Each /ask or
// /ask-text request loads them, appends the new user turn, sends the
// full sequence to Claude, then appends the assistant turn back to KV.
// This is what turns the device from a one-shot query box into a chat
// partner that remembers what you just talked about.

const SYSTEM_PROMPT =
  "You are Claude responding on a 240x135 pixel handheld LCD. " +
  "Reply in 1-3 short sentences. Plain ASCII when possible. " +
  "No markdown, no lists, no code fences. " +
  "Be direct; assume the user can't scroll. " +
  "You may receive a few prior turns of conversation history; " +
  "treat the latest user message as the current question.";

const CLAUDE_MODEL = "claude-haiku-4-5-20251001";
const HISTORY_MAX_MESSAGES = 8; // 4 user/assistant pairs
const HISTORY_TTL_SECONDS = 24 * 3600;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "GET" && url.pathname === "/") {
      return new Response("push-to-claude relay ok\n", {
        headers: { "content-type": "text/plain" },
      });
    }
    if (request.method === "POST" && url.pathname === "/ask") {
      return handleAsk(request, env);
    }
    if (request.method === "POST" && url.pathname === "/ask-text") {
      return handleAskText(request, env);
    }
    if (request.method === "POST" && url.pathname === "/reset") {
      return handleReset(request, env);
    }
    return new Response("not found\n", { status: 404 });
  },
};

function authOk(request, env) {
  return request.headers.get("x-device-secret") === env.DEVICE_SECRET;
}

function historyKey(deviceSecret) {
  return `turns:${deviceSecret}`;
}

async function getHistory(env, deviceSecret) {
  if (!env.HISTORY) return [];
  try {
    const raw = await env.HISTORY.get(historyKey(deviceSecret));
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
}

async function appendTurn(env, deviceSecret, userMsg, assistantMsg) {
  if (!env.HISTORY) return;
  const hist = await getHistory(env, deviceSecret);
  hist.push({ role: "user", content: userMsg });
  hist.push({ role: "assistant", content: assistantMsg });
  const trimmed = hist.slice(-HISTORY_MAX_MESSAGES);
  await env.HISTORY.put(historyKey(deviceSecret), JSON.stringify(trimmed), {
    expirationTtl: HISTORY_TTL_SECONDS,
  });
}

async function callClaude(env, deviceSecret, userMessage) {
  const history = await getHistory(env, deviceSecret);
  const messages = [...history, { role: "user", content: userMessage }];

  const claudeResp = await fetch("https://api.anthropic.com/v1/messages", {
    method: "POST",
    headers: {
      "content-type": "application/json",
      "x-api-key": env.ANTHROPIC_API_KEY,
      "anthropic-version": "2023-06-01",
    },
    body: JSON.stringify({
      model: CLAUDE_MODEL,
      max_tokens: 250,
      system: SYSTEM_PROMPT,
      messages,
    }),
  });

  if (!claudeResp.ok) {
    const detail = (await claudeResp.text()).slice(0, 300);
    return { ok: false, status: claudeResp.status, detail };
  }
  const data = await claudeResp.json();
  const text = (data.content?.[0]?.text || "").trim() || "(empty)";
  // Persist the turn even on transport-fine but logically-empty
  // responses; the user just had a turn and we want history to
  // reflect that. Skip persistence on errors.
  await appendTurn(env, deviceSecret, userMessage, text);
  return { ok: true, text };
}

async function handleAsk(request, env) {
  if (!authOk(request, env)) return json({ error: "unauthorized" }, 401);

  const audioBytes = await request.arrayBuffer();
  if (audioBytes.byteLength < 200) {
    return json(
      { error: "audio too short", bytes: audioBytes.byteLength },
      400,
    );
  }

  const form = new FormData();
  form.append(
    "file",
    new Blob([audioBytes], { type: "audio/wav" }),
    "audio.wav",
  );
  form.append("model", "whisper-1");
  form.append("response_format", "text");

  const whisperResp = await fetch(
    "https://api.openai.com/v1/audio/transcriptions",
    {
      method: "POST",
      headers: { Authorization: `Bearer ${env.OPENAI_API_KEY}` },
      body: form,
    },
  );
  if (!whisperResp.ok) {
    const detail = (await whisperResp.text()).slice(0, 300);
    return json(
      { error: "whisper failed", status: whisperResp.status, detail },
      502,
    );
  }
  const transcript = (await whisperResp.text()).trim();
  if (!transcript) {
    return json({ transcript: "", response: "(no speech)" });
  }

  const deviceSecret = request.headers.get("x-device-secret");
  const result = await callClaude(env, deviceSecret, transcript);
  if (!result.ok) {
    return json(
      {
        transcript,
        error: "claude failed",
        status: result.status,
        detail: result.detail,
      },
      502,
    );
  }
  return json({ transcript, response: result.text });
}

async function handleAskText(request, env) {
  if (!authOk(request, env)) return json({ error: "unauthorized" }, 401);
  let data;
  try {
    data = await request.json();
  } catch {
    return json({ error: "invalid json" }, 400);
  }
  const prompt = ((data.prompt || data.text || "") + "").trim();
  if (!prompt) return json({ error: "empty prompt" }, 400);

  const deviceSecret = request.headers.get("x-device-secret");
  const result = await callClaude(env, deviceSecret, prompt);
  if (!result.ok) {
    return json(
      {
        transcript: prompt,
        error: "claude failed",
        status: result.status,
        detail: result.detail,
      },
      502,
    );
  }
  return json({ transcript: prompt, response: result.text });
}

async function handleReset(request, env) {
  if (!authOk(request, env)) return json({ error: "unauthorized" }, 401);
  const deviceSecret = request.headers.get("x-device-secret");
  if (env.HISTORY) {
    await env.HISTORY.delete(historyKey(deviceSecret));
  }
  return json({ ok: true, cleared: true });
}

function json(obj, status = 200) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json" },
  });
}
