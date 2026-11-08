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
| `netlify/functions/canvas.mjs` | `/api/canvas`: the open canvas and the tracing template |
| `lib/common.mjs` | Shared bits: keys, point decoding, time formatting |
| `lib/canvas.mjs` | Canvas state and turn limits |
| `public/templates.json` | The built-in templates (horse, alien, robot, star, microchip) as lines |

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
- Open the page with `?admin` on the end for the admin panel and a **Delete**
  button on gallery drawings. It asks for `ADMIN_KEY` once and remembers it.

### Templates

In the admin panel pick a template (or **Upload an image…**: any picture,
fitted to the screen; thin lines stay as they are, filled shapes become their
outlines) and **Send to badge**. The badge shows it as a dithered grey outline
that stays through erases until you send another or **Clear template**.

### Open canvas

**Open canvas** lets anyone on the site draw on the badge. The live view gets a
**Draw on the badge** button: a popup explains the rules, then that visitor has
30 seconds of finger (or mouse) drawing on a pad that's just the badge's white
screen filling the window - turned sideways on a phone held upright - with the
timer, **Undo**, **Erase mine**, **Fit** and **Done** floating over it. Pinch
(or the mouse wheel) zooms; two fingers move around. Then a 2 minute cooldown.
The server counts turns per browser (a random id each one keeps), not per IP
address, since at a conference most phones share the venue's address. Each
line goes out when the finger lifts and shows up on the badge and everyone's
live view within a few seconds; Undo / Erase mine take a visitor's own lines
back off both (the badge redraws).

A round starts empty. **Add image to canvas** puts a picture on it as real
lines (black, like everyone else's - not the grey template): it's fitted to the
screen, thin lines are traced down their middle, filled shapes become their
outlines. Shaking the badge (or User B) saves the knob line and everyone's
lines together as one gallery drawing and wipes both screens for the next
round. **Close canvas** stops new turns; **Clear canvas** wipes the website's
lines without saving them.

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

| `GET /api/canvas?cv=C&from=n` | | Canvas lines from `n` on (all of them if `C` isn't the current round) |
| `GET /api/canvas?part=template` | | `{tv, name, m}`: `m` is a 1-bit mask, 88 bytes a row, 412 rows, over x 128-831, y 116-527 |
| `POST /api/canvas` | `{action: "start"}` / `{action: "stroke", p}` | A visitor's turn and lines |
| `POST /api/canvas` | `{action: "undo" \| "clear-mine"}` | A visitor takes back their last line / all of theirs |
| `POST /api/canvas` | `{action: "open" \| "close" \| "clear"}`, `{action: "template", name, m}`, `{action: "import", s}` | Admin, `X-Admin-Key` |

`/api/live` replies carry the canvas summary `{open, cv, n, wn, tv}`, so the
badge and the page know when to fetch lines or a template. When the badge
starts a new drawing (it was erased), the lines that were on the old one are
archived and attached to its upload.

The badge counts as online if it checked in within the last 45 seconds (it
does every 15, every 2 while the canvas is open).
