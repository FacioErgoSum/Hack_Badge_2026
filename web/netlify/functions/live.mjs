// /api/live - the drawing on the badge right now, and whether the badge is up.
//
//   POST  badge, header X-Badge-Key, body {id, from, p, mode}
//         Appends the points in p, which start at index `from`, to drawing `id`.
//         A new id starts over (it has to come with from = 0). If `from` isn't
//         where the server is, nothing is appended and the reply says
//         {need: n} so the badge resends from there. An empty p is a heartbeat.
//   GET   ?id=...&from=N   site; returns status, and the points from N on if the
//                          drawing is still `id`, otherwise the whole drawing.
import { getStore } from "@netlify/blobs";
import { json, hasKey, cleanId, decodePoints, fmtTime } from "../../lib/common.mjs";

export const config = { path: "/api/live" };

const ONLINE_MS = 45_000;   // badge heartbeats every 15 s
const MODES = new Set(["drawing", "gallery", "idle"]);

export default async (req) => {
  const s = getStore({ name: "live", consistency: "strong" });
  const cur = (await s.get("state", { type: "json" })) || { id: "", n: 0, p: "", seen: 0, mode: "idle" };
  const now = Date.now();

  if (req.method === "POST") {
    if (!hasKey(req, "UPLOAD_KEY", "x-badge-key")) return json({ error: "bad key" }, 401);
    let body;
    try { body = await req.json(); } catch { return json({ error: "bad json" }, 400); }
    const id = cleanId(body.id);
    const from = Number.isInteger(body.from) ? body.from : -1;
    const pts = decodePoints(body.p || "");
    if (!pts) return json({ error: "bad points" }, 400);

    cur.seen = now;
    if (MODES.has(body.mode)) cur.mode = body.mode;

    if (id && id !== cur.id) {
      if (from !== 0) {
        await s.setJSON("state", cur);
        return json({ need: 0 });
      }
      Object.assign(cur, { id, n: 0, p: "", started: now });
    }
    if (id && from !== cur.n) {
      await s.setJSON("state", cur);
      return json({ need: cur.n });
    }
    if (id && pts.length) {
      const all = Buffer.concat([Buffer.from(cur.p, "base64"), pts]);
      if (all.length <= 300_000) {
        cur.p = all.toString("base64");
        cur.n = all.length / 4;
        cur.updated = now;
      }
    }
    await s.setJSON("state", cur);
    return json({ ok: true, n: cur.n });
  }

  if (req.method !== "GET") return json({ error: "method" }, 405);

  const url = new URL(req.url);
  const id = url.searchParams.get("id") || "";
  let from = parseInt(url.searchParams.get("from") || "0", 10) || 0;
  if (id !== cur.id || from < 0 || from > cur.n) from = 0;
  const p = from ? Buffer.from(cur.p, "base64").subarray(from * 4).toString("base64") : cur.p;

  return json({
    online: now - (cur.seen || 0) < ONLINE_MS,
    seen: cur.seen || 0,
    seenText: cur.seen ? fmtTime(cur.seen) : "",
    mode: cur.mode,
    id: cur.id,
    n: cur.n,
    from,
    p,
  });
};
