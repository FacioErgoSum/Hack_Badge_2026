// /api/canvas - the open canvas and the tracing template.
//
//   GET   ?cv=C&from=N        {open, cv, n, wn, tv, from, s: [b64...]}
//                             strokes from N on; all of them if C isn't current
//   GET   ?part=template      {tv, name, m}   m: 1-bit mask (see lib/canvas.mjs TPL)
//   POST  {action: "start"}                   a visitor's 30 s turn: {ok, ms} or 429 {wait}
//   POST  {action: "stroke", p}               a visitor's line, during their turn
//   POST  {action: "undo" | "clear-mine"}     take back their last line / all of theirs
//   POST  {action: "open" | "close" | "clear"}             admin (X-Admin-Key)
//   POST  {action: "template", name, m}                    admin; m "" clears it
//   POST  {action: "import", s: [b64...]}                  admin; lines added to the canvas
//                                                          (a picture the admin page traced)
//
// Visitors only get in while the canvas is open. The badge reads all of this
// too: /api/live's replies carry the summary so it knows when to fetch.
import { createHash } from "node:crypto";
import { getStore } from "@netlify/blobs";
import { json, hasKey, decodePoints } from "../../lib/common.mjs";
import {
  canvasStore, readState, updateState, resetStrokes, cleanStroke, addStroke, removeStrokes, summary, TPL, turnLimits,
} from "../../lib/canvas.mjs";

export const config = { path: "/api/canvas" };

const MASK_BYTES = (TPL.w / 8) * TPL.h;

// Turns are counted per browser: each one makes up a random id and sends it.
// Not per IP address - at a conference most phones share the venue's one.
// Without an id (an old page), fall back to the IP.
function visitorKey(req, context) {
  const id = (req.headers.get("x-visitor") || "").replace(/[^A-Za-z0-9-]/g, "").slice(0, 64);
  const who = id.length >= 16 ? "v:" + id
    : "ip:" + (context?.ip || req.headers.get("x-nf-client-connection-ip") || req.headers.get("x-forwarded-for") || "anon");
  return "ip/" + createHash("sha256").update("hack-a-sketch:" + who).digest("hex").slice(0, 24);
}

export default async (req, context) => {
  const s = canvasStore();
  const url = new URL(req.url);

  if (req.method === "GET") {
    if (url.searchParams.get("part") === "template") {
      const t = (await s.get("template", { type: "json" })) || { tv: 0, name: "", m: "" };
      return json(t);
    }
    const { state } = await readState(s);
    const cv = parseInt(url.searchParams.get("cv") || "0", 10);
    let from = parseInt(url.searchParams.get("from") || "0", 10) || 0;
    if (cv !== state.cv || from < 0 || from > state.strokes.length) from = 0;
    return json({ ...summary(state), from, s: state.strokes.slice(from) });
  }

  if (req.method !== "POST") return json({ error: "method" }, 405);
  let body;
  try { body = await req.json(); } catch { return json({ error: "bad json" }, 400); }
  const admin = hasKey(req, "ADMIN_KEY", "x-admin-key");
  const { DRAW_MS, COOLDOWN_MS, GRACE_MS } = turnLimits;

  switch (body.action) {
    // --- Admin -----------------------------------------------------------------
    case "open":
    case "close": {
      if (!admin) return json({ error: "bad key" }, 401);
      const open = body.action === "open";
      const live = (await getStore({ name: "live", consistency: "strong" }).get("state", { type: "json" })) || {};
      const { state } = await updateState(s, (st) => {
        if (st.open === open) return false;
        st.open = open;
        if (open && !st.owner) st.owner = live.id || "";
      });
      return json({ ok: true, ...summary(state) });
    }
    case "clear": {
      if (!admin) return json({ error: "bad key" }, 401);
      const { state } = await updateState(s, (st) => { resetStrokes(st); });
      return json({ ok: true, ...summary(state) });
    }
    case "import": {
      if (!admin) return json({ error: "bad key" }, 401);
      if (!Array.isArray(body.s) || !body.s.length || body.s.length > 3000) return json({ error: "bad lines" }, 400);
      const lines = [];
      for (const b64 of body.s) {
        const buf = decodePoints(b64);
        if (!buf) return json({ error: "bad points" }, 400);
        const pts = cleanStroke(buf);
        if (pts.length === 1) pts.push(pts[0]);
        if (pts.length) lines.push(pts);
      }
      const live = (await getStore({ name: "live", consistency: "strong" }).get("state", { type: "json" })) || {};
      const { state, ret } = await updateState(s, (st) => {
        for (let i = 0; i < lines.length; i++) if (!addStroke(st, lines[i], live.id, "admin")) return i;
        return lines.length;
      });
      if (ret < lines.length) return json({ error: "canvas full", added: ret }, 409);
      return json({ ok: true, added: ret, ...summary(state) });
    }
    case "template": {
      if (!admin) return json({ error: "bad key" }, 401);
      const m = typeof body.m === "string" ? body.m : "";
      if (m && Buffer.from(m, "base64").length !== MASK_BYTES) return json({ error: "bad mask" }, 400);
      const { state } = await updateState(s, (st) => { st.tv += 1; });
      await s.setJSON("template", { tv: state.tv, name: m ? String(body.name || "").slice(0, 40) : "", m });
      return json({ ok: true, ...summary(state) });
    }

    // --- Visitors --------------------------------------------------------------
    case "start": {
      const { state } = await readState(s);
      if (!state.open && !admin) return json({ error: "closed" }, 403);
      if (admin) return json({ ok: true, ms: DRAW_MS });
      const key = visitorKey(req, context);
      const rec = await s.get(key, { type: "json" });
      const now = Date.now();
      if (rec && now - rec.start < DRAW_MS + COOLDOWN_MS) {
        // Still inside a turn: let them carry on with what's left of it
        if (now - rec.start < DRAW_MS) return json({ ok: true, ms: DRAW_MS - (now - rec.start) });
        return json({ error: "cooldown", wait: rec.start + DRAW_MS + COOLDOWN_MS - now }, 429);
      }
      await s.setJSON(key, { start: now });
      return json({ ok: true, ms: DRAW_MS });
    }
    case "stroke": {
      const buf = decodePoints(body.p);
      if (!buf) return json({ error: "bad points" }, 400);
      if (!admin) {
        const rec = await s.get(visitorKey(req, context), { type: "json" });
        if (!rec || Date.now() - rec.start > DRAW_MS + GRACE_MS) return json({ error: "turn over" }, 403);
      }
      const pts = cleanStroke(buf);
      if (pts.length < 1) return json({ error: "empty" }, 400);
      if (pts.length === 1) pts.push(pts[0]);   // a tap is a dot
      const live = (await getStore({ name: "live", consistency: "strong" }).get("state", { type: "json" })) || {};
      const who = admin ? "admin" : visitorKey(req, context);
      const { state, ret } = await updateState(s, (st) => {
        if (!st.open && !admin) return "closed";
        return addStroke(st, pts, live.id, who) ? "ok" : "full";
      });
      if (ret === "closed") return json({ error: "closed" }, 403);
      if (ret === "full") return json({ error: "canvas full" }, 409);
      return json({ ok: true, ...summary(state) });
    }
    case "undo":
    case "clear-mine": {
      const who = admin ? "admin" : visitorKey(req, context);
      if (!admin) {
        const rec = await s.get(who, { type: "json" });
        if (!rec || Date.now() - rec.start > DRAW_MS + GRACE_MS) return json({ error: "turn over" }, 403);
      }
      let removed = 0;
      const { state } = await updateState(s, (st) => {
        removed = removeStrokes(st, who, body.action === "clear-mine");
        if (!removed) return false;
      });
      return json({ ok: true, removed, ...summary(state) });
    }
  }
  return json({ error: "unknown action" }, 400);
};
