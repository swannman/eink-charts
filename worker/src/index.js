// Tiny ferry between the Pi (push) and the X3 (pull). The payload is already
// X25519 + AES-256-GCM sealed by the Pi before upload — this Worker never
// sees plaintext and doesn't hold the X3's private key. Bearer-token auth is
// only here to keep randos off the endpoint; confidentiality is end-to-end.

const BUNDLE_KEY = "bundle";

export default {
  async fetch(request, env) {
    const auth = request.headers.get("Authorization") ?? "";
    if (auth !== `Bearer ${env.BEARER_TOKEN}`) {
      return new Response("unauthorized", { status: 401 });
    }

    const url = new URL(request.url);
    if (url.pathname !== "/bundle") {
      return new Response("not found", { status: 404 });
    }

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
  },
};
