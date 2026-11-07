# Badge Sketches website

The gallery for the badge's Sketch firmware (`esp-idf/sketch` on the
`wireless-development` branch). It shows every drawing the badge uploads in the
Etch A Sketch frame, with the knobs as left/right arrows, a live view of
whatever is being drawn right now, and whether the badge is online.

| Path | |
| --- | --- |
| `public/` | The page: `index.html`, `app.js`, `style.css`, `etch.png` (the frame) |
| `netlify/functions/drawings.mjs` | `/api/drawings`: badge uploads, gallery list, delete |
| `netlify/functions/live.mjs` | `/api/live`: the drawing in progress and the badge's heartbeat |
| `lib/common.mjs` | Shared bits: keys, point decoding, time formatting |

Everything is stored in [Netlify Blobs](https://docs.netlify.com/build/data-and-storage/netlify-blobs/),
so there's no database to set up.

## Put it online

1. Push the branch to GitHub.
2. On [app.netlify.com](https://app.netlify.com): **Add new project → Import an
   existing project → GitHub**, pick this repo and the `wireless-development`
   branch. The `netlify.toml` at the repo root already points Netlify at `web/`,
   so leave the build settings as they are and deploy.
3. **Project configuration → Environment variables**, add:
   - `UPLOAD_KEY`: the same string as `BADGE_UPLOAD_KEY` in the firmware's
     `src/wifi_secrets.h`
   - `ADMIN_KEY`: anything long; it's what the delete button asks for
   - `BADGE_TZ` (optional): time zone for the timestamps, default `America/Los_Angeles`

   Then **Deploys → Trigger deploy** so the functions pick them up.
4. Put the site's address (`https://something.netlify.app`, no trailing slash)
   in `BADGE_SERVER` in `src/wifi_secrets.h`, and flash the badge.

## Using it

- `‹` / `›` (or the arrow keys) step through the gallery; the far right is the
  live view. Each drawing has its own link (the part after `#`).
- **Replay** redraws the line the way it was drawn. **Save PNG** downloads it.
- Open the page with `?admin` on the end to get a **Delete** button.

## How the badge talks to it

All requests carry `X-Badge-Key: <UPLOAD_KEY>`. A drawing's points are
little-endian int16 `x, y` pairs in the 960x672 frame, base64'd.

| Request | Body | |
| --- | --- | --- |
| `POST /api/drawings` | `{id, p}` | A finished drawing; a repeated `id` is ignored |
| `POST /api/live` | `{id, from, p, mode}` | Points from index `from` on; empty `p` is a heartbeat. Replies `{need: n}` if out of step |
| `GET /api/drawings?format=badge&limit=8` | | `{items: [{t, p}]}`, newest first, for the badge gallery |
| `GET /api/drawings?keys=1` | | Every drawing's key and time |
| `GET /api/drawings?key=...` | | One drawing |
| `DELETE /api/drawings?key=...` | | Needs `X-Admin-Key: <ADMIN_KEY>` |
| `GET /api/live?id=...&from=n` | | Status, and the live points from `n` on |

The badge counts as online if it checked in within the last 45 seconds (it
does every 15).
