// Hack-A-Sketch: the gallery of finished drawings plus a live slide.
//
// Slides run newest first, left to right: slides[0] is LIVE, slides[1] the
// newest drawing, the last one the oldest. The right knob steps back in time.
//
// The knobs turn with the pen like the badge's own: the left one drives x, the
// right one y, PX_PER_REV pixels per turn, clockwise = right / up.
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
  const pngLink = $("png"), delBtn = $("delete");
  const admin = new URLSearchParams(location.search).has("admin");

  let keys = [];            // [{key, ts, t}] newest first
  let index = 0;            // 0 = live, i = keys[i - 1]
  const cache = new Map();  // key -> {t, pts}
  let live = { id: "", pts: [], online: false, mode: "idle", seen: 0, seenText: "" };
  let shownPts = [];
  let anim = 0, replayTimer = 0;
  let livePollTimer = 0;

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
      cache.set(key, { t: d.t, pts: decode(d.p) });
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
      if (index === 0) renderLive();
    } catch (e) {
      console.warn(e);
    }
    clearTimeout(livePollTimer);
    const fast = index === 0 && live.online && !document.hidden;
    livePollTimer = setTimeout(pollLive, fast ? 2000 : 12000);
  }

  // --- Slides --------------------------------------------------------------
  function renderLive() {
    liveTag.setAttribute("visibility", live.online ? "visible" : "hidden");
    draw(live.pts, { showPen: live.online });
    empty.textContent = live.pts.length ? "" : live.online ? "Waiting for the knobs to turn…" : "The badge is offline";
    stamp.textContent = live.online ? "DRAWING NOW" : "LIVE VIEW";
    count.textContent = keys.length ? keys.length + " IN THE GALLERY ›" : "";
  }

  async function render() {
    leftBtn.disabled = index <= 0;
    rightBtn.disabled = index >= keys.length;
    liveBtn.disabled = index === 0;
    delBtn.hidden = !admin || index === 0;
    pngLink.removeAttribute("href");

    if (index === 0) {
      renderLive();
      history.replaceState(null, "", location.pathname + location.search);
      clearTimeout(livePollTimer);
      pollLive();
      return;
    }
    liveTag.setAttribute("visibility", "hidden");
    const k = keys[index - 1];
    stamp.textContent = k.t.toUpperCase();
    count.textContent = `${index} OF ${keys.length}`;
    empty.textContent = "";
    history.replaceState(null, "", "#" + k.key);
    try {
      const d = await getDrawing(k.key);
      if (keys[index - 1] !== k) return;   // moved on while loading
      draw(d.pts);
    } catch (e) {
      draw([]);
      empty.textContent = "Couldn't load this one";
    }
  }

  function go(to) {
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
    g.fillStyle = "#fff"; g.font = "700 30px system-ui, sans-serif"; g.textAlign = "center";
    g.fillText(stamp.textContent, 480, 606);
    return new Promise((resolve) => c.toBlob(resolve, "image/png"));
  }

  // Made fresh on every click, so it's always the drawing on screen right now
  pngLink.addEventListener("click", async (e) => {
    e.preventDefault();
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
    let key = "";
    try { key = localStorage.getItem("adminKey") || ""; } catch {}
    if (!key) key = prompt("Admin key") || "";
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

  // --- Wiring ----------------------------------------------------------------
  leftBtn.addEventListener("click", () => go(index - 1));    // newer
  rightBtn.addEventListener("click", () => go(index + 1));   // older
  liveBtn.addEventListener("click", () => go(0));
  replayBtn.addEventListener("click", replay);
  document.addEventListener("keydown", (e) => {
    if (e.key === "ArrowLeft") go(index - 1);
    else if (e.key === "ArrowRight") go(index + 1);
  });
  document.addEventListener("visibilitychange", () => { if (!document.hidden) pollLive(); });
  setInterval(() => loadKeys().then(() => { rightBtn.disabled = index >= keys.length; if (index === 0) renderLive(); }), 30000);

  (async () => {
    await Promise.all([loadKeys(), pollLive()]);
    const want = decodeURIComponent(location.hash.slice(1));
    const i = keys.findIndex((k) => k.key === want);
    if (i >= 0) index = i + 1;
    else index = live.online || !keys.length ? 0 : 1;
    render();
  })();
})();
