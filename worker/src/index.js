// Tiny ferry between the Pi (push) and the X3 (pull).
//
// /bundle (binary): The payload is already X25519 + AES-256-GCM sealed by
// the Pi before upload — this Worker never sees plaintext and doesn't hold
// the X3's private key. Bearer-token auth is only here to keep randos off
// the endpoint; confidentiality is end-to-end.
//
// /battery (JSON): rolling 7-day history of BQ27220 voltage readings the
// X3 posts after each successful bundle fetch. Just plaintext voltages
// (no privacy concern). The Pi reads these back to synthesize the
// battery panel inside the next encrypted bundle.

const BUNDLE_KEY = "bundle";
const BATTERY_KEY = "battery_history";
const BATTERY_RETENTION_SECONDS = 7 * 24 * 3600;  // 7 days, matches the 7d zoom view

export default {
  async fetch(request, env) {
    const auth = request.headers.get("Authorization") ?? "";
    if (auth !== `Bearer ${env.BEARER_TOKEN}`) {
      return new Response("unauthorized", { status: 401 });
    }

    const url = new URL(request.url);
    if (url.pathname === "/bundle") return handleBundle(request, env);
    if (url.pathname === "/battery") return handleBattery(request, env);
    return new Response("not found", { status: 404 });
  },
};

async function handleBundle(request, env) {
  if (request.method === "PUT") {
    const maxBytes = parseInt(env.MAX_BUNDLE_BYTES, 10) || 65536;
    const body = await request.arrayBuffer();
    if (body.byteLength > maxBytes) {
      return new Response("payload too large", { status: 413 });
    }
    await env.BUNDLE.put(BUNDLE_KEY, body, {
      httpMetadata: { contentType: "application/octet-stream" },
      customMetadata: { uploadedAt: new Date().toISOString() },
    });
    return new Response("ok", { status: 200 });
  }

  if (request.method === "GET" || request.method === "HEAD") {
    const obj = await env.BUNDLE.get(BUNDLE_KEY);
    if (!obj) return new Response("no bundle yet", { status: 404 });

    const currentEtag = `"${obj.etag}"`;
    const ifNoneMatch = request.headers.get("If-None-Match");
    if (ifNoneMatch === currentEtag) {
      return new Response(null, { status: 304, headers: { ETag: currentEtag } });
    }

    const headers = {
      "Content-Type": "application/octet-stream",
      "ETag": currentEtag,
      "Cache-Control": "no-cache",
    };
    const uploadedAt = obj.customMetadata?.uploadedAt;
    if (uploadedAt) headers["X-Uploaded-At"] = uploadedAt;

    if (request.method === "HEAD") {
      return new Response(null, { status: 200, headers });
    }
    return new Response(obj.body, { status: 200, headers });
  }

  return new Response("method not allowed", { status: 405 });
}

async function handleBattery(request, env) {
  if (request.method === "PUT") {
    // Body: {"mv": <int>}. Anything else is rejected so junk doesn't poison
    // the history.
    let body;
    try { body = await request.json(); } catch { body = null; }
    const mv = body?.mv;
    if (typeof mv !== "number" || mv < 2500 || mv > 5000) {
      return new Response("invalid mv (expected number 2500-5000)", { status: 400 });
    }

    let history = await loadBatteryHistory(env);
    const now = Math.floor(Date.now() / 1000);
    history.push({ ts: now, mv: Math.round(mv) });

    // Prune to retention window. Single writer (the X3) at low frequency,
    // so read-modify-write is safe enough.
    const cutoff = now - BATTERY_RETENTION_SECONDS;
    history = history.filter((e) => e.ts >= cutoff);

    await env.BUNDLE.put(BATTERY_KEY, JSON.stringify(history), {
      httpMetadata: { contentType: "application/json" },
      customMetadata: { uploadedAt: new Date().toISOString() },
    });
    return new Response(`ok (${history.length} readings stored)`, { status: 200 });
  }

  if (request.method === "GET" || request.method === "HEAD") {
    const obj = await env.BUNDLE.get(BATTERY_KEY);
    const headers = { "Content-Type": "application/json", "Cache-Control": "no-cache" };
    if (!obj) {
      // No readings yet — return empty array rather than 404 so the Pi can
      // just iterate without special-casing.
      return new Response(request.method === "HEAD" ? null : "[]",
                          { status: 200, headers });
    }
    if (request.method === "HEAD") return new Response(null, { status: 200, headers });
    return new Response(obj.body, { status: 200, headers });
  }

  return new Response("method not allowed", { status: 405 });
}

async function loadBatteryHistory(env) {
  const obj = await env.BUNDLE.get(BATTERY_KEY);
  if (!obj) return [];
  try {
    const arr = await obj.json();
    return Array.isArray(arr) ? arr : [];
  } catch {
    return [];
  }
}
