// /api/drawings - finished drawings.
//
//   POST    badge uploads a drawing     header X-Badge-Key, body {id, p}
//   GET     ?keys=1                     every drawing's key + time, newest first
//           ?key=d/...                  one drawing
//           ?format=badge&limit=8       newest drawings, trimmed for the badge
//   DELETE  ?key=d/...                  header X-Admin-Key
//
// A drawing is the badge's pen path p, plus s: the lines people drew on it from
// the website while the canvas was open (and the seed picture), if any.
//
// Stored in the "drawings" blob store as d/<15-digit ms>-<badge id>, so key
// order is time order. id/<badge id> points back at the key, which makes a
// retried upload a no-op instead of a duplicate.
import { getStore } from "@netlify/blobs";
import { json, hasKey, cleanId, decodePoints, fmtTime } from "../../lib/common.mjs";
import { canvasStore, closeRound, takeArchived } from "../../lib/canvas.mjs";

export const config = { path: "/api/drawings" };

const store = () => getStore({ name: "drawings", consistency: "strong" });

async function listKeys(s) {
  const { blobs } = await s.list({ prefix: "d/" });
  return blobs.map((b) => b.key).sort().reverse();
}

const tsOf = (key) => Number(key.slice(2, 17));

function view(key, d) {
  return { key, id: d.id, ts: d.ts, t: fmtTime(d.ts), n: d.n, p: d.p, s: d.s || [] };
}

export default async (req) => {
  const s = store();
  const url = new URL(req.url);
  const q = url.searchParams;

  if (req.method === "POST") {
    if (!hasKey(req, "UPLOAD_KEY", "x-badge-key")) return json({ error: "bad key" }, 401);
    let body;
    try { body = await req.json(); } catch { return json({ error: "bad json" }, 400); }
    const id = cleanId(body.id);
    const pts = decodePoints(body.p);
    if (!id || !pts || pts.length < 4) return json({ error: "bad drawing" }, 400);

    const seen = await s.get(`id/${id}`);
    if (seen) return json({ ok: true, key: seen, duplicate: true });

    // The canvas lines that were on the screen when it was erased
    const cs = canvasStore();
    await closeRound(cs, id);
    const strokes = (await takeArchived(cs, id)) || [];

    const ts = Date.now();
    const key = `d/${String(ts).padStart(15, "0")}-${id}`;
    await s.setJSON(key, { id, ts, n: pts.length / 4, p: body.p, s: strokes });
    await s.set(`id/${id}`, key);
    return json({ ok: true, key });
  }

  if (req.method === "DELETE") {
    if (!hasKey(req, "ADMIN_KEY", "x-admin-key")) return json({ error: "bad key" }, 401);
    const key = q.get("key") || "";
    if (!key.startsWith("d/")) return json({ error: "bad key name" }, 400);
    const d = await s.get(key, { type: "json" });
    if (d) await s.delete(`id/${d.id}`);
    await s.delete(key);
    return json({ ok: true });
  }

  if (req.method !== "GET") return json({ error: "method" }, 405);

  if (q.has("key")) {
    const key = q.get("key");
    const d = key.startsWith("d/") ? await s.get(key, { type: "json" }) : null;
    return d ? json(view(key, d)) : json({ error: "not found" }, 404);
  }

  const keys = await listKeys(s);

  if (q.get("format") === "badge") {
    // Kept small and flat for cJSON on the badge: the label, the path, the canvas lines
    const limit = Math.min(Math.max(parseInt(q.get("limit") || "8", 10) || 8, 1), 20);
    const items = [];
    for (const key of keys.slice(0, limit)) {
      const d = await s.get(key, { type: "json" });
      if (d) items.push({ t: fmtTime(d.ts), p: d.p, s: d.s || [] });
    }
    return json({ total: keys.length, items });
  }

  return json({ total: keys.length, keys: keys.map((key) => ({ key, ts: tsOf(key), t: fmtTime(tsOf(key)) })) });
};
