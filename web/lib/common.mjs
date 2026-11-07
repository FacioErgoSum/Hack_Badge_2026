// Shared helpers for the badge API functions.
//
// A drawing is the pen's path: a run of points, each x then y as little-endian
// int16, in the badge's 960x672 frame coordinates (the same pixels as
// public/etch.png). On the wire it's that byte string in base64.

export const TZ = process.env.BADGE_TZ || "America/Los_Angeles";
export const MAX_POINT_BYTES = 300_000;   // 75k points, far more than a drawing needs

export function json(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });
}

// Constant-time-ish compare against an env var. No key configured = refuse.
export function hasKey(req, envName, header) {
  const want = process.env[envName];
  const got = req.headers.get(header) || "";
  if (!want || got.length !== want.length) return false;
  let diff = 0;
  for (let i = 0; i < want.length; i++) diff |= want.charCodeAt(i) ^ got.charCodeAt(i);
  return diff === 0;
}

export function cleanId(s) {
  return String(s || "").replace(/[^A-Za-z0-9_-]/g, "").slice(0, 48);
}

// base64 -> Buffer of whole points, or null if it isn't one
export function decodePoints(b64) {
  if (typeof b64 !== "string" || !/^[A-Za-z0-9+/]*={0,2}$/.test(b64)) return null;
  const buf = Buffer.from(b64, "base64");
  if (buf.length % 4 !== 0 || buf.length > MAX_POINT_BYTES) return null;
  return buf;
}

// "Nov 8 2:14 PM" in the conference's time zone; short enough for the badge font
export function fmtTime(ts) {
  const d = new Date(ts);
  const date = new Intl.DateTimeFormat("en-US", { timeZone: TZ, month: "short", day: "numeric" }).format(d);
  const time = new Intl.DateTimeFormat("en-US", { timeZone: TZ, hour: "numeric", minute: "2-digit" }).format(d);
  return `${date} ${time}`.replace(/ /g, " ");
}
