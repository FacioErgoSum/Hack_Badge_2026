// Shared state for the open canvas and the template, in the "canvas" blob store.
//
//   state      {open, cv, owner, strokes: [b64...], by: [who...], web, points, tv}
//              by[i] says who drew strokes[i] (a hashed visitor id, "" for the
//              seed), so a visitor can undo or clear their own lines.
//              cv counts canvas rounds: it goes up whenever the strokes are
//              cleared, so a client holding an old cv knows to start over.
//              owner is the badge drawing the strokes belong to (its id); when
//              that drawing is erased they're archived with it.
//   template   {tv, name, m}   m: 1-bit mask over TPL, base64, "" for none
//   done/<id>  {s}             strokes archived with drawing <id>, until the
//                              badge's upload of that drawing picks them up
//   ip/<hash>  {start}         when that visitor's 30 s turn started
import { getStore } from "@netlify/blobs";

export const TPL = { x0: 128, y0: 116, w: 704, h: 412 };   // template mask area, 88 bytes a row
export const SCREEN = { x0: 132, y0: 121, x1: 826, y1: 524 }; // where lines can go (the badge's pen box)
export const DRAW_MS = 30_000;
export const COOLDOWN_MS = 120_000;
const GRACE_MS = 5_000;          // strokes still in flight when the turn ends
const MAX_POINTS = 150_000;      // per round, all strokes together
const MAX_STROKE = 4_000;        // points in one post

export const canvasStore = () => getStore({ name: "canvas", consistency: "strong" });

export function encodePoints(pts) {
  const b = Buffer.alloc(pts.length * 4);
  pts.forEach(([x, y], i) => { b.writeInt16LE(x, i * 4); b.writeInt16LE(y, i * 4 + 2); });
  return b.toString("base64");
}

const fresh = () => ({ open: false, cv: 1, owner: "", strokes: [], by: [], web: 0, points: 0, tv: 0 });

export async function readState(s) {
  const r = await s.getWithMetadata("state", { type: "json" });
  if (!r || !r.data) return { state: fresh(), etag: null };
  const state = { ...fresh(), ...r.data };
  while (state.by.length < state.strokes.length) state.by.push("");
  return { state, etag: r.etag };
}

// Read-modify-write that survives many phones posting at once: the write only
// lands if nobody changed the state since we read it, otherwise read again.
// fn mutates the state and returns a value, or false to leave it alone.
export async function updateState(s, fn) {
  for (let i = 0; i < 10; i++) {
    const { state, etag } = await readState(s);
    const ret = fn(state);
    if (ret === false) return { state, ret };
    const res = await s.setJSON("state", state, etag ? { onlyIfMatch: etag } : { onlyIfNew: true });
    if (!res || res.modified !== false) return { state, ret };
    await new Promise((r) => setTimeout(r, 20 + Math.random() * 150));
  }
  throw new Error("canvas busy");
}

// A new round: an empty screen
export function resetStrokes(state) {
  state.strokes = [];
  state.by = [];
  state.points = 0;
  state.web = 0;
  state.cv += 1;
}

// The badge drawing `id` was erased: archive the strokes that were on it, so
// its upload carries them, and start the next round.
export async function closeRound(s, id) {
  if (!id) return;
  const { ret } = await updateState(s, (st) => {
    if (st.owner !== id) return false;
    const archived = st.strokes;
    resetStrokes(st);
    st.owner = "";
    return archived;
  });
  if (ret && ret.length) await s.setJSON(`done/${id}`, { s: ret });
}

// Strokes for drawing `id`, if any were archived with it (removes them)
export async function takeArchived(s, id) {
  const d = await s.get(`done/${id}`, { type: "json" });
  if (!d) return null;
  await s.delete(`done/${id}`);
  return d.s;
}

// Clamp a stroke into the screen and drop repeated points
export function cleanStroke(buf) {
  const out = [];
  for (let i = 0; i + 3 < buf.length && out.length < MAX_STROKE; i += 4) {
    const x = Math.min(SCREEN.x1, Math.max(SCREEN.x0, buf.readInt16LE(i)));
    const y = Math.min(SCREEN.y1, Math.max(SCREEN.y0, buf.readInt16LE(i + 2)));
    const last = out[out.length - 1];
    if (!last || last[0] !== x || last[1] !== y) out.push([x, y]);
  }
  return out;
}

export function addStroke(state, pts, owner, who) {
  if (state.points + pts.length > MAX_POINTS) return false;
  if (!state.owner) state.owner = owner || "";
  state.strokes.push(encodePoints(pts));
  state.by.push(who || "");
  state.points += pts.length;
  state.web += 1;
  return true;
}

// Take back `who`'s last line (all: every one of theirs this round). The
// round's version goes up, so the badge and the viewers fetch the lines again
// and redraw. Returns how many were removed.
export function removeStrokes(state, who, all) {
  let removed = 0;
  for (let i = state.strokes.length - 1; i >= 0; i--) {
    if (!who || state.by[i] !== who) continue;
    state.points -= Buffer.from(state.strokes[i], "base64").length / 4;
    state.strokes.splice(i, 1);
    state.by.splice(i, 1);
    state.web -= 1;
    removed++;
    if (!all) break;
  }
  if (removed) state.cv += 1;
  return removed;
}

export function summary(state) {
  return { open: state.open, cv: state.cv, n: state.strokes.length, wn: state.web, tv: state.tv };
}

export const turnLimits = { DRAW_MS, COOLDOWN_MS, GRACE_MS };
