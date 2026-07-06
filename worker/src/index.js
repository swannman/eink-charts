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

// The TRMNL X is a second, independent device with its own sealed bundle and
// its own battery history. Distinct R2 keys so the two never collide.
// TRMNL bundles are now split one object per dashboard: bundle_trmnl_<index>.
// The device prefetches them all (guided by the manifest) and serves the
// slideshow from cache, so each dashboard gets the device's full capacity.
const BUNDLE_TRMNL_KEY = "bundle_trmnl";       // + "_<index>"
const MANIFEST_TRMNL_KEY = "manifest_trmnl";   // {count, etags} the device reads first
// TRMNL entries carry the BQ27427 fuel-gauge fields too (soc/charging/cells),
// not just voltage like the X3's BQ27220.
const BATTERY_TRMNL_KEY = "battery_history_trmnl";

// Last cache capacity the TRMNL X advertised (via the X-Bundle-Capacity header
// on its bundle GET). The Pi reads this before building the next bundle so it
// can scale chart resolution to fit the device — the server side of the
// overflow defense.
const CAPACITY_TRMNL_KEY = "capacity_trmnl";

export default {
  async fetch(request, env) {
    const auth = request.headers.get("Authorization") ?? "";
    if (auth !== `Bearer ${env.BEARER_TOKEN}`) {
      return new Response("unauthorized", { status: 401 });
    }

    const url = new URL(request.url);
    if (url.pathname === "/bundle") return handleBundle(request, env, BUNDLE_KEY);
    if (url.pathname === "/bundle-trmnl") {
      const d = parseInt(url.searchParams.get("d") ?? "", 10);
      if (!Number.isInteger(d) || d < 0 || d > 63) {
        return new Response("missing/invalid ?d (0-63)", { status: 400 });
      }
      return handleBundle(request, env, `${BUNDLE_TRMNL_KEY}_${d}`, CAPACITY_TRMNL_KEY);
    }
    if (url.pathname === "/manifest-trmnl") return handleBundle(request, env, MANIFEST_TRMNL_KEY);
    if (url.pathname === "/capacity-trmnl") return handleCapacity(request, env, CAPACITY_TRMNL_KEY);
    if (url.pathname === "/battery") return handleBattery(request, env, BATTERY_KEY);
    if (url.pathname === "/battery-trmnl") return handleBattery(request, env, BATTERY_TRMNL_KEY);
    return new Response("not found", { status: 404 });
  },
};

async function handleBundle(request, env, key, capacityKey) {
  // On a device GET, record the cache capacity it advertised so the Pi can
  // fit the next bundle to it. Best-effort — a bad/absent header just leaves
  // the last known value in place.
  if (capacityKey && (request.method === "GET" || request.method === "HEAD")) {
    const raw = request.headers.get("X-Bundle-Capacity");
    const cap = raw ? parseInt(raw, 10) : NaN;
    if (Number.isFinite(cap) && cap > 0 && cap < 1 << 24) {
      await env.BUNDLE.put(
        capacityKey,
        JSON.stringify({ cap, reportedAt: new Date().toISOString() }),
        { httpMetadata: { contentType: "application/json" } },
      );
    }
  }

  if (request.method === "PUT") {
    const maxBytes = parseInt(env.MAX_BUNDLE_BYTES, 10) || 262144;
    const body = await request.arrayBuffer();
    if (body.byteLength > maxBytes) {
      return new Response("payload too large", { status: 413 });
    }
    await env.BUNDLE.put(key, body, {
      httpMetadata: { contentType: "application/octet-stream" },
      customMetadata: { uploadedAt: new Date().toISOString() },
    });
    return new Response("ok", { status: 200 });
  }

  if (request.method === "GET" || request.method === "HEAD") {
    const obj = await env.BUNDLE.get(key);
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

async function handleCapacity(request, env, key) {
  // Read-only for the Pi. Returns {cap, reportedAt} or {} if the device has
  // not reported yet, so the caller can fall back to a conservative default.
  if (request.method === "GET" || request.method === "HEAD") {
    const obj = await env.BUNDLE.get(key);
    const headers = { "Content-Type": "application/json", "Cache-Control": "no-cache" };
    if (request.method === "HEAD") return new Response(null, { status: 200, headers });
    return new Response(obj ? obj.body : "{}", { status: 200, headers });
  }
  return new Response("method not allowed", { status: 405 });
}

async function handleBattery(request, env, key) {
  if (request.method === "PUT") {
    // Body: {"mv": <int>, "soc"?: <int -1..100>, "charging"?: <bool>,
    // "cells"?: <int 0..2>}. mv is required; the rest are optional TRMNL X fuel-
    // gauge fields (the X3 sends mv only). Upper bound spans a 2-cell pack.
    let body;
    try { body = await request.json(); } catch { body = null; }
    const mv = body?.mv;
    if (typeof mv !== "number" || mv < 2500 || mv > 9000) {
      return new Response("invalid mv (expected number 2500-9000)", { status: 400 });
    }

    let history = await loadBatteryHistory(env, key);
    const now = Math.floor(Date.now() / 1000);
    const entry = { ts: now, mv: Math.round(mv) };
    if (typeof body.soc === "number" && body.soc >= 0 && body.soc <= 100)
      entry.soc = Math.round(body.soc);
    if (typeof body.charging === "boolean") entry.charging = body.charging;
    if (typeof body.cells === "number" && body.cells >= 0 && body.cells <= 2)
      entry.cells = body.cells;
    history.push(entry);

    // Prune to retention window. Single writer per device at low frequency,
    // so read-modify-write is safe enough.
    const cutoff = now - BATTERY_RETENTION_SECONDS;
    history = history.filter((e) => e.ts >= cutoff);

    await env.BUNDLE.put(key, JSON.stringify(history), {
      httpMetadata: { contentType: "application/json" },
      customMetadata: { uploadedAt: new Date().toISOString() },
    });
    return new Response(`ok (${history.length} readings stored)`, { status: 200 });
  }

  if (request.method === "GET" || request.method === "HEAD") {
    const obj = await env.BUNDLE.get(key);
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

async function loadBatteryHistory(env, key) {
  const obj = await env.BUNDLE.get(key);
  if (!obj) return [];
  try {
    const arr = await obj.json();
    return Array.isArray(arr) ? arr : [];
  } catch {
    return [];
  }
}
