// Hack-A-Sketch: the gallery of finished drawings plus a live slide.
//
// Slides run newest first, left to right: slides[0] is LIVE, slides[1] the
// newest drawing, the last one the oldest. The right knob steps back in time.
//
// The knobs turn with the pen like the badge's own: the left one drives x, the
// right one y, PX_PER_REV pixels per turn, clockwise = right / up.
//
// The live slide also shows the tracing template (grey) and the open canvas:
// lines people draw here on their phones, which the badge picks up too. While
// the canvas is open, a Draw button gives each visitor a 30 s turn.
(() => {
  "use strict";

  const PX_PER_REV = 240;          // same as the badge firmware
  const REPLAY_MS_PER_PX = 4;      // about one knob turn a second
  const PEN = 8;                   // pen square, px

  const $ = (id) => document.getElementById(id);
  const line = $("line"), pen = $("pen"), empty = $("empty"), stamp = $("stamp"), count = $("count");
  const liveTag = $("live-tag"), statusEl = $("status"), statusText = $("status-text");
  const leftBtn = $("left"), rightBtn = $("right"), replayBtn = $("replay"), liveBtn = $("to-live");
  const gripX = leftBtn.querySelector(".grip"), gripY = rightBtn.querySelector(".grip");
  const pngBtn = $("png"), delBtn = $("delete"), tools = $("tools");
  const tplImg = $("tpl"), webG = $("web"), mineG = $("mine"), art = $("art");
  const timerEl = $("timer"), doneBtn = $("done");
  const drawBar = $("draw-bar"), drawBtn = $("draw"), dialog = $("draw-dialog");
  const admin = new URLSearchParams(location.search).has("admin");
  const SVGNS = "http://www.w3.org/2000/svg";
  const SCREEN = { x0: 132, y0: 121, x1: 826, y1: 524 };   // where lines can go, as on the badge
  const TPL = { x0: 128, y0: 116, w: 704, h: 412 };         // template mask area

  let keys = [];            // [{key, ts, t}] newest first
  let index = 0;            // 0 = live, i = keys[i - 1]
  const cache = new Map();  // key -> {t, pts}
  let live = { id: "", pts: [], online: false, mode: "idle", seen: 0, seenText: "" };
  let shownPts = [];
  let anim = 0, replayTimer = 0;
  let livePollTimer = 0;
  // The open canvas as this page last saw it
  let canv = { cv: 0, open: false, wn: 0, tv: -1, strokes: [] };
  let shownStrokes = [];
  // --- Points --------------------------------------------------------------
  function decode(b64) {
    if (!b64) return [];
    const bin = atob(b64);
    const pts = [];
    for (let i = 0; i + 3 < bin.length; i += 4) {
      let x = bin.charCodeAt(i) | (bin.charCodeAt(i + 1) << 8);
      let y = bin.charCodeAt(i + 2) | (bin.charCodeAt(i + 3) << 8);
      if (x & 0x8000) x -= 0x10000;
      if (y & 0x8000) y -= 0x10000;
      pts.push([x, y]);
    }
    return pts;
  }
  const toAttr = (pts) => pts.map((p) => p[0] + "," + p[1]).join(" ");

  // --- Pen and knobs ---------------------------------------------------------
  function placePen(x, y, show) {
    pen.setAttribute("x", x - PEN / 2);
    pen.setAttribute("y", y - PEN / 2);
    pen.setAttribute("visibility", show ? "visible" : "hidden");
  }

  // Knob angle from the pen position, measured from the middle of the screen.
  // `smooth` lets CSS ease them there; replays set every frame and turn it off.
  function turnKnobs(x, y, smooth) {
    gripX.classList.toggle("turning", !smooth);
    gripY.classList.toggle("turning", !smooth);
    gripX.style.transform = `rotate(${((x - 480) * 360) / PX_PER_REV}deg)`;
    gripY.style.transform = `rotate(${(-(y - 336) * 360) / PX_PER_REV}deg)`;
  }

  // The canvas lines (or a gallery drawing's). New ones can draw themselves in.
  function setWeb(strokes, animate = null) {
    shownStrokes = strokes;
    while (webG.childNodes.length > strokes.length) webG.lastChild.remove();
    strokes.forEach((pts, i) => {
      let el = webG.childNodes[i];
      if (!el) { el = document.createElementNS(SVGNS, "polyline"); webG.appendChild(el); }
      const attr = toAttr(pts);
      if (el.getAttribute("points") === attr) return;
      el.setAttribute("points", attr);
      if (animate && animate.has(i)) {
        el.setAttribute("pathLength", "1");
        el.classList.remove("in");
        el.style.strokeDasharray = "1";
        el.style.strokeDashoffset = "1";
        el.getBoundingClientRect();
        el.classList.add("in");
        el.style.strokeDashoffset = "0";
      } else {
        el.removeAttribute("pathLength");
        el.style.strokeDasharray = el.style.strokeDashoffset = "";
      }
    });
  }

  function draw(pts, { showPen = false } = {}) {
    cancelAnimationFrame(anim);
    clearTimeout(replayTimer);
    shownPts = pts;
    line.setAttribute("points", toAttr(pts));
    if (pts.length) {
      const [x, y] = pts[pts.length - 1];
      placePen(x, y, showPen);
      turnKnobs(x, y, true);
    } else {
      placePen(0, 0, false);
      turnKnobs(480, 336, true);
    }
  }

  // Redraw the line the way it was drawn, the pen moving at a steady speed
  // along it and the knobs turning to match.
  function replay() {
    const pts = shownPts;
    if (pts.length < 2) return;
    cancelAnimationFrame(anim);
    clearTimeout(replayTimer);
    let total = 0;
    const acc = [0];
    for (let i = 1; i < pts.length; i++) {
      total += Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]);
      acc.push(total);
    }
    const ms = Math.min(15000, Math.max(1500, total * REPLAY_MS_PER_PX));
    let t0 = 0;
    let n = 1;
    const step = (now) => {
      if (!t0) t0 = now;
      const want = Math.min(1, (now - t0) / ms) * total;
      while (n < pts.length - 1 && acc[n] < want) n++;
      // The pen sits partway along segment n-1 -> n
      const [ax, ay] = pts[n - 1], [bx, by] = pts[n];
      const seg = acc[n] - acc[n - 1];
      const f = seg > 0 ? Math.min(1, Math.max(0, (want - acc[n - 1]) / seg)) : 1;
      const x = ax + (bx - ax) * f, y = ay + (by - ay) * f;
      line.setAttribute("points", toAttr(pts.slice(0, n)) + ` ${x},${y}`);
      placePen(x, y, true);
      turnKnobs(x, y, false);
      if (want < total) anim = requestAnimationFrame(step);
      else draw(pts);
    };
    // Wind the knobs back to where the drawing started, then go
    placePen(pts[0][0], pts[0][1], true);
    line.setAttribute("points", "");
    turnKnobs(pts[0][0], pts[0][1], true);
    replayTimer = setTimeout(() => (anim = requestAnimationFrame(step)), 450);
  }

  // --- API -----------------------------------------------------------------
  async function api(path, opts) {
    const r = await fetch(path, opts);
    if (!r.ok) throw new Error(path + " " + r.status);
    return r.json();
  }

  async function loadKeys() {
    try {
      const data = await api("/api/drawings?keys=1");
      const current = index > 0 ? keys[index - 1]?.key : null;
      keys = data.keys || [];
      if (current) {
        const i = keys.findIndex((k) => k.key === current);
        index = i >= 0 ? i + 1 : Math.min(index, keys.length);
      }
    } catch (e) {
      console.warn(e);
    }
  }

  async function getDrawing(key) {
    if (!cache.has(key)) {
      const d = await api("/api/drawings?key=" + encodeURIComponent(key));
      cache.set(key, { t: d.t, pts: decode(d.p), strokes: (d.s || []).map(decode) });
    }
    return cache.get(key);
  }

  // --- Status pill -----------------------------------------------------------
  function renderStatus() {
    if (live.online) {
      statusEl.dataset.state = "online";
      statusText.textContent = "Badge Online";
    } else {
      statusEl.dataset.state = "offline";
      statusText.textContent = "Badge Offline";
    }
  }

  async function pollLive() {
    try {
      const from = live.id ? live.pts.length : 0;
      const d = await api(`/api/live?id=${encodeURIComponent(live.id)}&from=${from}`);
      const changed = d.id !== live.id;
      if (changed) {
        // The badge was erased: that drawing is (or will be) in the gallery
        if (live.id) setTimeout(() => loadKeys().then(render), 2500);
        live.pts = decode(d.p);
      } else if (d.from === from) {
        live.pts = live.pts.concat(decode(d.p));
      } else {
        live.pts = decode(d.p);
      }
      Object.assign(live, { id: d.id, online: d.online, mode: d.mode, seen: d.seen, seenText: d.seenText });
      renderStatus();
      if (d.canvas) await syncCanvas(d.canvas);
      if (index === 0) renderLive();
      if (drawing) paintPad();
    } catch (e) {
      console.warn(e);
    }
    clearTimeout(livePollTimer);
    const fast = index === 0 && (live.online || canv.open) && !document.hidden;
    livePollTimer = setTimeout(pollLive, drawing ? 1500 : fast ? 2000 : 12000);
  }

  // --- Canvas and template ---------------------------------------------------
  let animateFrom = null;   // which canvas lines to draw in on the next render (a Set)
  async function syncCanvas(sum) {
    try {
      if (sum.cv !== canv.cv || sum.n > canv.strokes.length) {
        const firstLoad = canv.cv === 0;
        const from = sum.cv !== canv.cv ? 0 : canv.strokes.length;
        const c = await api(`/api/canvas?cv=${canv.cv}&from=${from}`);
        const got = c.s.map(decode);
        const before = new Set(canv.raw || []);
        const raw = c.from === 0 ? c.s : (canv.raw || []).concat(c.s);
        canv.strokes = c.from === 0 ? got : canv.strokes.concat(got);
        canv.raw = raw;
        canv.cv = c.cv;
        // Draw in only the lines that weren't there before (an undo refetches all)
        animateFrom = firstLoad ? null : new Set(raw.map((r, i) => (before.has(r) ? -1 : i)).filter((i) => i >= 0));
        // My lines are on the server now: stop drawing my local copies of them
        if (!drawing && !pending && !queue.length) mine = [];
        if (drawing) paintPad();
      }
      Object.assign(canv, { open: sum.open, wn: sum.wn });
      if (sum.tv !== canv.tv) await loadTemplate(sum.tv);
      renderDrawBar();
      renderAdmin();
    } catch (e) {
      console.warn(e);
    }
  }

  // The template mask -> a grey picture over the screen
  async function loadTemplate(tv) {
    const t = await api("/api/canvas?part=template");
    canv.tv = tv;
    if (!t.m) { tplImg.removeAttribute("href"); tplImg.setAttribute("visibility", "hidden"); tplBitmap = null; if (drawing) paintPad(); return; }
    const bits = Uint8Array.from(atob(t.m), (ch) => ch.charCodeAt(0));
    const c = document.createElement("canvas");
    c.width = TPL.w; c.height = TPL.h;
    const g = c.getContext("2d");
    const img = g.createImageData(TPL.w, TPL.h);
    for (let y = 0; y < TPL.h; y++) {
      for (let x = 0; x < TPL.w; x++) {
        if (bits[y * (TPL.w / 8) + (x >> 3)] & (0x80 >> (x & 7))) {
          const o = (y * TPL.w + x) * 4;
          img.data[o] = img.data[o + 1] = img.data[o + 2] = 150;
          img.data[o + 3] = 255;
        }
      }
    }
    g.putImageData(img, 0, 0);
    tplBitmap = c;
    if (drawing) paintPad();
    tplImg.setAttribute("href", c.toDataURL());
    tplImg.setAttribute("visibility", index === 0 ? "visible" : "hidden");
  }

  // --- Slides --------------------------------------------------------------
  function renderLive() {
    liveTag.setAttribute("visibility", live.online ? "visible" : "hidden");
    tplImg.setAttribute("visibility", tplImg.getAttribute("href") ? "visible" : "hidden");
    setWeb(canv.strokes, animateFrom);
    animateFrom = null;
    draw(live.pts, { showPen: live.online });
    empty.textContent = live.pts.length > 1 || canv.strokes.length ? "" : live.online ? "Waiting for the knobs to turn…" : "The badge is offline";
    stamp.textContent = "LIVE VIEW";
    count.textContent = keys.length ? keys.length + " IN THE GALLERY ›" : "";
  }

  async function render() {
    leftBtn.disabled = index <= 0;
    rightBtn.disabled = index >= keys.length;
    tools.hidden = index === 0;   // replay / download / delete are for gallery drawings
    delBtn.hidden = !admin;
    renderDrawBar();

    if (index === 0) {
      renderLive();
      history.replaceState(null, "", location.pathname + location.search);
      clearTimeout(livePollTimer);
      pollLive();
      return;
    }
    liveTag.setAttribute("visibility", "hidden");
    tplImg.setAttribute("visibility", "hidden");
    setWeb([]);
    const k = keys[index - 1];
    stamp.textContent = k.t.toUpperCase();
    count.textContent = `${index} OF ${keys.length}`;
    empty.textContent = "";
    history.replaceState(null, "", "#" + k.key);
    try {
      const d = await getDrawing(k.key);
      if (keys[index - 1] !== k) return;   // moved on while loading
      setWeb(d.strokes);
      draw(d.pts);
    } catch (e) {
      setWeb([]);
      draw([]);
      empty.textContent = "Couldn't load this one";
    }
  }

  function go(to) {
    if (drawing) return;
    index = Math.max(0, Math.min(keys.length, to));
    render();
  }

  // --- PNG export: frame + line on a canvas ------------------------------------
  async function makePng() {
    const img = new Image();
    img.src = "etch.png";
    await img.decode();
    // Twice the badge's resolution, like etch.png, so it stays sharp
    const c = document.createElement("canvas");
    c.width = 1920; c.height = 1344;
    const g = c.getContext("2d");
    g.scale(2, 2);   // draw in the badge's 960x672 coordinates
    // Solid white underneath, so nothing in the file is see-through
    g.fillStyle = "#fff";
    g.fillRect(0, 0, 960, 672);
    g.drawImage(img, 0, 0, 960, 672);
    g.strokeStyle = "#3b3b3b"; g.lineWidth = 3; g.lineCap = "square"; g.lineJoin = "miter";
    g.beginPath();
    shownPts.forEach(([x, y], i) => (i ? g.lineTo(x, y) : g.moveTo(x, y)));
    g.stroke();
    g.lineCap = "round"; g.lineJoin = "round";
    for (const pts of shownStrokes) {
      g.beginPath();
      pts.forEach(([x, y], i) => (i ? g.lineTo(x, y) : g.moveTo(x, y)));
      g.stroke();
    }
    g.fillStyle = "#fff"; g.font = "800 34px Inter, sans-serif"; g.textAlign = "center";
    g.fillText(stamp.textContent, 480, 606);
    return new Promise((resolve) => c.toBlob(resolve, "image/png"));
  }

  // Made fresh on every click, so it's always the drawing on screen right now
  pngBtn.addEventListener("click", async () => {
    const blob = await makePng();
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = index > 0 ? "badge-" + keys[index - 1].key.slice(2) + ".png" : "badge-live.png";
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
  });

  // --- Admin delete (open the page with ?admin) ------------------------------
  delBtn.addEventListener("click", async () => {
    const k = keys[index - 1];
    if (!k) return;
    const key = adminKey(true).trim();
    if (!key || !confirm("Delete the drawing from " + k.t + "?")) return;
    const r = await fetch("/api/drawings?key=" + encodeURIComponent(k.key), {
      method: "DELETE", headers: { "x-admin-key": key },
    });
    if (r.status === 401) {
      try { localStorage.removeItem("adminKey"); } catch {}
      alert("Wrong admin key");
      return;
    }
    try { localStorage.setItem("adminKey", key); } catch {}
    cache.delete(k.key);
    await loadKeys();
    go(index);
  });

  // --- Admin key ---------------------------------------------------------------
  function adminKey(ask) {
    let key = "";
    try { key = localStorage.getItem("adminKey") || ""; } catch {}
    if (!key && ask) key = prompt("Admin key") || "";
    return key;
  }
  function forgetAdminKey() { try { localStorage.removeItem("adminKey"); } catch {} }
  function keepAdminKey(k) { try { localStorage.setItem("adminKey", k); } catch {} }

  // --- Drawing on the badge from here (open canvas) ---------------------------
  // A 30 s turn on a full-window pad that shows just the badge's white screen.
  // Each line goes out when the finger lifts; Undo / Erase take back your own.
  // Then a 2 minute cooldown before the next turn.
  //
  // The pad does its own coordinate maths (no CSS rotation): frame point ->
  // "base" (fit the screen into the window, turned 90 degrees clockwise when the
  // window is taller than wide, so the top of the badge is on the right) ->
  // zoom (scale z about the window origin, then pan tx, ty).
  const pad = $("pad"), padCanvas = $("pad-canvas"), padUi = $("pad-ui"), padHint = $("pad-hint");
  const undoBtn = $("undo"), clearMineBtn = $("clear-mine"), fitBtn = $("fit");
  const SW = SCREEN.x1 - SCREEN.x0, SH = SCREEN.y1 - SCREEN.y0;
  let drawing = false, turnEnd = 0, turnTimer = 0, cooldownUntil = 0, cooldownTimer = 0;
  let view = { w: 0, h: 0, rot: false, k: 1, ox: 0, oy: 0, z: 1, tx: 0, ty: 0 };
  let mine = [];            // my lines this turn: {pts, state: "drawing" | "queued" | "sent"}
  let queue = [];           // lines waiting to go out, in order
  let pending = 0;          // posts in flight
  let current = null;       // the line under the finger
  const touches = new Map();  // pointerId -> [x, y] in window px
  let pinch = null;
  let tplBitmap = null;
  let padFrame = 0;

  try { cooldownUntil = +localStorage.getItem("cooldownUntil") || 0; } catch {}

  function renderDrawBar() {
    drawBar.hidden = !(index === 0 && canv.open) || drawing;
    const left = cooldownUntil - Date.now();
    clearTimeout(cooldownTimer);
    if (left > 0 && !adminKey(false)) {
      drawBtn.disabled = true;
      const s = Math.ceil(left / 1000);
      drawBtn.textContent = `Next turn in ${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
      cooldownTimer = setTimeout(renderDrawBar, 1000);
    } else {
      drawBtn.disabled = false;
      drawBtn.textContent = "Draw on the badge";
    }
  }

  // A random id for this browser, so turns are counted per phone, not per Wi-Fi
  let visitor = "";
  try { visitor = localStorage.getItem("visitor") || ""; } catch {}
  if (!visitor) {
    visitor = (crypto.randomUUID ? crypto.randomUUID() : Math.random().toString(36).slice(2) + Date.now().toString(36)).replace(/[^A-Za-z0-9-]/g, "");
    try { localStorage.setItem("visitor", visitor); } catch {}
  }

  async function canvasPost(body, useAdmin) {
    const headers = { "content-type": "application/json", "x-visitor": visitor };
    const key = adminKey(false);
    if (key && (useAdmin || admin)) headers["x-admin-key"] = key;
    const r = await fetch("/api/canvas", { method: "POST", headers, body: JSON.stringify(body) });
    return { status: r.status, body: await r.json().catch(() => ({})) };
  }

  function encodePts(pts) {
    let bin = "";
    for (const [x, y] of pts) bin += String.fromCharCode(x & 255, (x >> 8) & 255, y & 255, (y >> 8) & 255);
    return btoa(bin);
  }

  // --- Pad geometry ---
  function layoutPad() {
    const w = pad.clientWidth, h = pad.clientHeight;
    const rot = h > w;
    const margin = 8;
    const k = rot ? Math.min((h - 2 * margin) / SW, (w - 2 * margin) / SH) : Math.min((w - 2 * margin) / SW, (h - 2 * margin) / SH);
    const cw = rot ? SH * k : SW * k, ch = rot ? SW * k : SH * k;
    view = { ...view, w, h, rot, k, ox: (w - cw) / 2, oy: (h - ch) / 2 };
    const dpr = Math.min(window.devicePixelRatio || 1, 3);
    padCanvas.width = Math.round(w * dpr);
    padCanvas.height = Math.round(h * dpr);
    pad.classList.toggle("rot", rot);
    // Controls along the badge's top edge, the hint along its bottom: on the
    // right / left of the window when the screen is turned
    if (rot) {
      padUi.style.width = h + "px";
      padUi.style.transform = `translateX(${w}px) rotate(90deg)`;
      const hw = padHint.offsetWidth, hh = padHint.offsetHeight;
      padHint.style.cssText = `left:0;top:0;bottom:auto;transform-origin:0 0;transform:translate(${14 + hh}px, ${(h - hw) / 2}px) rotate(90deg)`;
    } else {
      padUi.style.width = "100%";
      padUi.style.transform = "";
      padHint.style.cssText = "";
    }
    clampPan();
    paintPad();
  }
  function toBase(fx, fy) {   // frame -> window px at zoom 1
    const x = (fx - SCREEN.x0) * view.k, y = (fy - SCREEN.y0) * view.k;
    return view.rot ? [view.ox + SH * view.k - y, view.oy + x] : [view.ox + x, view.oy + y];
  }
  function toFrame(px, py) {  // window px -> frame, clamped to the screen
    const bx = (px - view.tx) / view.z, by = (py - view.ty) / view.z;
    let x, y;
    if (view.rot) { x = by - view.oy; y = view.ox + SH * view.k - bx; }
    else { x = bx - view.ox; y = by - view.oy; }
    const fx = SCREEN.x0 + x / view.k, fy = SCREEN.y0 + y / view.k;
    return [Math.round(Math.min(SCREEN.x1, Math.max(SCREEN.x0, fx))), Math.round(Math.min(SCREEN.y1, Math.max(SCREEN.y0, fy)))];
  }
  function clampPan() {
    // Keep the screen from sliding off: at zoom 1 it's centred, zoomed in its
    // edges can't come further in than the window's
    const z = view.z;
    if (z <= 1.001) { view.z = 1; view.tx = view.ty = 0; return; }
    const [ax, ay] = toBase(SCREEN.x0, SCREEN.y0), [bx, by] = toBase(SCREEN.x1, SCREEN.y1);
    const x0 = Math.min(ax, bx) * z, x1 = Math.max(ax, bx) * z, y0 = Math.min(ay, by) * z, y1 = Math.max(ay, by) * z;
    view.tx = Math.min(-x0 + 8, Math.max(view.w - x1 - 8, view.tx));
    view.ty = Math.min(-y0 + 8, Math.max(view.h - y1 - 8, view.ty));
  }

  function paintPad() {
    cancelAnimationFrame(padFrame);
    padFrame = requestAnimationFrame(() => {
      const g = padCanvas.getContext("2d");
      const dpr = padCanvas.width / Math.max(1, view.w);
      g.setTransform(1, 0, 0, 1, 0, 0);
      g.fillStyle = "#e9e7e2";
      g.fillRect(0, 0, padCanvas.width, padCanvas.height);
      // window px -> device px, zoom, then frame -> base
      g.setTransform(dpr * view.z, 0, 0, dpr * view.z, dpr * view.tx, dpr * view.ty);
      if (view.rot) { g.translate(view.ox + SH * view.k, view.oy); g.rotate(Math.PI / 2); }
      else g.translate(view.ox, view.oy);
      g.scale(view.k, view.k);
      g.translate(-SCREEN.x0, -SCREEN.y0);
      // the screen
      g.fillStyle = "#fff";
      g.shadowColor = "rgba(0,0,0,0.25)"; g.shadowBlur = 12 / view.k;
      g.fillRect(SCREEN.x0, SCREEN.y0, SW, SH);
      g.shadowBlur = 0;
      g.save();
      g.beginPath(); g.rect(SCREEN.x0, SCREEN.y0, SW, SH); g.clip();
      if (tplBitmap) g.drawImage(tplBitmap, TPL.x0, TPL.y0, TPL.w, TPL.h);
      g.lineCap = "round"; g.lineJoin = "round"; g.lineWidth = 3;
      const line = (pts) => {
        if (!pts.length) return;
        g.beginPath();
        g.moveTo(pts[0][0], pts[0][1]);
        if (pts.length === 1) g.lineTo(pts[0][0] + 0.01, pts[0][1]);
        for (let i = 1; i < pts.length; i++) g.lineTo(pts[i][0], pts[i][1]);
        g.stroke();
      };
      g.strokeStyle = "#3b3b3b";
      canv.strokes.forEach(line);
      g.lineCap = "square"; g.lineJoin = "miter";
      line(live.pts);
      g.lineCap = "round"; g.lineJoin = "round";
      g.strokeStyle = "#111";
      for (const m of mine) if (m.state !== "gone") line(m.pts);
      g.restore();
      undoBtn.disabled = !mine.some((m) => m.state !== "gone" && m.state !== "drawing");
      clearMineBtn.disabled = undoBtn.disabled;
    });
  }

  // --- Sending ---
  async function pump() {
    while (queue.length && pending === 0) {
      const m = queue.shift();
      if (m.state === "gone") continue;
      pending++;
      try {
        const r = await canvasPost({ action: "stroke", p: encodePts(m.pts) });
        if (r.status === 200) m.state = "sent";
        else { m.state = "gone"; if (r.body.error === "turn over") queue = []; }
      } catch (e) { console.warn(e); m.state = "gone"; }
      pending--;
      paintPad();
    }
  }
  function finishLine() {
    if (!current) return;
    current.state = "queued";
    queue.push(current);
    current = null;
    pump();
  }

  async function undo(all) {
    // Lines not sent yet just go; sent ones are taken back on the server
    const live_ = mine.filter((m) => m.state === "queued" || m.state === "sent");
    if (!live_.length) return;
    const targets = all ? live_ : [live_[live_.length - 1]];
    const needServer = targets.some((m) => m.state === "sent");
    targets.forEach((m) => (m.state = m.state === "sent" ? "taking" : "gone"));
    paintPad();
    if (!needServer) return;
    await new Promise((r) => { const wait = () => (pending ? setTimeout(wait, 50) : r()); wait(); });
    const r = await canvasPost({ action: all ? "clear-mine" : "undo" });
    targets.forEach((m) => { if (m.state === "taking") m.state = r.status === 200 ? "gone" : "sent"; });
    if (r.status === 200 && r.body.cv) await syncCanvas({ open: r.body.open, cv: r.body.cv, n: r.body.n, wn: r.body.wn, tv: r.body.tv });
    paintPad();
  }
  undoBtn.addEventListener("click", () => undo(false));
  clearMineBtn.addEventListener("click", () => undo(true));
  fitBtn.addEventListener("click", () => { view.z = 1; clampPan(); paintPad(); });

  // --- Touch / mouse ---
  const local = (e) => { const r = padCanvas.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
  padCanvas.addEventListener("pointerdown", (e) => {
    if (!drawing) return;
    e.preventDefault();
    try { padCanvas.setPointerCapture(e.pointerId); } catch {}
    touches.set(e.pointerId, local(e));
    if (touches.size === 1) {
      current = { pts: [toFrame(...local(e))], state: "drawing", id: e.pointerId };
      mine.push(current);
    } else if (touches.size === 2) {
      // A second finger: this is a pinch, not a line. Drop a line that's barely begun.
      if (current) {
        if (current.pts.length < 6) { current.state = "gone"; mine = mine.filter((m) => m !== current); current = null; }
        else finishLine();
      }
      const [a, b] = [...touches.values()];
      pinch = { d: Math.hypot(a[0] - b[0], a[1] - b[1]) || 1, mid: [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2], z: view.z, tx: view.tx, ty: view.ty };
      padHint.classList.add("gone");
    }
    paintPad();
  });
  padCanvas.addEventListener("pointermove", (e) => {
    if (!drawing || !touches.has(e.pointerId)) return;
    e.preventDefault();
    touches.set(e.pointerId, local(e));
    if (pinch && touches.size >= 2) {
      const [a, b] = [...touches.values()];
      const d = Math.hypot(a[0] - b[0], a[1] - b[1]) || 1;
      const mid = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];
      const z = Math.min(6, Math.max(1, pinch.z * d / pinch.d));
      // the point under the fingers' midpoint stays under it
      view.z = z;
      view.tx = mid[0] - ((pinch.mid[0] - pinch.tx) / pinch.z) * z;
      view.ty = mid[1] - ((pinch.mid[1] - pinch.ty) / pinch.z) * z;
      clampPan();
      paintPad();
      return;
    }
    if (current && e.pointerId === current.id) {
      const p = toFrame(...local(e)), last = current.pts[current.pts.length - 1];
      if (Math.abs(p[0] - last[0]) + Math.abs(p[1] - last[1]) < 2 / view.z) return;
      current.pts.push(p);
      paintPad();
    }
  });
  const lift = (e) => {
    if (!touches.has(e.pointerId)) return;
    touches.delete(e.pointerId);
    if (current && e.pointerId === current.id) finishLine();
    if (touches.size < 2) pinch = null;
  };
  padCanvas.addEventListener("pointerup", lift);
  padCanvas.addEventListener("pointercancel", lift);
  padCanvas.addEventListener("wheel", (e) => {
    if (!drawing) return;
    e.preventDefault();
    const [mx, my] = local(e);
    const z = Math.min(6, Math.max(1, view.z * Math.exp(-e.deltaY * 0.002)));
    view.tx = mx - ((mx - view.tx) / view.z) * z;
    view.ty = my - ((my - view.ty) / view.z) * z;
    view.z = z;
    clampPan();
    paintPad();
  }, { passive: false });
  // iOS Safari pinch-zooms the page itself unless told not to
  const noGesture = (e) => { if (drawing) e.preventDefault(); };
  document.addEventListener("gesturestart", noGesture);
  document.addEventListener("gesturechange", noGesture);
  window.addEventListener("resize", () => { if (drawing) layoutPad(); });

  // --- The turn ---
  async function startTurn() {
    const r = await canvasPost({ action: "start" });
    if (r.status === 429) {
      cooldownUntil = Date.now() + (r.body.wait || 0);
      try { localStorage.setItem("cooldownUntil", cooldownUntil); } catch {}
      renderDrawBar();
      return;
    }
    if (r.status !== 200) { alert(r.body.error === "closed" ? "The canvas just closed." : "Couldn't start - try again."); return; }
    drawing = true;
    turnEnd = Date.now() + r.body.ms;
    mine = []; queue = []; current = null; touches.clear(); pinch = null;
    view.z = 1; view.tx = view.ty = 0;
    document.body.classList.add("drawing");
    pad.hidden = false;
    drawBar.hidden = true;
    padHint.classList.remove("gone");
    setTimeout(() => padHint.classList.add("gone"), 4000);
    try {
      if (matchMedia("(pointer: coarse)").matches && pad.requestFullscreen) await pad.requestFullscreen({ navigationUI: "hide" });
    } catch {}
    layoutPad();
    tick();
    clearTimeout(livePollTimer);
    pollLive();
  }
  function tick() {
    const left = Math.max(0, turnEnd - Date.now());
    const s = Math.ceil(left / 1000);
    timerEl.textContent = `0:${String(s).padStart(2, "0")}`;
    timerEl.classList.toggle("low", s <= 5);
    if (left <= 0) return endTurn();
    turnTimer = setTimeout(tick, 250);
  }
  async function endTurn() {
    if (!drawing) return;
    clearTimeout(turnTimer);
    finishLine();
    drawing = false;
    pad.hidden = true;
    document.body.classList.remove("drawing");
    if (!adminKey(false)) {
      cooldownUntil = turnEnd + 120_000;
      try { localStorage.setItem("cooldownUntil", cooldownUntil); } catch {}
    }
    try { if (document.fullscreenElement) await document.exitFullscreen(); } catch {}
    renderDrawBar();
    if (index === 0) renderLive();
  }
  drawBtn.addEventListener("click", () => {
    if (typeof dialog.showModal === "function") dialog.showModal();
    else if (confirm("You get 30 seconds to draw, then a 2 minute cooldown. Start?")) startTurn();
  });
  dialog.addEventListener("close", () => { if (dialog.returnValue === "go") startTurn(); });
  doneBtn.addEventListener("click", endTurn);

  // --- Admin panel (?admin) -----------------------------------------------------
  const adminEl = $("admin"), tplSelect = $("tpl-select"), tplFile = $("tpl-file");
  const canvasState = $("canvas-state"), canvasToggle = $("canvas-toggle"), adminNote = $("admin-note");
  let templates = [], uploaded = null;

  function renderAdmin() {
    if (!admin) return;
    canvasState.textContent = canv.open ? `open · ${canv.wn} line${canv.wn === 1 ? "" : "s"}` : "closed";
    canvasState.classList.toggle("open", canv.open);
    canvasToggle.textContent = canv.open ? "Close canvas" : "Open canvas";
  }
  async function adminAction(body, okText) {
    const key = adminKey(true).trim();
    if (!key) return;
    keepAdminKey(key);   // the request below reads it from here
    adminNote.textContent = "…";
    const r = await canvasPost(body, true);
    if (r.status === 401) { forgetAdminKey(); adminNote.textContent = "Wrong admin key - press again to retype it."; return; }
    if (r.status !== 200) { adminNote.textContent = "Didn't work: " + (r.body.error || r.status); return; }
    adminNote.textContent = okText;
    if (r.body.cv) await syncCanvas({ open: r.body.open, cv: r.body.cv, n: r.body.n, wn: r.body.wn, tv: r.body.tv });
    if (index === 0) renderLive();
  }

  // Pixels -> the badge's 1-bit template mask
  function packMask(on) {
    const out = new Uint8Array((TPL.w / 8) * TPL.h);
    for (let y = 0; y < TPL.h; y++) for (let x = 0; x < TPL.w; x++) {
      if (on(x, y)) out[y * (TPL.w / 8) + (x >> 3)] |= 0x80 >> (x & 7);
    }
    let bin = "";
    out.forEach((b) => (bin += String.fromCharCode(b)));
    return btoa(bin);
  }
  // A built-in template: its lines, 4 px wide
  function maskFromStrokes(strokes) {
    const c = document.createElement("canvas");
    c.width = TPL.w; c.height = TPL.h;
    const g = c.getContext("2d");
    g.translate(-TPL.x0, -TPL.y0);
    g.lineWidth = 4; g.lineCap = "round"; g.lineJoin = "round"; g.strokeStyle = "#000";
    for (const s of strokes) {
      g.beginPath();
      s.forEach(([x, y], i) => (i ? g.lineTo(x, y) : g.moveTo(x, y)));
      g.stroke();
    }
    const d = g.getImageData(0, 0, TPL.w, TPL.h).data;
    return packMask((x, y) => d[(y * TPL.w + x) * 4 + 3] > 110);
  }
  // An uploaded picture: fitted into the screen, and only the outlines of its
  // dark parts kept (thin lines stay whole, filled shapes become outlines)
  function maskFromImage(img) {
    const W = TPL.w, H = TPL.h, pad = 24;
    const c = document.createElement("canvas");
    c.width = W; c.height = H;
    const g = c.getContext("2d");
    g.fillStyle = "#fff"; g.fillRect(0, 0, W, H);
    const k = Math.min((W - 2 * pad) / img.width, (H - 2 * pad) / img.height);
    const w = img.width * k, h = img.height * k;
    g.drawImage(img, (W - w) / 2, (H - h) / 2, w, h);
    const d = g.getImageData(0, 0, W, H).data;
    const dark = new Uint8Array(W * H);
    for (let i = 0; i < W * H; i++) dark[i] = 0.299 * d[i * 4] + 0.587 * d[i * 4 + 1] + 0.114 * d[i * 4 + 2] < 128 ? 1 : 0;
    const R = 2;
    return packMask((x, y) => {
      if (!dark[y * W + x]) return false;
      for (let dy = -R; dy <= R; dy++) for (let dx = -R; dx <= R; dx++) {
        const xx = x + dx, yy = y + dy;
        if (xx < 0 || yy < 0 || xx >= W || yy >= H || !dark[yy * W + xx]) return true;
      }
      return false;
    });
  }

  // A picture -> lines for the canvas, drawn like everyone else's: fit it to the
  // screen, keep its dark parts (thin lines whole, filled shapes as outlines),
  // thin that to one-pixel centre lines (Zhang-Suen), follow them into paths
  // and straighten each path (Douglas-Peucker).
  function linesFromImage(img) {
    const W = SW + 1, H = SH + 1, pad = 18;
    const c = document.createElement("canvas");
    c.width = W; c.height = H;
    const g = c.getContext("2d");
    g.fillStyle = "#fff"; g.fillRect(0, 0, W, H);
    const k = Math.min((W - 2 * pad) / img.width, (H - 2 * pad) / img.height);
    const w = img.width * k, h = img.height * k;
    g.drawImage(img, (W - w) / 2, (H - h) / 2, w, h);
    const d = g.getImageData(0, 0, W, H).data;
    const dark = new Uint8Array(W * H);
    for (let i = 0; i < W * H; i++) dark[i] = 0.299 * d[i * 4] + 0.587 * d[i * 4 + 1] + 0.114 * d[i * 4 + 2] < 140 ? 1 : 0;
    // Filled areas become their outlines; lines up to ~6 px wide stay whole, so
    // thinning turns them into one line down the middle rather than two edges.
    // "Inside" = every pixel of the 7x7 square around it is dark (box sums).
    const R = 3, S = new Uint32Array((W + 1) * (H + 1));
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++)
      S[(y + 1) * (W + 1) + x + 1] = dark[y * W + x] + S[y * (W + 1) + x + 1] + S[(y + 1) * (W + 1) + x] - S[y * (W + 1) + x];
    const box = (x0, y0, x1, y1) => S[y1 * (W + 1) + x1] - S[y0 * (W + 1) + x1] - S[y1 * (W + 1) + x0] + S[y0 * (W + 1) + x0];
    const inside = new Uint8Array(W * H);
    for (let y = R; y < H - R; y++) for (let x = R; x < W - R; x++)
      if (box(x - R, y - R, x + R + 1, y + R + 1) === (2 * R + 1) ** 2) inside[y * W + x] = 1;
    // A dark pixel stays unless it's deep inside a filled area (keep a 2 px rim)
    const m = new Uint8Array(W * H);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const i = y * W + x;
      if (!dark[i]) continue;
      let deep = true;
      for (let dy = -2; dy <= 2 && deep; dy++) for (let dx = -2; dx <= 2; dx++) {
        const u = x + dx, v = y + dy;
        if (u < 0 || v < 0 || u >= W || v >= H || !inside[v * W + u]) { deep = false; break; }
      }
      if (!deep) m[i] = 1;
    }
    // Zhang-Suen thinning
    const at = (x, y) => m[y * W + x];
    for (let changed = true, pass = 0; changed && pass < 60; pass++) {
      changed = false;
      for (const step of [0, 1]) {
        const del = [];
        for (let y = 1; y < H - 1; y++) for (let x = 1; x < W - 1; x++) {
          if (!at(x, y)) continue;
          const p = [at(x, y - 1), at(x + 1, y - 1), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1), at(x - 1, y + 1), at(x - 1, y), at(x - 1, y - 1)];
          const b = p.reduce((a, v) => a + v, 0);
          if (b < 2 || b > 6) continue;
          let a = 0;
          for (let j = 0; j < 8; j++) if (!p[j] && p[(j + 1) % 8]) a++;
          if (a !== 1) continue;
          if (step === 0 ? (p[0] && p[2] && p[4]) || (p[2] && p[4] && p[6]) : (p[0] && p[2] && p[6]) || (p[0] && p[4] && p[6])) continue;
          del.push(y * W + x);
        }
        if (del.length) { changed = true; for (const i of del) m[i] = 0; }
      }
    }
    // Follow the thin lines: from each loose end first, then round any loops
    const N8 = [[1, 0], [0, 1], [-1, 0], [0, -1], [1, 1], [-1, 1], [-1, -1], [1, -1]];   // straight steps first
    const seen = new Uint8Array(W * H);
    const nbrs = (x, y) => N8.map(([dx, dy]) => [x + dx, y + dy]).filter(([u, v]) => u >= 0 && v >= 0 && u < W && v < H && m[v * W + u]);
    const paths = [];
    const walk = (x, y) => {
      const path = [[x, y]];
      seen[y * W + x] = 1;
      for (;;) {
        const next = nbrs(x, y).find(([u, v]) => !seen[v * W + u]);
        if (!next) {
          // Join up with a line already followed, if it ends right here
          const touch = nbrs(x, y).find(([u, v]) => path.length < 2 || u !== path[path.length - 2][0] || v !== path[path.length - 2][1]);
          if (touch && path.length > 2) path.push(touch);
          break;
        }
        [x, y] = next;
        seen[y * W + x] = 1;
        path.push([x, y]);
      }
      if (path.length >= 4) paths.push(path);
    };
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (m[y * W + x] && !seen[y * W + x] && nbrs(x, y).length === 1) walk(x, y);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (m[y * W + x] && !seen[y * W + x]) walk(x, y);
    // Straighten (keep corners), then into badge coordinates
    const rdp = (pts, eps) => {
      if (pts.length < 3) return pts;
      const [ax, ay] = pts[0], [bx, by] = pts[pts.length - 1];
      const len = Math.hypot(bx - ax, by - ay);
      let worst = 0, at_ = 0;
      for (let i = 1; i < pts.length - 1; i++) {
        // distance from the line between the ends - or from the end itself when
        // the path is a loop that comes back to where it started
        const dd = len < 0.5 ? Math.hypot(pts[i][0] - ax, pts[i][1] - ay)
          : Math.abs((bx - ax) * (ay - pts[i][1]) - (ax - pts[i][0]) * (by - ay)) / len;
        if (dd > worst) { worst = dd; at_ = i; }
      }
      if (worst <= eps) return [pts[0], pts[pts.length - 1]];
      return rdp(pts.slice(0, at_ + 1), eps).slice(0, -1).concat(rdp(pts.slice(at_), eps));
    };
    // Drop crumbs (a few pixels left where lines cross at sharp angles)
    const long = (p) => { let L = 0; for (let i = 1; i < p.length; i++) L += Math.hypot(p[i][0] - p[i - 1][0], p[i][1] - p[i - 1][1]); return L >= 8; };
    return paths.filter(long).map((p) => rdp(p, 0.75).map(([x, y]) => [x + SCREEN.x0, y + SCREEN.y0]));
  }

  if (admin) {
    adminEl.hidden = false;
    fetch("templates.json").then((r) => r.json()).then((list) => {
      templates = list;
      const upload = tplSelect.querySelector('option[value="upload"]');
      for (const t of list) {
        const o = document.createElement("option");
        o.value = t.id; o.textContent = t.name;
        tplSelect.insertBefore(o, upload);
      }
    });
    tplSelect.addEventListener("change", () => { if (tplSelect.value === "upload") tplFile.click(); });
    tplFile.addEventListener("change", async () => {
      const f = tplFile.files[0];
      if (!f) return;
      const img = new Image();
      img.src = URL.createObjectURL(f);
      await img.decode();
      uploaded = { name: f.name.replace(/\.[^.]+$/, "").slice(0, 40), img };
      const o = tplSelect.querySelector('option[value="upload"]');
      o.textContent = "Upload: " + uploaded.name;
      adminNote.textContent = "Ready to send " + f.name;
    });
    $("tpl-send").addEventListener("click", () => {
      const v = tplSelect.value;
      if (!v) return adminAction({ action: "template", m: "" }, "Template cleared.");
      if (v === "upload") {
        if (!uploaded) return tplFile.click();
        return adminAction({ action: "template", name: uploaded.name, m: maskFromImage(uploaded.img) }, `Sent ${uploaded.name} to the badge.`);
      }
      const t = templates.find((x) => x.id === v);
      return adminAction({ action: "template", name: t.name, m: maskFromStrokes(t.strokes) }, `Sent the ${t.name.toLowerCase()} to the badge.`);
    });
    $("tpl-clear").addEventListener("click", () => adminAction({ action: "template", m: "" }, "Template cleared."));
    canvasToggle.addEventListener("click", () =>
      adminAction({ action: canv.open ? "close" : "open" }, canv.open ? "Canvas closed." : "Canvas open: anyone on the site can draw."));
    const canvasFile = $("canvas-file");
    $("canvas-image").addEventListener("click", () => canvasFile.click());
    canvasFile.addEventListener("change", async () => {
      const f = canvasFile.files[0];
      canvasFile.value = "";
      if (!f) return;
      const img = new Image();
      img.src = URL.createObjectURL(f);
      try { await img.decode(); } catch { adminNote.textContent = "Couldn't read that image."; return; }
      adminNote.textContent = "Tracing " + f.name + "…";
      await new Promise((r) => setTimeout(r, 30));
      const lines = linesFromImage(img).filter((l) => l.length >= 2);
      if (!lines.length) { adminNote.textContent = "No dark lines found in " + f.name + "."; return; }
      if (!canv.open && !confirm("The canvas is closed. Add the picture anyway? It'll show on the badge now.")) { adminNote.textContent = ""; return; }
      await adminAction({ action: "import", s: lines.map(encodePts) }, `Added ${f.name} to the canvas as ${lines.length} line${lines.length === 1 ? "" : "s"}.`);
    });
    $("canvas-clear").addEventListener("click", () => {
      if (confirm("Wipe everyone's lines from the canvas? (The badge's own line stays.)")) adminAction({ action: "clear" }, "Canvas cleared.");
    });
    renderAdmin();
  }

  // --- Wiring ----------------------------------------------------------------
  leftBtn.addEventListener("click", () => go(index - 1));    // newer
  rightBtn.addEventListener("click", () => go(index + 1));   // older
  liveBtn.addEventListener("click", () => go(0));
  replayBtn.addEventListener("click", replay);
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && drawing) endTurn();
    else if (e.key === "ArrowLeft") go(index - 1);
    else if (e.key === "ArrowRight") go(index + 1);
  });
  document.addEventListener("visibilitychange", () => { if (!document.hidden) pollLive(); });
  setInterval(() => loadKeys().then(() => { rightBtn.disabled = index >= keys.length; if (index === 0) renderLive(); }), 30000);

  (async () => {
    await Promise.all([loadKeys(), pollLive()]);
    const want = decodeURIComponent(location.hash.slice(1));
    const i = keys.findIndex((k) => k.key === want);
    if (i >= 0) index = i + 1;
    else index = live.online || canv.open || !keys.length ? 0 : 1;   // open canvas: land where you can draw
    render();
  })();
})();
