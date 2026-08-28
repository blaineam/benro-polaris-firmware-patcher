/* Polaris web control.
 *
 * Two rules shape everything here.
 *
 * 1. THE BROWSER NEVER DRIVES THE WIRE. A held button declares an intent with a
 *    short lease and renews it; the server owns the 50 ms repeat and the stop.
 *    So a locked phone, a dropped Wi-Fi link or a closed tab STOPS the head
 *    instead of abandoning it in motion. See polaris-jog.h.
 *
 * 2. NOTHING IS INVENTED. Exposure option lists come from the camera, because
 *    the set commands take an INDEX into the camera's own list — a hardcoded
 *    table would silently set the wrong shutter speed on a different body. A
 *    value we do not have is shown as "—", never as a plausible guess.
 */
'use strict';

var $  = function (s, r) { return (r || document).querySelector(s); };
var $$ = function (s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); };

/* ─────────────────────────────── transport ─────────────────────────────── */

function post(path, params) {
  var body = Object.keys(params || {}).map(function (k) {
    return encodeURIComponent(k) + '=' + encodeURIComponent(params[k]);
  }).join('&');
  return fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: body,
    /* A control request that queues behind a stale one is worse than a lost
       one: the head would act on an intention the user has already abandoned. */
    cache: 'no-store'
  }).then(function (r) { return r.json().catch(function () { return { ok: r.ok }; }); });
}

function send(cmd, args, confirm) {
  return post('/api/link/send', { cmd: cmd, args: args || '', confirm: confirm ? 1 : 0 });
}

/* Parse the head's "k:v;k:v;" payload. */
function kv(s) {
  var out = {};
  (s || '').split(';').forEach(function (p) {
    var i = p.indexOf(':');
    if (i > 0) out[p.slice(0, i).trim()] = p.slice(i + 1).trim();
  });
  return out;
}

/* ──────────────────────────────── state ────────────────────────────────── */

var S = {
  link: 'down',
  ops: {},          /* opcode -> {args, age_ms, count} */
  gear: 3,
  live: false,
  recording: false,
  expoLoaded: false,
  lastExpoFetch: 0
};

function setHint(html) {
  var el = $('#hint');
  if (!html) { el.hidden = true; el.innerHTML = ''; return; }
  if (el.innerHTML !== html) el.innerHTML = html;
  el.hidden = false;
}

/* ─────────────────────────────── polling ───────────────────────────────── */

/* One request per frame of UI. The head pushes telemetry continuously into the
   server's cache, so this is a cheap read of memory, not a round trip to the
   mount. */
function poll() {
  fetch('/api/link', { cache: 'no-store' })
    .then(function (r) { return r.json(); })
    .then(render)
    .catch(function () {
      S.link = 'down';
      paintLink({ status: 'down', error: 'the server is not answering' });
    })
    .then(function () { setTimeout(poll, S.link === 'up' ? 500 : 1500); });
}

function render(d) {
  S.link = d.status;
  S.ops = d.opcodes || {};
  paintLink(d);
  paintStatus();
  paintMount();
  paintLinkStats(d);
  if (S.link === 'up' && !S.expoLoaded && Date.now() - S.lastExpoFetch > 3000) fetchExposure();
  if (S.link !== 'up') { S.expoLoaded = false; }
}

function paintLink(d) {
  var dot = $('#linkdot'), txt = $('#linktext');
  dot.className = 'dot ' + (d.status === 'up' ? 'up' : d.status === 'connecting' ? 'busy' : 'down');
  txt.textContent = d.status === 'up' ? 'connected'
                  : d.status === 'connecting' ? 'connecting…' : 'not connected';

  var moveable = d.status === 'up';
  $$('.jog, .recentre').forEach(function (b) { b.disabled = !moveable; });
  $('#shoot').disabled = !moveable;
  $('#rec').disabled = !moveable;
  $('#level').disabled = !moveable;

  if (d.status === 'up') { setHint(''); return; }
  /* Every blocking condition carries its own fix — the app's hint-banner
     pattern, which is the thing worth copying about it. */
  if (d.wanted === false) {
    setHint('<b>The control link is closed.</b> It stays closed until the mount ' +
            'is aligned, because connecting while unaligned makes the Benro app ' +
            'ask for a compass calibration. Align in the app, or use the solver ' +
            'on the <a href="/legacy">Astro page</a>.');
  } else {
    setHint('<b>Connecting to the head.</b> ' +
            (d.error ? String(d.error) : 'If this does not clear, the head may be asleep — ' +
             'wake it and the link will come back on its own.'));
  }
}

function fmtBytes(n) {
  n = Number(n);
  if (!isFinite(n) || n <= 0) return '—';
  var u = ['B', 'KB', 'MB', 'GB', 'TB'], i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return (n >= 10 || i === 0 ? Math.round(n) : n.toFixed(1)) + ' ' + u[i];
}

function paintStatus() {
  /* Battery: 778, keys capacity + charge. */
  var b = S.ops['778'], el = $('#battery');
  if (b) {
    var a = kv(b.args), pct = parseInt(a.capacity, 10);
    if (isFinite(pct)) {
      el.hidden = false;
      el.textContent = (a.charge && a.charge !== '0' ? '⚡ ' : '') + pct + '%';
      el.className = 'chip' + (pct <= 16 ? ' err' : pct <= 20 ? ' warn' : '');
    }
  } else el.hidden = true;

  /* Card: 775, keys status/totalspace/freespace/usespace. */
  var c = S.ops['775'], cel = $('#card');
  if (c) {
    var s = kv(c.args), free = Number(s.freespace);
    cel.hidden = false;
    cel.textContent = isFinite(free) && free > 0 ? fmtBytes(free) + ' free' : 'no card';
    cel.className = 'chip' + (!isFinite(free) || free <= 0 ? ' warn' : '');
  } else cel.hidden = true;

  /* Camera identity: 286. Shown as-is; the payload shape varies by body, so
     the whole thing is displayed rather than a field being guessed at. */
  var cam = S.ops['286'];
  $('#camname').textContent = cam ? (kv(cam.args).name || cam.args.replace(/;$/, '')) : '';
}

function paintMount() {
  var dl = $('#mountstate');
  if (!dl) return;
  var pose = S.ops['518'], mode = S.ops['284'];
  var rows = [];
  if (pose) {
    var p = kv(pose.args);
    /* 518's `alt` is positive-DOWNWARD (docs/APP-PROTOCOL.md); negate it so
       the page shows altitude the way a human means it. */
    if (p.alt !== undefined) rows.push(['Altitude', (-parseFloat(p.alt)).toFixed(3) + '°']);
    if (p.compass !== undefined) rows.push(['Compass', parseFloat(p.compass).toFixed(3) + '°']);
  }
  if (mode) {
    var m = kv(mode.args);
    var names = { 1: 'Photo', 7: 'Sun', 8: 'Astro', 9: 'Free program', 10: 'Video' };
    if (m.mode !== undefined) rows.push(['Mode', names[m.mode] || ('mode ' + m.mode)]);
    if (m.track !== undefined) rows.push(['Tracking', m.track === '1' ? 'on' : 'off']);
  }
  if (!rows.length) rows.push(['Mount', 'no telemetry yet']);
  dl.innerHTML = rows.map(function (r) {
    return '<dt>' + r[0] + '</dt><dd>' + r[1] + '</dd>';
  }).join('');
}

function paintLinkStats(d) {
  var dl = $('#linkstats');
  if (!dl || $('#tab-settings').hidden) return;
  var rows = [
    ['Status', d.status],
    ['Connects', d.connects], ['Drops', d.drops],
    ['Frames in', d.rx], ['Frames out', d.tx],
    ['Parse errors', d.parse_errors],
    ['Last frame', d.last_rx_age_ms >= 0 ? Math.round(d.last_rx_age_ms) + ' ms ago' : '—']
  ];
  if (d.error) rows.push(['Last error', d.error]);
  dl.innerHTML = rows.map(function (r) {
    return '<dt>' + r[0] + '</dt><dd>' + r[1] + '</dd>';
  }).join('');
}

/* ──────────────────────────────── motion ───────────────────────────────── */

/* Renew comfortably inside the server's 400 ms lease. Not so fast that a busy
   2.4 GHz AP is flooded, not so slow that one dropped request stops the head
   mid-gesture. */
var RENEW_MS = 140;
var holds = {};    /* axis -> {timer, dir} */

/* Map the 1..5 gear to the head's fast-jog range. The head ignores magnitudes
   under ~100 and clips over 2500, and the server clamps too — this only has to
   feel right. */
function speedForGear(g) {
  var table = { 1: 180, 2: 420, 3: 900, 4: 1600, 5: 2500 };
  return table[g] || 900;
}

/* Every jog — buttons, keys, sticks — funnels through moveAxis so the 400 ms
   lease is renewed one way. `affirm` is the sole sender; the per-axis timer is a
   keepalive that fires only when a live speed change hasn't already sent. */
function affirm(axis) {
  var h = holds[axis];
  if (!h) return;
  h.last = Date.now();
  post('/api/move', { axis: axis, speed: h.speed });
}

/* Set an axis's signed speed (0 stops it). Creates/updates the hold and its
   keepalive, sending promptly on a change but throttled so a drag can't flood
   the AP. `dir` (±1) is stored for button/key holds so a gear change can
   recompute; a stick passes no dir and re-derives speed on its next move. */
function moveAxis(axis, speed, btn, dir) {
  if (S.link !== 'up') return;
  speed = Math.round(speed) || 0;
  if (!speed) { endHold(axis); return; }
  var h = holds[axis], fresh = false;
  if (!h) {
    fresh = true;
    h = holds[axis] = { speed: 0, last: 0, btn: null, dir: 0,
      timer: setInterval(function () {
        var hh = holds[axis];
        if (hh && Date.now() - hh.last >= RENEW_MS) affirm(axis);
      }, RENEW_MS) };
  }
  if (btn) { h.btn = btn; btn.classList.add('held'); }
  if (dir !== undefined) h.dir = dir;
  var changed = h.speed !== speed;
  h.speed = speed;
  if (fresh || (changed && Date.now() - h.last >= 70)) affirm(axis);
}

/* Buttons + keyboard: a fixed speed from the gear, one direction. */
function startHold(axis, dir, btn) {
  if (S.link !== 'up') return;
  moveAxis(axis, dir * speedForGear(S.gear), btn, dir);
  if (navigator.vibrate) navigator.vibrate(8);
}

function endHold(axis) {
  var h = holds[axis];
  if (!h) return;
  clearInterval(h.timer);
  if (h.btn) h.btn.classList.remove('held');
  delete holds[axis];
  /* Tell the server explicitly. The lease would expire on its own in 400 ms,
     but "the user let go" should feel immediate, and a stop is idempotent. */
  post('/api/move', { axis: axis, speed: 0 });
}

function resetSticks() {
  $$('.stick').forEach(function (s) {
    s.classList.remove('active');
    var k = $('.knob', s); if (k) k.style.transform = 'translate(0,0)';
  });
}

function stopEverything() {
  Object.keys(holds).forEach(function (a) {
    clearInterval(holds[a].timer);
    if (holds[a].btn) holds[a].btn.classList.remove('held');
    delete holds[a];
  });
  resetSticks();
  post('/api/stop', {});
}

/* Analog joysticks — displacement -> direction AND speed, on the same lease and
   dead-man as the pads. Past its dead-zone a stick maps to [SPEED_MIN, the
   gear's top speed], so the speed slider caps sensitivity. */
var STICK_DEAD  = 0.14;   /* fraction of travel ignored around the centre       */
var STICK_EXPO  = 1.9;    /* >1 = fine control near centre, ramps up at the edge */

function stickSpeed(frac, maxSpeed) {
  var a = Math.abs(frac);
  if (a <= STICK_DEAD) return 0;
  var f = (a - STICK_DEAD) / (1 - STICK_DEAD);          /* 0..1 past the dead-zone */
  /* Expo curve: a small push barely moves (fine framing), and only the top of
     the throw reaches full speed. Astro framing needs the slow end usable — a
     linear map spends most of the travel too fast to place a star. */
  f = Math.pow(f, STICK_EXPO);
  var mag = maxSpeed <= 100 ? maxSpeed : 100 + f * (maxSpeed - 100);
  return (frac < 0 ? -1 : 1) * mag;
}

function wireSticks() {
  $$('.stick').forEach(function (stick) {
    var kind = stick.dataset.stick;      /* 'xy' or 'rot' */
    var knob = $('.knob', stick);
    var active = false, pid = null, lastTap = 0;

    function place(nx, ny) {
      var r = stick.clientWidth / 2 - knob.clientWidth / 2;
      knob.style.transform = 'translate(' + (nx * r).toFixed(1) + 'px,' + (ny * r).toFixed(1) + 'px)';
    }

    function update(e) {
      var rect = stick.getBoundingClientRect();
      var rad = rect.width / 2;
      var nx = (e.clientX - (rect.left + rect.width / 2)) / rad;
      var ny = (e.clientY - (rect.top + rect.height / 2)) / rad;
      if (kind === 'xy') {
        var m = Math.hypot(nx, ny); if (m > 1) { nx /= m; ny /= m; }   /* clamp to the circle */
        place(nx, ny);
        var maxS = speedForGear(S.gear);
        moveAxis('pan', stickSpeed(nx, maxS));
        moveAxis('tilt', stickSpeed(-ny, maxS));      /* screen-up = tilt up */
      } else {
        if (nx > 1) nx = 1; if (nx < -1) nx = -1;
        place(nx, 0);
        moveAxis('rot', stickSpeed(nx, speedForGear(S.gear)));
      }
    }

    function release() {
      if (!active) return;
      active = false; pid = null;
      stick.classList.remove('active');
      knob.style.transform = 'translate(0,0)';        /* snap back to centre */
      if (kind === 'xy') { endHold('pan'); endHold('tilt'); }
      else endHold('rot');
    }

    stick.addEventListener('pointerdown', function (e) {
      e.preventDefault();
      if (S.link !== 'up') return;
      /* Double-tap a stick re-centres its axis (523), like the app. */
      var now = Date.now();
      if (now - lastTap < 300) {
        lastTap = 0;
        if (kind === 'xy') { send(523, 'axis:1;', true); send(523, 'axis:2;', true); }
        else send(523, 'axis:3;', true);
        if (navigator.vibrate) navigator.vibrate([8, 40, 8]);
        return;
      }
      lastTap = now;
      active = true; pid = e.pointerId;
      try { stick.setPointerCapture(pid); } catch (_) {}
      stick.classList.add('active');
      if (navigator.vibrate) navigator.vibrate(8);
      update(e);
    });
    stick.addEventListener('pointermove', function (e) {
      if (active && e.pointerId === pid) update(e);
    });
    /* Release ONLY on up/cancel — a joystick finger is allowed to leave the
       widget's bounds mid-drag, so pointerleave must NOT stop it. */
    ['pointerup', 'pointercancel'].forEach(function (ev) {
      stick.addEventListener(ev, release);
    });
    stick.addEventListener('contextmenu', function (e) { e.preventDefault(); });
  });
}

function wireJog() {
  $$('.pad').forEach(function (pad) {
    var axis = pad.dataset.axis;
    $$('.jog', pad).forEach(function (btn) {
      var dir = parseInt(btn.dataset.dir, 10);
      /* Pointer events cover mouse, touch and pen with one path, and
         setPointerCapture means a finger that slides off the button still
         delivers its release here — without it, sliding off leaves the axis
         running until the lease expires. */
      btn.addEventListener('pointerdown', function (e) {
        e.preventDefault();
        if (btn.disabled) return;
        try { btn.setPointerCapture(e.pointerId); } catch (_) {}
        startHold(axis, dir, btn);
      });
      ['pointerup', 'pointercancel', 'pointerleave'].forEach(function (ev) {
        btn.addEventListener(ev, function () { endHold(axis); });
      });
      btn.addEventListener('contextmenu', function (e) { e.preventDefault(); });
    });
    $$('.recentre', pad).forEach(function (btn) {
      btn.addEventListener('click', function () {
        /* 523 SP_GIMBAL_POS_RESET moves the head, so it is confirmed. */
        send(523, 'axis:' + btn.dataset.axisId + ';', true);
      });
    });
  });

  /* Keyboard: free on a browser, impossible on the phone app. */
  var keymap = {
    ArrowLeft:  ['pan', -1], ArrowRight: ['pan', 1],
    ArrowUp:    ['tilt', 1], ArrowDown:  ['tilt', -1],
    ',':        ['rot', -1], '.':        ['rot', 1]
  };
  document.addEventListener('keydown', function (e) {
    if (e.repeat || e.target.matches('input,select,textarea')) return;
    if (e.key === 'Escape') { stopEverything(); return; }
    var m = keymap[e.key];
    if (!m) return;
    e.preventDefault();
    startHold(m[0], m[1], null);
  });
  document.addEventListener('keyup', function (e) {
    var m = keymap[e.key];
    if (m) endHold(m[0]);
  });

  /* THE GUARANTEED RELEASE. The server's lease already covers a client that
     dies without warning; these make the common cases instant rather than
     leaving the head moving for up to 400 ms. */
  ['blur', 'pagehide', 'beforeunload'].forEach(function (ev) {
    window.addEventListener(ev, stopEverything);
  });
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) stopEverything();
  });

  $('#estop').addEventListener('click', stopEverything);

  var gear = $('#gear');
  gear.addEventListener('input', function () {
    S.gear = parseInt(gear.value, 10);
    $('#gearout').textContent = S.gear;
    /* Re-affirm any in-flight BUTTON/key hold at the new top speed rather than
       waiting for the next renewal, so the slider feels live under a held
       button. Stick holds (dir 0) re-derive speed on their next move. */
    Object.keys(holds).forEach(function (axis) {
      var h = holds[axis];
      if (h.dir) moveAxis(axis, h.dir * speedForGear(S.gear));
    });
  });

  $('#level').addEventListener('click', function () {
    /* 549 actively levels the head — motion, so confirmed. */
    send(549, 'state:1;', true);
  });
}

/* ────────────────────────────── live view ──────────────────────────────── */

/* The stream is mjpg-streamer on the head's port 8080, not this server. It is
   referenced directly rather than proxied: proxying it through a
   single-threaded server would block every other request for as long as the
   stream is open, which is forever. */
function streamURL() {
  return 'http://' + location.hostname + ':8080/?action=stream';
}

function setLive(on) {
  var img = $('#live'), msg = $('#livemsg'), btn = $('#previewtoggle');
  S.live = on;
  btn.classList.toggle('on', on);
  if (on) {
    msg.textContent = 'starting live view…';
    /* Ask the head to turn its preview on, then attach the stream. */
    send(291, 'state:1;').then(function () {
      img.onload = function () { img.classList.add('on'); msg.textContent = ''; };
      img.onerror = function () {
        img.classList.remove('on');
        msg.innerHTML = 'Live view did not load from <span class="mono">' +
          streamURL().replace(/</g, '&lt;') + '</span>.<br>' +
          'The camera must be awake and in a mode that allows preview.';
      };
      img.src = streamURL() + '&t=' + Date.now();
    });
  } else {
    img.classList.remove('on');
    img.removeAttribute('src');      /* actually closes the connection */
    msg.textContent = 'live view off';
    send(291, 'state:0;');
  }
}

/* ── stage: accessibility size + fullscreen preview ──
   The size button cycles Normal -> Large -> Huge, scaling the stage, the
   joysticks and the text together — the whole point of a browser control for
   low-vision use. The choice persists. Fullscreen keeps the joysticks and
   shutter overlaid, so you can frame and jog on a big image at once. */
function applySize(sz) {
  var root = document.documentElement;
  if (sz === 'lg' || sz === 'xl') { sz = 'lg'; root.setAttribute('data-size', 'lg'); }
  else { root.removeAttribute('data-size'); sz = ''; }
  var b = $('#sizebtn');
  if (b) { b.textContent = sz === 'lg' ? 'A+' : 'A'; b.classList.toggle('on', sz === 'lg'); }
  try { localStorage.setItem('polaris-size', sz); } catch (_) {}
  resetSticks();   /* a size change moves the sticks — recentre any knob */
  return sz;
}

function wireStage() {
  var order = ['', 'lg'], cur = '';
  try { cur = localStorage.getItem('polaris-size') || ''; } catch (_) {}
  cur = applySize(cur);

  $('#sizebtn').addEventListener('click', function () {
    cur = applySize(order[(order.indexOf(cur) + 1) % order.length]);
  });

  var stage = $('#stage');
  var fsOn = function () { return !!(document.fullscreenElement || document.webkitFullscreenElement); };
  $('#fsbtn').addEventListener('click', function () {
    if (fsOn()) { (document.exitFullscreen || document.webkitExitFullscreen || function(){}).call(document); }
    else { (stage.requestFullscreen || stage.webkitRequestFullscreen || function(){}).call(stage); }
  });
  ['fullscreenchange', 'webkitfullscreenchange'].forEach(function (ev) {
    document.addEventListener(ev, function () { $('#fsbtn').classList.toggle('on', fsOn()); });
  });
}

/* ── framing grid · focus peaking · live histogram ──
   The grid is a pure CSS overlay. Peaking and the histogram need real pixels,
   and the :8080 MJPEG is cross-origin — a canvas that draws it is tainted and
   getImageData() throws. So they run off /api/snapshot: one SAME-ORIGIN still a
   second, analysed on a small offscreen canvas, drawn back over the frame. The
   still shares the live view's field of view, so peaking marks line up. */
var peakOn = false, peakTimer = 0, peakImg = null;

function drawHistogram(ctx, hist, hmax) {
  var c = ctx.canvas, W = c.width, H = c.height, i, h;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = 'rgba(0,0,0,.5)'; ctx.fillRect(0, 0, W, H);
  ctx.fillStyle = 'rgba(120,220,255,.9)';
  for (i = 0; i < 256; i++) {
    h = hmax > 0 ? Math.round(Math.log(1 + hist[i]) / Math.log(1 + hmax) * (H - 2)) : 0;
    if (h > 0) ctx.fillRect(i, H - h, 1, h);
  }
}

/* Draw one snapshot into a small buffer, then compute the histogram (into #hist)
   and a gradient-magnitude "peaking" glow (into #peak, over the frame). */
function analyseFrame(img) {
  var iw = img.naturalWidth, ih = img.naturalHeight, i, p, L;
  if (!iw || !ih) return;
  var aw = Math.min(iw, 512), ah = Math.round(ih * aw / iw);
  var off = analyseFrame._c || (analyseFrame._c = document.createElement('canvas'));
  off.width = aw; off.height = ah;
  var octx = off.getContext('2d'), data;
  octx.drawImage(img, 0, 0, aw, ah);
  try { data = octx.getImageData(0, 0, aw, ah).data; } catch (_) { return; }

  var n = aw * ah, lum = new Uint8Array(n), hist = new Uint32Array(256), hmax = 0;
  for (i = 0, p = 0; i < n; i++, p += 4) {
    L = (data[p] * 77 + data[p + 1] * 150 + data[p + 2] * 29) >> 8;
    lum[i] = L; hist[L]++;
  }
  for (i = 1; i < 255; i++) if (hist[i] > hmax) hmax = hist[i];   /* ignore 0/255 spikes */
  var hc = $('#hist'); if (hc) drawHistogram(hc.getContext('2d'), hist, hmax);

  var pk = $('#peak'); pk.width = aw; pk.height = ah;
  var pctx = pk.getContext('2d'), out = pctx.createImageData(aw, ah), o = out.data;
  var x, y, g, gx, gy, idx, a, FLOOR = 24, RANGE = 60;
  for (y = 1; y < ah - 1; y++) {
    for (x = 1; x < aw - 1; x++) {
      idx = y * aw + x;
      gx = lum[idx + 1] - lum[idx - 1]; if (gx < 0) gx = -gx;
      gy = lum[idx + aw] - lum[idx - aw]; if (gy < 0) gy = -gy;
      g = gx + gy;
      if (g > FLOOR) {
        a = g - FLOOR; if (a > RANGE) a = RANGE;
        p = idx * 4;
        o[p] = 130; o[p + 1] = 255; o[p + 2] = 100; o[p + 3] = Math.round(a / RANGE * 220);
      }
    }
  }
  pctx.putImageData(out, 0, 0);
}

function peakTick() {
  if (!peakOn) return;
  if (!peakImg) peakImg = new Image();
  peakImg.onload = function () { try { analyseFrame(peakImg); } catch (_) {} };
  peakImg.onerror = function () {};
  peakImg.src = '/api/snapshot?t=' + Date.now();
}

function setPeak(on) {
  peakOn = on;
  $('#peakbtn').classList.toggle('on', on);
  $('#peak').hidden = !on;
  $('#histbox').hidden = !on;
  try { localStorage.setItem('polaris-peak', on ? '1' : ''); } catch (_) {}
  if (on) { peakTick(); if (!peakTimer) peakTimer = setInterval(peakTick, 1000); }
  else { clearInterval(peakTimer); peakTimer = 0; }
}

function wireOverlays() {
  var gridOn = false;
  try { gridOn = localStorage.getItem('polaris-grid') === '1'; } catch (_) {}
  $('#grid').hidden = !gridOn;
  $('#gridbtn').classList.toggle('on', gridOn);
  $('#gridbtn').addEventListener('click', function () {
    gridOn = !gridOn;
    $('#grid').hidden = !gridOn;
    this.classList.toggle('on', gridOn);
    try { localStorage.setItem('polaris-grid', gridOn ? '1' : ''); } catch (_) {}
  });
  $('#peakbtn').addEventListener('click', function () { setPeak(!peakOn); });
  var want = false;
  try { want = localStorage.getItem('polaris-peak') === '1'; } catch (_) {}
  if (want) setPeak(true);
}

/* ─────────────────────────────── exposure ──────────────────────────────── */

/* Reply shape, from the app's own parser (PolarisOrderCommunication:
   parseSP_GET_*_INFO):
       RD:<0|1>;V:<selectedIndex>;R:<comma,separated,options>;
   RD IS INVERTED: "0" means the control is AVAILABLE. Reading it the obvious
   way disables every control on a working camera. */
function parseOptions(args) {
  var a = kv(args);
  if (a.R === undefined) return null;
  return {
    available: a.RD === '0',
    index: parseInt(a.V, 10) || 0,
    options: a.R.split(',').map(function (o) {
      return /auto/i.test(o) ? 'Auto' : o;
    })
  };
}

function fetchExposure() {
  S.lastExpoFetch = Date.now();
  /* The app refreshes all five together, rate-limited to once per 2 s. */
  var gets = $$('.exposlot').map(function (slot) { return slot.dataset.get; });
  Promise.all(gets.map(function (g) { return send(g, ''); }))
    .then(function () { setTimeout(paintExposure, 700); });
}

function paintExposure() {
  var any = false;
  $$('.exposlot').forEach(function (slot) {
    var got = S.ops[slot.dataset.get];
    var sel = $('select', slot);
    if (!got) { sel.disabled = true; return; }
    var parsed = parseOptions(got.args);
    if (!parsed || !parsed.options.length) { sel.disabled = true; return; }
    any = true;
    var sig = parsed.options.join('|');
    if (sel.dataset.sig !== sig) {
      sel.dataset.sig = sig;
      sel.innerHTML = parsed.options.map(function (o, i) {
        return '<option value="' + i + '">' + o.replace(/</g, '&lt;') + '</option>';
      }).join('');
    }
    /* Do not fight the user: only adopt the head's index while the control is
       not focused, or a poll lands mid-selection and snaps the value back. */
    if (document.activeElement !== sel) sel.value = String(parsed.index);
    sel.disabled = !parsed.available;
  });
  if (any) S.expoLoaded = true;
}

function wireExposure() {
  $$('.exposlot').forEach(function (slot) {
    $('select', slot).addEventListener('change', function (e) {
      /* Set by INDEX into the camera's own list — never by the label. */
      send(slot.dataset.set, slot.dataset.key + ':' + e.target.value + ';')
        .then(function () { setTimeout(fetchExposure, 400); });
    });
  });
}

/* ──────────────────────────────── shutter ──────────────────────────────── */

/* The app uses slide-to-fire. The intent — "you cannot do this by brushing the
   screen" — is right; the mechanic is awkward. Hold-to-fire keeps the guard and
   is easier one-handed in the dark, and it maps onto a keyboard too. */
var HOLD_MS = 450;

function wireShutter() {
  var btn = $('#shoot'), t0 = 0, raf = 0, timer = 0;

  function progress() {
    var p = Math.min(1, (Date.now() - t0) / HOLD_MS);
    btn.style.setProperty('--progress', (p * 100) + '%');
    if (p < 1) raf = requestAnimationFrame(progress);
  }
  function begin(e) {
    if (btn.disabled) return;
    e.preventDefault();
    try { btn.setPointerCapture(e.pointerId); } catch (_) {}
    t0 = Date.now();
    btn.classList.add('arming');
    raf = requestAnimationFrame(progress);
    timer = setTimeout(fire, HOLD_MS);
  }
  function cancel() {
    clearTimeout(timer); cancelAnimationFrame(raf);
    btn.classList.remove('arming');
    btn.style.setProperty('--progress', '0%');
  }
  function fire() {
    cancel();
    btn.classList.add('fired');
    if (navigator.vibrate) navigator.vibrate(24);
    /* 264 SP_SET_PHOTO_RECORD_STATUS: state/bulb/c. Confirmed, because it
       commits a frame to the card. */
    send(264, 'state:1;bulb:0;c:0;', true).then(function (r) {
      $('.label', btn).textContent = r && r.ok === false
        ? (r.error || 'shutter refused') : 'shot';
      setTimeout(function () {
        btn.classList.remove('fired');
        $('.label', btn).textContent = 'hold to shoot';
      }, 1200);
    });
  }

  btn.addEventListener('pointerdown', begin);
  ['pointerup', 'pointercancel', 'pointerleave'].forEach(function (ev) {
    btn.addEventListener(ev, cancel);
  });

  $('#rec').addEventListener('click', function () {
    S.recording = !S.recording;
    $('#rec').classList.toggle('on', S.recording);
    $('#rec').textContent = S.recording ? '■ stop' : '● record';
    send(263, 'state:' + (S.recording ? 1 : 0) + ';', true);
  });

  $('#previewtoggle').addEventListener('click', function () { setLive(!S.live); });
}

/* ─────────────────────────────── settings ──────────────────────────────── */

/* Head settings: camera-plate direction (546, a benign move) and the restricted-
   angle / travel-limit release (542, dangerous — attestation-gated). */
function headSettingsRefresh() {
  send(545, '', false);   /* prime the camera-plate direction read */
  setTimeout(function () {
    var d = S.ops['545']; if (d) $('#camdir').checked = kv(d.args).dir === '1';
  }, 600);
  var reflect = function (r) {
    if (r && typeof r.released === 'boolean') {
      $('#limits').checked = r.released;
      $('#limits-danger').hidden = !r.released;
      $('#limits-ok').checked = r.released;
    }
  };
  var getLimits = function () {
    return fetch('/api/astro/limits', { cache: 'no-store' }).then(function (r) { return r.json(); });
  };
  /* First read primes the head's 541; the reply lands a beat later, so read
     again to reflect the true state (a safety toggle mustn't show stale). */
  getLimits().then(reflect).catch(function () {});
  setTimeout(function () { getLimits().then(reflect).catch(function () {}); }, 700);
}

function wireHeadSettings() {
  $('#camdir').addEventListener('change', function () {
    send(546, 'dir:' + (this.checked ? 1 : 0) + ';', true);   /* MOVES -> confirm=1 */
    setHint('<b>Rotating the camera plate…</b>');
  });
  /* Restricted angle: toggling ON only REVEALS the attestation; the actual
     release fires when the clearance box is ticked. Toggling OFF enforces the
     limits immediately (always safe). */
  $('#limits').addEventListener('change', function () {
    if (this.checked) {
      $('#limits-danger').hidden = false;
    } else {
      $('#limits-danger').hidden = true; $('#limits-ok').checked = false;
      post('/api/astro/limits', { state: 0 }).then(function () {
        setHint('<b>Rotation limits enforced</b> — the safe default.');
      });
    }
  });
  $('#limits-ok').addEventListener('change', function () {
    if (!this.checked) { post('/api/astro/limits', { state: 0 }); return; }
    post('/api/astro/limits', { state: 1, attest: 1 }).then(function (r) {
      if (r && r.ok === false) {
        setHint('<b>Limits:</b> ' + (r.error || ''));
        $('#limits').checked = false; $('#limits-danger').hidden = true; $('#limits-ok').checked = false;
      } else {
        setHint('<b>Rotation limits RELEASED.</b> Watch the Astro Kit clearance in real time — the head can collide with it.');
      }
    });
  });
  $$('#tabs button').forEach(function (b) {
    if (b.dataset.tab === 'settings') b.addEventListener('click', headSettingsRefresh);
  });
  headSettingsRefresh();
}

function wireSettings() {
  $('#aboutstream').textContent = streamURL();

  var keep = $('#keepwifi');
  fetch('/api/keepwifi', { cache: 'no-store' })
    .then(function (r) { return r.json(); })
    .then(function (d) { keep.checked = !!(d.on || d.running); })
    .catch(function () {});
  keep.addEventListener('change', function () {
    post('/api/keepwifi', { on: keep.checked ? 1 : 0 });
  });

  var join = $('#autojoin');
  fetch('/api/wifi', { cache: 'no-store' })
    .then(function (r) { return r.json(); })
    .then(function (d) { join.checked = !!(d.autojoin || d.auto_join); })
    .catch(function () {});
  join.addEventListener('change', function () {
    post('/api/wifi', { autojoin: join.checked ? 1 : 0 });
  });
}

/* ─────────────────────────────── programs ──────────────────────────────── */

/* Timelapse and panorama are started here but RUN ON THE HEAD; the server owns
   the progress poll, so this page only reads /api/prog. It refreshes fast while
   a run is live and slowly otherwise, and it is safe to leave and come back to.

   Two guards from the protocol research shape the flow:
     * every start MOVES the mount and fires the shutter, so it is a two-tap
       arm-then-confirm, never a single tap that could fire on a brush;
     * the panorama start payload is INFERRED (the binding method would not
       decompile), so panorama previews the exact frame first and says so. */

var progTimer = 0;

function progGet() {
  return fetch('/api/prog', { cache: 'no-store' }).then(function (r) { return r.json(); });
}

function fmtDuration(sec) {
  sec = Math.round(sec);
  var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
  function pad(n) { return (n < 10 ? '0' : '') + n; }
  return (h ? h + ':' : '') + pad(m) + ':' + pad(s);
}

/* ---- timelapse panel ---- */

function lapseParams() {
  var shots = $('#tl-shots').value.trim();
  return {
    interval: parseFloat($('#tl-interval').value) || 0,
    shots: shots === '' ? -1 : parseInt(shots, 10),
    bulb: parseFloat($('#tl-bulb').value) || 0,
    fps: parseInt($('#tl-fps').value, 10) || 24
  };
}

function paintLapseDerived() {
  var p = lapseParams();
  var shoot = $('#tl-shoot'), video = $('#tl-video');
  if (p.shots === -1 || !(p.interval > 0)) {
    shoot.textContent = '∞'; video.textContent = '∞';
    return;
  }
  /* (shots-1)*interval — the gaps, not the frames; matches the head. */
  shoot.textContent = '≥ ' + fmtDuration((p.shots - 1) * p.interval);
  video.textContent = fmtDuration(Math.ceil(p.shots / p.fps));
}

/* ---- panorama panel ---- */

function panoParams() {
  return {
    cols: parseInt($('#pa-cols').value, 10) || 0,
    rows: parseInt($('#pa-rows').value, 10) || 0,
    hangle: parseFloat($('#pa-hangle').value) || 0,
    vangle: parseFloat($('#pa-vangle').value) || 0,
    startdir: parseInt($('#pa-startdir').value, 10),
    perspot: parseInt($('#pa-perspot').value, 10) || 1,
    interval: parseInt($('#pa-interval').value, 10) || 0,
    isp: $('#pa-isp').checked ? 1 : 0
  };
}

function paintPanoDerived() {
  var p = panoParams();
  $('#pa-coverage').textContent =
    (p.hangle * p.cols).toFixed(1) + '° × ' + (p.vangle * p.rows).toFixed(1) + '°';
  $('#pa-total').textContent = String(p.cols * p.rows * p.perspot);
  /* A parameter edit invalidates any preview shown for the old numbers. */
  var btn = $('#pa-start');
  if (btn.dataset.armed === '1') {
    btn.dataset.armed = '0';
    btn.classList.remove('armed');
    btn.textContent = 'Preview panorama';
    $('#pa-preview').hidden = true;
  }
}

/* ---- start flows ---- */

function armLapse() {
  var btn = $('#tl-start');
  if (btn.dataset.armed === '1') {
    /* second tap: commit */
    post('/api/prog/lapse', Object.assign(lapseParams(), { confirm: 1 })).then(function (r) {
      btn.dataset.armed = '0'; btn.classList.remove('armed');
      btn.textContent = 'Start timelapse';
      if (r && r.ok === false) setHint('<b>Timelapse refused.</b> ' + (r.error || ''));
      progRefresh();
    });
    return;
  }
  btn.dataset.armed = '1';
  btn.classList.add('armed');
  btn.textContent = 'Tap again to start — this fires the shutter';
  setTimeout(function () {
    if (btn.dataset.armed === '1') {
      btn.dataset.armed = '0'; btn.classList.remove('armed');
      btn.textContent = 'Start timelapse';
    }
  }, 4000);
}

function armPano() {
  var btn = $('#pa-start');
  if (btn.dataset.armed !== '1') {
    /* first tap: fetch and show the exact frame, then arm. */
    post('/api/prog/pano', panoParams()).then(function (r) {
      if (!r || r.valid === false) {
        $('#pa-preview').hidden = true;
        setHint('<b>Panorama parameters are out of range.</b> ' + ((r && r.error) || ''));
        return;
      }
      setHint('');
      $('#pa-frame').textContent = r.frame;
      $('#pa-preview').hidden = false;
      btn.dataset.armed = '1';
      btn.classList.add('armed');
      btn.textContent = 'Send this — ' + r.shots + ' frames, starts the sweep';
    });
    return;
  }
  /* second tap: commit */
  post('/api/prog/pano', Object.assign(panoParams(), { confirm: 1 })).then(function (r) {
    btn.dataset.armed = '0'; btn.classList.remove('armed');
    btn.textContent = 'Preview panorama';
    $('#pa-preview').hidden = true;
    if (r && r.ok === false) setHint('<b>Panorama refused.</b> ' + (r.error || ''));
    progRefresh();
  });
}

/* ---- the running view ---- */

function paintProg(d) {
  var run = $('#progrun'), pick = $('#progpick');
  var running = d && d.kind && d.kind !== 'none';
  run.hidden = !running;
  pick.hidden = !!running;
  if (!running) return;

  var titles = { panorama: 'Panorama running', timelapse: 'Timelapse running',
                 hdr: 'HDR bracket', focus: d.preview ? 'Focus preview' : 'Focus stack running',
                 program: 'Program running', pathlapse: 'Path-lapse running',
                 sun: 'Sun lapse running' };
  $('#progrun-title').textContent = titles[d.kind] || 'Running';

  var fill = $('#progfill');
  var known = !d.unlimited && typeof d.remaining === 'number' && d.remaining >= 0 && d.total > 0;
  fill.classList.toggle('indeterminate', !known);
  fill.classList.toggle('stale', !!d.stale);
  if (known) fill.style.width = (100 * (d.total - d.remaining) / d.total) + '%';

  var line;
  if (d.unlimited) {
    line = (typeof d.taken === 'number' ? d.taken : '—') + ' frames · ∞ · ' + fmtDuration(d.elapsed_s) + ' elapsed';
  } else if (known) {
    line = (d.total - d.remaining) + ' of ' + d.total + ' frames · ' + fmtDuration(d.elapsed_s) + ' elapsed';
  } else if (typeof d.remaining === 'number' && d.remaining >= 0) {
    /* Total unknown (the head owns the count, e.g. a Sun lapse), but it is
       reporting how many frames are left. */
    line = d.remaining + ' frames remaining · ' + fmtDuration(d.elapsed_s) + ' elapsed';
  } else {
    line = fmtDuration(d.elapsed_s) + ' elapsed · waiting on the head';
  }
  $('#progrun-line').textContent = line;

  var note = $('#progrun-note');
  if (d.stale) {
    note.hidden = false;
    note.innerHTML = 'The head has not reported progress recently. The run may have ' +
      'finished, or the camera may have stalled — this cannot tell which, so it will not guess.';
  } else note.hidden = true;

  /* Controls: cancel always; panorama also pause/resume. Rebuilt only when the
     set changes, so a poll does not blow away a button mid-press. */
  var want = d.kind === 'panorama'
    ? 'cancel,' + (d.paused ? 'resume' : 'pause')
    : 'cancel';
  var ctl = $('#progrun-controls');
  if (ctl.dataset.set !== want) {
    ctl.dataset.set = want;
    ctl.innerHTML = '';
    if (d.kind === 'panorama') {
      var pz = document.createElement('button');
      pz.className = 'ghost';
      pz.textContent = d.paused ? 'Resume' : 'Pause';
      pz.onclick = function () { post('/api/prog/pano/pause', { paused: d.paused ? 0 : 1 }).then(progRefresh); };
      ctl.appendChild(pz);
    }
    var cx = document.createElement('button');
    cx.className = 'ghost';
    cx.style.borderColor = 'var(--err)';
    cx.style.color = 'var(--err)';
    cx.textContent = 'Stop programme';
    cx.onclick = function () { post('/api/prog/cancel', {}).then(progRefresh); };
    ctl.appendChild(cx);
  }
}

function progRefresh() { return progGet().then(paintProg); }

/* Poll cadence follows what is happening: 1 s while a run is live so the count
   moves, 4 s otherwise so an idle Programs tab is quiet. Only runs while the
   tab is visible. */
function progPoll() {
  clearTimeout(progTimer);
  if ($('#tab-programs').hidden) return;
  progGet().then(function (d) {
    paintProg(d);
    /* Refresh the Holy Grail runtime brightness while its panel is the one
       showing (config, not a run — so it lives outside the run status). */
    var gp = $('.progpanel[data-prog="grail"]');
    if (gp && !gp.hidden && !$('#progpick').hidden) hgFetchStatus(false);
    var running = d && d.kind && d.kind !== 'none';
    progTimer = setTimeout(progPoll, running ? 1000 : 4000);
  }).catch(function () { progTimer = setTimeout(progPoll, 4000); });
}

/* ---- HDR panel ---- */

function armHdr() {
  var btn = $('#hdr-start');
  var params = { spread: parseInt($('#hdr-spread').value, 10) || 3, isp: $('#hdr-isp').checked ? 1 : 0 };
  if (btn.dataset.armed !== '1') {
    /* first tap: resolve the real three shutter speeds and show them */
    post('/api/prog/hdr', params).then(function (r) {
      if (!r || r.valid === false) {
        $('#hdr-preview').hidden = true;
        setHint('<b>HDR is not ready.</b> ' + ((r && r.error) || ''));
        return;
      }
      setHint('');
      /* the preview frame's trailing "shutters: a / b / c" line is the useful bit */
      var m = /shutters:\s*(.+)$/.exec(r.frame || '');
      $('#hdr-shutters').textContent = m ? m[1] : r.frame;
      $('#hdr-preview').hidden = false;
      btn.dataset.armed = '1'; btn.classList.add('armed');
      btn.textContent = 'Fire the three-frame bracket';
    });
    return;
  }
  post('/api/prog/hdr', Object.assign(params, { confirm: 1 })).then(function (r) {
    btn.dataset.armed = '0'; btn.classList.remove('armed');
    btn.textContent = 'Preview HDR';
    $('#hdr-preview').hidden = true;
    if (r && r.ok === false) setHint('<b>HDR refused.</b> ' + (r.error || ''));
    progRefresh();
  });
}

/* ---- focus-stack panel ---- */

var rackHold = 0;

function focusMark(which, btn) {
  post('/api/prog/focus/mark', { which: which }).then(function () {
    if (btn) { btn.classList.add('marked'); setTimeout(function () { btn.classList.remove('marked'); }, 1500); }
  });
}

function wireFocusRack() {
  /* Both the focus-stack rack (.rack) and the standalone manual-focus jog (.mf)
     on the Control tab drive the same 311 focus-adjust, so they share one wiring. */
  $$('.rack, .mf').forEach(function (btn) {
    var adj = btn.dataset.adj;
    /* Racking is a 311 focus-adjust, hold-repeats like the phone app (~300 ms).
       It goes through the ordinary allowlisted send, not a bespoke route. */
    function tick() { send(311, 'mode:1;adj:' + adj + ';'); }
    btn.addEventListener('pointerdown', function (e) {
      e.preventDefault();
      try { btn.setPointerCapture(e.pointerId); } catch (_) {}
      tick();
      if (navigator.vibrate) navigator.vibrate(6);
      rackHold = setInterval(tick, 300);
    });
    ['pointerup', 'pointercancel', 'pointerleave'].forEach(function (ev) {
      btn.addEventListener(ev, function () { clearInterval(rackHold); });
    });
  });
  $('#fs-mark-near').addEventListener('click', function () { focusMark(0, this); });
  $('#fs-mark-far').addEventListener('click', function () { focusMark(1, this); });

  $('#fs-preview').addEventListener('click', function () {
    post('/api/prog/focus', { shots: parseInt($('#fs-shots').value, 10) || 14,
                              isp: $('#fs-isp').checked ? 1 : 0, preview: 1 }).then(function (r) {
      if (r && r.ok === false) setHint('<b>Preview refused.</b> ' + (r.error || ''));
      progRefresh();
    });
  });

  var start = $('#fs-start');
  start.addEventListener('click', function () {
    var params = { shots: parseInt($('#fs-shots').value, 10) || 14, isp: $('#fs-isp').checked ? 1 : 0 };
    if (start.dataset.armed !== '1') {
      /* validate + arm */
      post('/api/prog/focus', params).then(function (r) {
        if (r && r.valid === false) { setHint('<b>Focus stack:</b> ' + (r.error || '')); return; }
        setHint('');
        start.dataset.armed = '1'; start.classList.add('armed');
        start.textContent = 'Tap again — racks focus and shoots';
        setTimeout(function () {
          if (start.dataset.armed === '1') {
            start.dataset.armed = '0'; start.classList.remove('armed');
            start.textContent = 'Start focus stack';
          }
        }, 4000);
      });
      return;
    }
    post('/api/prog/focus', Object.assign(params, { confirm: 1 })).then(function (r) {
      start.dataset.armed = '0'; start.classList.remove('armed');
      start.textContent = 'Start focus stack';
      if (r && r.ok === false) setHint('<b>Focus stack refused.</b> ' + (r.error || ''));
      progRefresh();
    });
  });
}

function wirePrograms() {
  $$('.seg').forEach(function (b) {
    b.addEventListener('click', function () {
      $$('.seg').forEach(function (x) { x.classList.toggle('on', x === b); });
      $$('.progpanel').forEach(function (pnl) { pnl.hidden = pnl.dataset.prog !== b.dataset.prog; });
      if (b.dataset.prog === 'grail') hgOnShow();   /* load lists + brightness */
    });
  });

  ['#tl-interval', '#tl-shots', '#tl-bulb', '#tl-fps'].forEach(function (sel) {
    $(sel).addEventListener('input', paintLapseDerived);
  });
  $('#tl-inf').addEventListener('click', function () { $('#tl-shots').value = ''; paintLapseDerived(); });
  $('#tl-start').addEventListener('click', armLapse);
  paintLapseDerived();

  ['#pa-cols', '#pa-rows', '#pa-hangle', '#pa-vangle', '#pa-startdir', '#pa-perspot', '#pa-interval']
    .forEach(function (sel) { $(sel).addEventListener('input', paintPanoDerived); });
  $('#pa-start').addEventListener('click', armPano);
  paintPanoDerived();

  /* Re-preview HDR / re-arm focus when their inputs change. */
  ['#hdr-spread', '#hdr-isp'].forEach(function (sel) {
    $(sel).addEventListener('input', function () {
      var b = $('#hdr-start');
      if (b.dataset.armed === '1') { b.dataset.armed = '0'; b.classList.remove('armed'); b.textContent = 'Preview HDR'; $('#hdr-preview').hidden = true; }
    });
  });
  $('#hdr-start').addEventListener('click', armHdr);
  wireFocusRack();
}

/* ─────────────────────────────── astro ─────────────────────────────────── */

/* Alignment, the browser way. Three methods, all ending in the same 527
   SP_SET_YAW the phone app sends -- the plate solve writes it from an actual
   solve (best), the phone compass reads the handset magnetometer if there is
   one, and manual entry is the last resort. */

var astroPoll = 0;

function astroGet() { return fetch('/api/astro', { cache: 'no-store' }).then(function (r) { return r.json(); }); }

function paintAstro(a) {
  var on = !!(a && a.active);
  $('#astro-enter').checked = on;
  $('#astro-controls').hidden = !on;
  /* The tracking button reflects what the mount actually reports (284), not
     just what we asked for -- read from the shared telemetry. */
  var mode = S.ops['284'] ? kv(S.ops['284'].args) : {};
  var tracking = mode.track === '1';
  var tb = $('#astro-track');
  tb.classList.toggle('armed', tracking);
  tb.textContent = tracking ? 'Stop tracking' : 'Start tracking';
}

function astroRefresh() { return astroGet().then(paintAstro); }

/* iOS 13+ gates DeviceOrientation behind a permission call that must come from a
   user gesture; Android exposes it without one. This normalises both, and the
   heading maths follows the snippet the operator pasted. */
function readPhoneCompass() {
  var out = $('#astro-compass-val'), btn = $('#astro-compass-align');
  function start() {
    if (!window.DeviceOrientationEvent) { out.textContent = 'no sensor on this device'; return; }
    var handler = function (e) {
      var h;
      if (typeof e.webkitCompassHeading === 'number') {
        h = e.webkitCompassHeading;                 /* iOS: already true-ish heading */
      } else if (e.alpha != null) {
        h = e.alpha;
        if (!window.chrome) h = h - 270;            /* Android stock, per the app note */
        h = (360 - h) % 360;                        /* alpha is counter-clockwise from east */
      } else { return; }
      h = ((h % 360) + 360) % 360;
      out.textContent = h.toFixed(1) + '°';
      out.classList.remove('dim');
      btn.disabled = false;
      btn.dataset.heading = h.toFixed(2);
    };
    window.addEventListener('deviceorientation', handler, true);
    out.textContent = 'reading…';
  }
  if (typeof DeviceOrientationEvent !== 'undefined' &&
      typeof DeviceOrientationEvent.requestPermission === 'function') {
    DeviceOrientationEvent.requestPermission().then(function (state) {
      if (state === 'granted') start();
      else out.textContent = 'permission denied';
    }).catch(function () { out.textContent = 'permission error'; });
  } else start();
}

function wireAstro() {
  $('#astro-enter').addEventListener('change', function () {
    var entering = this.checked;
    post(entering ? '/api/astro/enter' : '/api/astro/leave', {}).then(function (r) {
      if (r && r.ok === false) setHint('<b>Astro:</b> ' + (r.error || ''));
      if (entering) {
        /* Ask the head for its current tilt-compensation + dithering flags so the
           toggles reflect reality rather than defaulting to off. */
        send(537, '', false);
        send(539, '', false);
        setTimeout(function () {
          var t = S.ops['537']; if (t) $('#astro-tilt').checked = kv(t.args).state === '1';
          var d = S.ops['539']; if (d) $('#dither').checked = kv(d.args).state === '1';
        }, 600);
      }
      astroRefresh();
    });
  });

  /* Tilt compensation (538): the setting that lets deep-sky tracking work on an
     unlevel tripod. Persistent, no motion — so no confirm. */
  $('#astro-tilt').addEventListener('change', function () {
    send(538, 'state:' + (this.checked ? 1 : 0) + ';', false);
    setHint(this.checked
      ? '<b>Tilt compensation on</b> — the head will correct for an unlevel base.'
      : '<b>Tilt compensation off</b> — level the tripod by hand for accurate tracking.');
  });

  /* Auto-level (549) physically drives the head to level. It MOVES, so it is
     confirmed, and it must not run while tracking. */
  $('#astro-level').addEventListener('click', function () {
    send(549, 'state:1;', true);
    setHint('<b>Auto-levelling…</b> let the head settle before you align, and never level while tracking.');
  });

  /* Dithering (540): a stacking aid, persistent, no immediate motion. */
  $('#dither').addEventListener('change', function () {
    send(540, 'state:' + (this.checked ? 1 : 0) + ';', false);
  });

  /* Plate solve — the compass-free path. Kicks the solver with apply=1 and
     watches /api/state, which the legacy dashboard already drives. */
  $('#astro-solve').addEventListener('click', function () {
    var msg = $('#astro-solve-msg');
    msg.hidden = false; msg.textContent = 'solving the sky…';
    post('/api/solve?mode=0&apply=1', {}).then(function () {
      var tries = 0;
      var iv = setInterval(function () {
        fetch('/api/state', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (st) {
          if (st.status === 'done') {
            clearInterval(iv);
            msg.textContent = st.result && st.result.solved
              ? 'solved and aligned — the head now knows where it is pointing'
              : 'solve finished; alignment written';
          } else if (st.status === 'failed') {
            clearInterval(iv); msg.textContent = 'solve failed — check framing and focal length on the solver dashboard';
          } else if (++tries > 60) { clearInterval(iv); msg.textContent = 'still solving — watch the solver dashboard'; }
          else msg.textContent = 'solving… (' + (st.status || 'working') + ')';
        }).catch(function () { clearInterval(iv); msg.textContent = 'lost contact with the solver'; });
      }, 1000);
    });
  });

  $('#astro-compass').addEventListener('click', readPhoneCompass);
  $('#astro-compass-align').addEventListener('click', function () {
    post('/api/astro/align', { compass: this.dataset.heading }).then(function (r) {
      setHint(r && r.ok ? '' : '<b>Align:</b> ' + ((r && r.error) || ''));
    });
  });
  $('#astro-manual-align').addEventListener('click', function () {
    var v = $('#astro-manual').value;
    if (v === '') return;
    post('/api/astro/align', { compass: v }).then(function (r) {
      setHint(r && r.ok ? '' : '<b>Align:</b> ' + ((r && r.error) || ''));
    });
  });

  $('#astro-track').addEventListener('click', function () {
    var mode = S.ops['284'] ? kv(S.ops['284'].args) : {};
    var tracking = mode.track === '1';
    post('/api/astro/track', {
      on: tracking ? 0 : 1,
      rate: $('#astro-rate').value,
      half: $('#astro-half').checked ? 1 : 0
    }).then(function (r) {
      if (r && r.ok === false) setHint('<b>Tracking:</b> ' + (r.error || ''));
      setTimeout(astroRefresh, 300);
    });
  });

  wireObservatoryTools();
  wireTargets();
  wireAstroCapture();
  wireAstroAutofocus();
}

/* A compact deep-sky + bright-star catalogue (J2000, degrees). Fixed coordinates
   only — planets and the Moon need an ephemeris, so they stay in a planetarium
   app for now; these are the objects you actually image. */
var ASTRO_TARGETS = [
  { n: 'Moon', t: 'moon', dyn: 'moon', a: 'luna' },
  { n: 'Sun', t: 'sun', dyn: 'sun', a: 'solar', danger: 'solar' },
  { n: 'Mars', t: 'planet', dyn: 'mars', a: '' },
  { n: 'Jupiter', t: 'planet', dyn: 'jupiter', a: '' },
  { n: 'Saturn', t: 'planet', dyn: 'saturn', a: '' },
  { n: 'Venus', t: 'planet', dyn: 'venus', a: '' },
  { n: 'Mercury', t: 'planet', dyn: 'mercury', a: '' },
  { n: 'Uranus', t: 'planet', dyn: 'uranus', a: '' },
  { n: 'Neptune', t: 'planet', dyn: 'neptune', a: '' },
  { n: 'M42 Orion Nebula', t: 'nebula', ra: 83.82, dec: -5.39, a: 'orion' },
  { n: 'M31 Andromeda Galaxy', t: 'galaxy', ra: 10.68, dec: 41.27, a: 'andromeda' },
  { n: 'M45 Pleiades', t: 'cluster', ra: 56.75, dec: 24.12, a: 'seven sisters subaru' },
  { n: 'M1 Crab Nebula', t: 'nebula', ra: 83.63, dec: 22.01, a: '' },
  { n: 'M8 Lagoon Nebula', t: 'nebula', ra: 270.92, dec: -24.38, a: '' },
  { n: 'M16 Eagle Nebula', t: 'nebula', ra: 274.70, dec: -13.81, a: 'pillars' },
  { n: 'M17 Omega / Swan Nebula', t: 'nebula', ra: 275.20, dec: -16.17, a: 'swan' },
  { n: 'M20 Trifid Nebula', t: 'nebula', ra: 270.60, dec: -23.03, a: '' },
  { n: 'M27 Dumbbell Nebula', t: 'nebula', ra: 299.90, dec: 22.72, a: '' },
  { n: 'M57 Ring Nebula', t: 'nebula', ra: 283.40, dec: 33.03, a: 'ring' },
  { n: 'M97 Owl Nebula', t: 'nebula', ra: 168.70, dec: 55.02, a: 'owl' },
  { n: 'M33 Triangulum Galaxy', t: 'galaxy', ra: 23.46, dec: 30.66, a: 'pinwheel' },
  { n: 'M51 Whirlpool Galaxy', t: 'galaxy', ra: 202.47, dec: 47.19, a: 'whirlpool' },
  { n: 'M63 Sunflower Galaxy', t: 'galaxy', ra: 198.96, dec: 42.03, a: 'sunflower' },
  { n: 'M64 Black Eye Galaxy', t: 'galaxy', ra: 194.18, dec: 21.68, a: 'black eye' },
  { n: 'M81 Bode’s Galaxy', t: 'galaxy', ra: 148.89, dec: 69.07, a: 'bode' },
  { n: 'M82 Cigar Galaxy', t: 'galaxy', ra: 148.97, dec: 69.68, a: 'cigar' },
  { n: 'M101 Pinwheel Galaxy', t: 'galaxy', ra: 210.80, dec: 54.35, a: 'pinwheel' },
  { n: 'M104 Sombrero Galaxy', t: 'galaxy', ra: 189.998, dec: -11.62, a: 'sombrero' },
  { n: 'M13 Hercules Cluster', t: 'cluster', ra: 250.42, dec: 36.46, a: 'hercules globular' },
  { n: 'M22 Sagittarius Cluster', t: 'cluster', ra: 279.10, dec: -23.90, a: '' },
  { n: 'M3 Globular Cluster', t: 'cluster', ra: 205.55, dec: 28.38, a: '' },
  { n: 'M44 Beehive Cluster', t: 'cluster', ra: 130.10, dec: 19.67, a: 'beehive praesepe' },
  { n: 'NGC 7000 North America Nebula', t: 'nebula', ra: 314.75, dec: 44.31, a: 'north america' },
  { n: 'NGC 6960 Veil Nebula', t: 'nebula', ra: 311.60, dec: 30.72, a: 'veil' },
  { n: 'NGC 7293 Helix Nebula', t: 'nebula', ra: 337.41, dec: -20.84, a: 'helix' },
  { n: 'NGC 2237 Rosette Nebula', t: 'nebula', ra: 97.90, dec: 4.95, a: 'rosette' },
  { n: 'NGC 869/884 Double Cluster', t: 'cluster', ra: 34.70, dec: 57.13, a: 'double cluster' },
  { n: 'IC 1396 Elephant’s Trunk', t: 'nebula', ra: 324.74, dec: 57.50, a: 'elephant trunk' },
  { n: 'IC 434 Horsehead Nebula', t: 'nebula', ra: 85.24, dec: -2.46, a: 'horsehead' },
  { n: 'Polaris (align/home)', t: 'star', ra: 37.95, dec: 89.26, a: 'north star' },
  { n: 'Vega', t: 'star', ra: 279.23, dec: 38.78, a: '' },
  { n: 'Deneb', t: 'star', ra: 310.36, dec: 45.28, a: '' },
  { n: 'Altair', t: 'star', ra: 297.70, dec: 8.87, a: '' },
  { n: 'Arcturus', t: 'star', ra: 213.92, dec: 19.18, a: '' },
  { n: 'Capella', t: 'star', ra: 79.17, dec: 45.998, a: '' },
  { n: 'Betelgeuse', t: 'star', ra: 88.79, dec: 7.41, a: '' },
  { n: 'Sirius', t: 'star', ra: 101.29, dec: -16.72, a: '' }
];

/* Low-precision ephemeris for the Moon and planets (Paul Schlyter's method),
   good to ~1–2′ — well inside a GOTO + plate-solve refine. Returns geocentric
   RA/Dec of date in degrees. The Sun is deliberately absent from the catalogue
   (pointing a camera at it without a filter destroys the sensor). */
function ephem(body, date) {
  var RAD = Math.PI / 180;
  var Y = date.getUTCFullYear(), Mo = date.getUTCMonth() + 1, D = date.getUTCDate();
  var ut = date.getUTCHours() + date.getUTCMinutes() / 60 + date.getUTCSeconds() / 3600;
  var d = 367 * Y - Math.floor(7 * (Y + Math.floor((Mo + 9) / 12)) / 4) + Math.floor(275 * Mo / 9) + D - 730530 + ut / 24;
  var ecl = 23.4393 - 3.563e-7 * d;
  function rev(x) { return x - Math.floor(x / 360) * 360; }
  function sind(x) { return Math.sin(x * RAD); }
  function cosd(x) { return Math.cos(x * RAD); }
  function radec(xg, yg, zg) {
    var xe = xg, ye = yg * cosd(ecl) - zg * sind(ecl), ze = yg * sind(ecl) + zg * cosd(ecl);
    return { ra: rev(Math.atan2(ye, xe) / RAD), dec: Math.atan2(ze, Math.sqrt(xe * xe + ye * ye)) / RAD };
  }
  /* the Sun's geocentric rectangular position — needed to make planets geocentric */
  var ws = 282.9404 + 4.70935e-5 * d, es = 0.016709 - 1.151e-9 * d, Ms = rev(356.0470 + 0.9856002585 * d);
  var Esun = Ms + (1 / RAD) * es * sind(Ms) * (1 + es * cosd(Ms));
  var xvs = cosd(Esun) - es, yvs = Math.sqrt(1 - es * es) * sind(Esun);
  var lonsun = rev(Math.atan2(yvs, xvs) / RAD + ws), rsun = Math.sqrt(xvs * xvs + yvs * yvs);
  var xs = rsun * cosd(lonsun), ys = rsun * sind(lonsun);

  if (body === 'sun') return radec(xs, ys, 0);
  if (body === 'moon') {
    var N = rev(125.1228 - 0.0529538083 * d), i = 5.1454, w = rev(318.0634 + 0.1643573223 * d);
    var a = 60.2666, e = 0.054900, M = rev(115.3654 + 13.0649929509 * d);
    var E = M + (1 / RAD) * e * sind(M) * (1 + e * cosd(M));
    E = E - (E - (1 / RAD) * e * sind(E) - M) / (1 - e * cosd(E));
    var xv = a * (cosd(E) - e), yv = a * Math.sqrt(1 - e * e) * sind(E);
    var v = rev(Math.atan2(yv, xv) / RAD), r = Math.sqrt(xv * xv + yv * yv);
    var xh = r * (cosd(N) * cosd(v + w) - sind(N) * sind(v + w) * cosd(i));
    var yh = r * (sind(N) * cosd(v + w) + cosd(N) * sind(v + w) * cosd(i));
    var zh = r * (sind(v + w) * sind(i));
    var lon = rev(Math.atan2(yh, xh) / RAD), lat = Math.atan2(zh, Math.sqrt(xh * xh + yh * yh)) / RAD;
    var Lm = rev(N + w + M), Ls = rev(Ms + ws), Dm = rev(Lm - Ls), F = rev(Lm - N);
    lon += -1.274 * sind(M - 2 * Dm) + 0.658 * sind(2 * Dm) - 0.186 * sind(Ms) - 0.059 * sind(2 * M - 2 * Dm)
         - 0.057 * sind(M - 2 * Dm + Ms) + 0.053 * sind(M + 2 * Dm) + 0.046 * sind(2 * Dm - Ms)
         + 0.041 * sind(M - Ms) - 0.035 * sind(Dm) - 0.031 * sind(M + Ms) - 0.015 * sind(2 * F - 2 * Dm)
         + 0.011 * sind(M - 4 * Dm);
    lat += -0.173 * sind(F - 2 * Dm) - 0.055 * sind(M - F - 2 * Dm) - 0.046 * sind(M + F - 2 * Dm)
         + 0.033 * sind(F + 2 * Dm) + 0.017 * sind(2 * M + F);
    r += -0.58 * cosd(M - 2 * Dm) - 0.46 * cosd(2 * Dm);
    return radec(r * cosd(lon) * cosd(lat), r * sind(lon) * cosd(lat), r * sind(lat));
  }
  var P = {
    mercury: [48.3313, 4.5236e-5, 7.0047, 5.00e-8, 29.1241, 1.01444e-5, 0.387098, 0, 0.205635, 5.59e-10, 168.6562, 4.0923344368],
    venus:   [76.6799, 2.4659e-5, 3.3946, 2.75e-8, 54.8910, 1.38374e-5, 0.723330, 0, 0.006773, -1.302e-9, 48.0052, 1.6021302244],
    mars:    [49.5574, 2.11081e-5, 1.8497, -1.78e-8, 286.5016, 2.92961e-5, 1.523688, 0, 0.093405, 2.516e-9, 18.6021, 0.5240207766],
    jupiter: [100.4542, 2.76854e-5, 1.3030, -1.557e-7, 273.8777, 1.64505e-5, 5.20256, 0, 0.048498, 4.469e-9, 19.8950, 0.0830853001],
    saturn:  [113.6634, 2.38980e-5, 2.4886, -1.081e-7, 339.3939, 2.97661e-5, 9.55475, 0, 0.055546, -9.499e-9, 316.9670, 0.0334442282],
    uranus:  [74.0005, 1.3978e-5, 0.7733, 1.9e-8, 96.6612, 3.0565e-5, 19.18171, -1.55e-8, 0.047318, 7.45e-9, 142.5905, 0.011725806],
    neptune: [131.7806, 3.0173e-5, 1.7700, -2.55e-7, 272.8461, -6.027e-6, 30.05826, 3.313e-8, 0.008606, 2.15e-9, 260.2471, 0.005995147]
  };
  var el = P[body]; if (!el) return { ra: 0, dec: 0 };
  var N2 = rev(el[0] + el[1] * d), ip = el[2] + el[3] * d, w2 = rev(el[4] + el[5] * d);
  var a2 = el[6] + el[7] * d, e2 = el[8] + el[9] * d, M2 = rev(el[10] + el[11] * d), E2 = M2, k;
  for (k = 0; k < 5; k++) E2 = E2 - (E2 - (1 / RAD) * e2 * sind(E2) - M2) / (1 - e2 * cosd(E2));
  var xv2 = a2 * (cosd(E2) - e2), yv2 = a2 * Math.sqrt(1 - e2 * e2) * sind(E2);
  var v2 = rev(Math.atan2(yv2, xv2) / RAD), r2 = Math.sqrt(xv2 * xv2 + yv2 * yv2);
  var xh2 = r2 * (cosd(N2) * cosd(v2 + w2) - sind(N2) * sind(v2 + w2) * cosd(ip));
  var yh2 = r2 * (sind(N2) * cosd(v2 + w2) + cosd(N2) * sind(v2 + w2) * cosd(ip));
  var zh2 = r2 * (sind(v2 + w2) * sind(ip));
  /* the giant planets carry perturbations big enough to matter for pointing */
  if (body === 'jupiter' || body === 'saturn') {
    var lon2 = rev(Math.atan2(yh2, xh2) / RAD), lat2 = Math.atan2(zh2, Math.sqrt(xh2 * xh2 + yh2 * yh2)) / RAD;
    var Mj = rev(19.8950 + 0.0830853001 * d), Msat = rev(316.9670 + 0.0334442282 * d);
    if (body === 'jupiter') {
      lon2 += -0.332 * sind(2 * Mj - 5 * Msat - 67.6) - 0.056 * sind(2 * Mj - 2 * Msat + 21)
            + 0.042 * sind(3 * Mj - 5 * Msat + 21) - 0.036 * sind(Mj - 2 * Msat)
            + 0.022 * cosd(Mj - Msat) + 0.023 * sind(2 * Mj - 3 * Msat + 52) - 0.016 * sind(Mj - 5 * Msat - 69);
    } else {
      lon2 += 0.812 * sind(2 * Mj - 5 * Msat - 67.6) - 0.229 * cosd(2 * Mj - 4 * Msat - 2)
            + 0.119 * sind(Mj - 2 * Msat - 3) + 0.046 * sind(2 * Mj - 6 * Msat - 69) + 0.014 * sind(Mj - 3 * Msat + 32);
      lat2 += -0.020 * cosd(2 * Mj - 4 * Msat - 2) + 0.018 * sind(2 * Mj - 6 * Msat - 49);
    }
    xh2 = r2 * cosd(lon2) * cosd(lat2); yh2 = r2 * sind(lon2) * cosd(lat2); zh2 = r2 * sind(lat2);
  }
  return radec(xh2 + xs, yh2 + ys, zh2);
}

/* A target's RA/Dec now — computed for the Moon/planets, fixed for deep sky. */
function resolveTarget(o) {
  if (o.dyn) { var c = ephem(o.dyn, new Date()); return { ra: c.ra, dec: c.dec, dyn: true }; }
  return { ra: o.ra, dec: o.dec, dyn: false };
}

function wireTargets() {
  var sel = $('#tgt-select'), search = $('#tgt-search'), info = $('#tgt-info'), go = $('#tgt-goto');
  function render(filter) {
    var q = (filter || '').toLowerCase().trim();
    sel.innerHTML = ASTRO_TARGETS.map(function (o, i) {
      if (q && o.n.toLowerCase().indexOf(q) < 0 && (o.a || '').indexOf(q) < 0 && o.t.indexOf(q) < 0) return '';
      return '<option value="' + i + '">' + o.n + '  ·  ' + o.t + '</option>';
    }).join('');
  }
  render('');
  search.addEventListener('input', function () { render(this.value); });
  function refreshGoState() {
    var o = ASTRO_TARGETS[parseInt(sel.value, 10)]; if (!o) return;
    var danger = o.danger === 'solar';
    $('#tgt-danger').hidden = !danger;
    go.disabled = danger && !$('#tgt-solar-ok').checked;
    go.dataset.armed = '0'; go.classList.remove('armed');
    go.textContent = 'Go to ' + o.n.split(' ')[0];
  }
  sel.addEventListener('change', function () {
    var o = ASTRO_TARGETS[parseInt(this.value, 10)]; if (!o) return;
    var c = resolveTarget(o);
    info.textContent = o.n + '  ·  RA ' + (c.ra / 15).toFixed(2) + 'h  Dec ' + c.dec.toFixed(1) + '°' +
                       (c.dyn ? '  · now' : '');
    if (o.danger !== 'solar') $('#tgt-solar-ok').checked = false;
    refreshGoState();
  });
  $('#tgt-solar-ok').addEventListener('change', refreshGoState);
  go.addEventListener('click', function () {
    var o = ASTRO_TARGETS[parseInt(sel.value, 10)]; if (!o) return;
    var msg = $('#tgt-msg'), c = resolveTarget(o);   /* fresh coords each tap for the Moon/planets */
    if (o.danger === 'solar' && !$('#tgt-solar-ok').checked) {
      setHint('<b>Sun:</b> confirm a solar filter is fitted first.'); return;
    }
    if (go.dataset.armed !== '1') {
      post('/api/astro/goto', { ra: c.ra, dec: c.dec, name: o.n }).then(function (r) {
        if (r && r.valid === false) { setHint('<b>Go to:</b> ' + (r.error || '')); return; }
        go.dataset.armed = '1'; go.classList.add('armed');
        go.textContent = 'Confirm — slew to ' + o.n.split(' ')[0];
      });
      return;
    }
    post('/api/astro/goto', { ra: c.ra, dec: c.dec, name: o.n, confirm: 1 }).then(function (r) {
      go.dataset.armed = '0'; go.classList.remove('armed'); go.textContent = 'Go to ' + o.n.split(' ')[0];
      msg.hidden = false;
      msg.innerHTML = (r && r.ok === false)
        ? '<b>Refused.</b> ' + (r.error || '')
        : 'Slewing to ' + o.n + ' — it will track once it arrives.' +
          (o.dyn === 'moon' ? ' The Moon drifts ~0.5°/hr — switch Track to lunar rate.' : '');
    });
  });
}

function wireAstroCapture() {
  var b = $('#cap-start');
  $('#cap-inf').addEventListener('click', function () { $('#cap-shots').value = ''; });
  b.addEventListener('click', function () {
    var params = {
      interval: parseInt($('#cap-interval').value, 10) || 8,
      shots: $('#cap-shots').value.trim() === '' ? '' : parseInt($('#cap-shots').value, 10)
    };
    if (b.dataset.armed !== '1') {
      b.dataset.armed = '1'; b.classList.add('armed'); b.textContent = 'Confirm — start capturing';
      return;
    }
    post('/api/astro/capture', Object.assign(params, { confirm: 1 })).then(function (r) {
      b.dataset.armed = '0'; b.classList.remove('armed'); b.textContent = 'Start astro capture';
      if (r && r.ok === false) { setHint('<b>Astro capture:</b> ' + (r.error || '')); return; }
      setHint(''); $('#cap-run').hidden = false;
    });
  });
}

/* Reflect a running astro capture (a 272 programme) in the capture card. */
function paintAstroCapture(d) {
  var run = $('#cap-run'); if (!run) return;
  var on = d && d.kind && d.kind !== 'none';
  run.hidden = !on;
  if (!on) return;
  var line = d.unlimited ? ((typeof d.taken === 'number' ? d.taken : '—') + ' frames · ∞')
    : (typeof d.remaining === 'number' && d.remaining >= 0 && d.total > 0)
      ? ((d.total - d.remaining) + ' of ' + d.total + ' frames')
      : (typeof d.remaining === 'number' && d.remaining >= 0) ? (d.remaining + ' frames left')
      : 'running';
  $('#cap-status').textContent = line + ' · ' + fmtDuration(d.elapsed_s) + ' elapsed';
}

/* ── astro autofocus: start a sweep, poll the V-curve, settle on best ──
   The server runs the sweep (focuser + HFR scoring); the page just starts it and
   polls /api/astro/autofocus, which returns the sweep's own progress JSON. */
var afPoll = 0;

function afDrawCurve(prog) {
  var box = $('#af-curvebox'), svg = $('#af-curve'), best = $('#af-best');
  var s = (prog && prog.samples) || [], i, sc, max = 0, min = Infinity;
  if (!s.length) { box.hidden = true; return; }
  box.hidden = false;
  for (i = 0; i < s.length; i++) { sc = +s[i].score || 0; if (sc > max) max = sc; if (sc < min) min = sc; }
  var W = 300, H = 90, pad = 5, span = (max - min) || 1, n = s.length;
  var X = function (i) { return pad + (n <= 1 ? 0 : i * (W - 2 * pad) / (n - 1)); };
  var Y = function (sc) { return H - pad - ((sc - min) / span) * (H - 2 * pad); };
  var pts = s.map(function (p, i) { return X(i).toFixed(1) + ',' + Y(+p.score || 0).toFixed(1); }).join(' ');
  var bstep = (prog.best && typeof prog.best.step === 'number') ? prog.best.step : -1;
  var svgtext = '<polyline class="afline" points="' + pts + '"/>';
  if (bstep >= 0) { var bx = X(bstep).toFixed(1); svgtext += '<line class="afbest" x1="' + bx + '" y1="0" x2="' + bx + '" y2="' + H + '"/>'; }
  svgtext += s.map(function (p, i) { return '<circle class="afdot" cx="' + X(i).toFixed(1) + '" cy="' + Y(+p.score || 0).toFixed(1) + '" r="2.2"/>'; }).join('');
  svg.innerHTML = svgtext;
  if (prog.best) {
    var h = prog.best.hfr;
    best.textContent = 'best: step ' + prog.best.step +
      (h != null && +h > 0 ? ' · HFR ' + (+h).toFixed(2) + ' px' : '') +
      (prog.step && prog.steps ? '  ·  swept ' + prog.step + '/' + prog.steps : '');
  }
}

function afRender(d) {
  var run = $('#af-run'), stop = $('#af-stop'), msg = $('#af-msg');
  var prog = (d && d.progress) || {};
  var st = (d && d.status) || prog.state || 'idle';
  afDrawCurve(prog);
  if (st === 'running') {
    run.hidden = true; stop.hidden = false;
    msg.hidden = false; msg.innerHTML = '<b>Focusing…</b> step ' + (prog.step || 0) + ' of ' + (prog.steps || '—');
  } else {
    run.hidden = false; stop.hidden = true;
    if (st === 'done') {
      msg.hidden = false;
      msg.innerHTML = '<b>Focused.</b> Settled on the sharpest step' +
        (prog.best && +prog.best.hfr > 0 ? ' — HFR ' + (+prog.best.hfr).toFixed(2) + ' px.' : '.');
    } else if (st === 'failed') {
      msg.hidden = false; msg.innerHTML = '<b>Auto-focus:</b> ' + (prog.error || 'the sweep could not complete.');
    } else { msg.hidden = true; }
    if (afPoll) { clearInterval(afPoll); afPoll = 0; }
  }
}

function afTick() {
  fetch('/api/astro/autofocus', { cache: 'no-store' })
    .then(function (r) { return r.json(); }).then(afRender).catch(function () {});
}

function wireAstroAutofocus() {
  var run = $('#af-run'), stop = $('#af-stop');
  run.addEventListener('click', function () {
    var bl = parseInt($('#af-backlash').value, 10);
    var params = {
      confirm: 1,
      steps: parseInt($('#af-steps').value, 10) || 12,
      adj: parseInt($('#af-adj').value, 10) || 1,
      settle: parseFloat($('#af-settle').value),
      backlash: isNaN(bl) ? 3 : bl
    };
    if (isNaN(params.settle)) params.settle = 0.8;
    $('#af-msg').hidden = false; $('#af-msg').innerHTML = 'Starting…';
    post('/api/astro/autofocus', params).then(function (r) {
      if (r && r.ok === false) { $('#af-msg').innerHTML = '<b>Auto-focus:</b> ' + (r.error || 'could not start.'); return; }
      run.hidden = true; stop.hidden = false;
      afTick();
      if (!afPoll) afPoll = setInterval(afTick, 1200);
    });
  });
  stop.addEventListener('click', function () {
    post('/api/astro/autofocus', { stop: 1 }).then(function () { afTick(); });
  });
  afTick();   /* pick up a sweep already running (e.g. after a reload) */
}

/* ── plate solving, guiding, status — the /legacy dashboard, folded in native ──
   All read-only polls hit files on the server (no mount connection), so they are
   safe to run on a timer; only the explicit "Read position" opens a link, and
   only when aligned. */

function fmtRaDec(sol) {
  if (!sol || sol.solved !== true) return '—';
  var ra = sol.ra_deg, dec = sol.dec_deg;
  if (typeof ra !== 'number' || typeof dec !== 'number') return 'solved';
  var h = ra / 15, hh = Math.floor(h), mm = Math.floor((h - hh) * 60), ss = Math.round((((h - hh) * 60) - mm) * 60);
  var sign = dec < 0 ? '-' : '+', da = Math.abs(dec), dd = Math.floor(da), dm = Math.round((da - dd) * 60);
  return 'RA ' + hh + 'h' + mm + 'm' + ss + 's · Dec ' + sign + dd + '°' + dm + "'";
}

function paintSolveState(st) {
  if (!st) return;
  $('#solve-status').textContent = st.status +
    (st.status === 'running' && st.elapsed_sec ? ' (' + st.elapsed_sec + 's)' : '');
  $('#solve-cancel').hidden = st.status !== 'running';
  var sol = (st.solution && st.solution !== null && typeof st.solution === 'object') ? st.solution : null;
  $('#solve-solution').textContent = sol ? fmtRaDec(sol) : (st.solution ? 'solved' : '—');
  if (st.log != null) { var lp = $('#solve-log'); lp.textContent = st.log || '—'; lp.scrollTop = lp.scrollHeight; }
  /* Status card rides the same reply. */
  var m = S.ops['284'] ? kv(S.ops['284'].args) : {};
  $('#st-mode').textContent = m.mode || '—';
  $('#st-aligned').textContent = st.aligned ? 'yes' : (st.mount_blocked ? 'unknown (unaligned)' : 'no');
  $('#st-tracking').textContent = st.tracking ? 'yes' : 'no';
  if (st.mount_read) $('#st-altaz').textContent = st.alt.toFixed(3) + '° / ' + st.az.toFixed(3) + '°';
  var p = S.ops['517'] ? kv(S.ops['517'].args) : {};
  if (p.pitch !== undefined)
    $('#st-att').textContent = 'pitch ' + (parseFloat(p.pitch) * 180 / Math.PI).toFixed(1) +
                               '° · roll ' + (parseFloat(p.roll || 0) * 180 / Math.PI).toFixed(1) + '°';
}

function solveGet(mount) {
  return fetch('/api/state' + (mount ? '?mount=1' : ''), { cache: 'no-store' })
    .then(function (r) { return r.json(); }).then(paintSolveState).catch(function () {});
}

function drawSpark(pts) {
  var c = $('#guide-spark'); if (!c || !c.getContext) return;
  c.hidden = pts.length < 2;
  if (pts.length < 2) return;
  var ctx = c.getContext('2d'), W = c.width, H = c.height, max = 1;
  pts.forEach(function (p) { max = Math.max(max, Math.abs(p.d)); });
  ctx.clearRect(0, 0, W, H);
  ctx.strokeStyle = 'rgba(255,255,255,.12)';
  ctx.beginPath(); ctx.moveTo(0, H / 2); ctx.lineTo(W, H / 2); ctx.stroke();
  ctx.strokeStyle = '#5aa9e6'; ctx.lineWidth = 1.5; ctx.beginPath();
  pts.forEach(function (p, i) {
    var x = i / (pts.length - 1) * W, y = H / 2 - (p.d / max) * (H / 2 - 5);
    if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
  });
  ctx.stroke();
  ctx.fillStyle = '#e0a852';
  pts.forEach(function (p, i) {
    if (!p.c) return;
    var x = i / (pts.length - 1) * W, y = H / 2 - (p.d / max) * (H / 2 - 5);
    ctx.beginPath(); ctx.arc(x, y, 2.6, 0, 6.3); ctx.fill();
  });
}

function paintGuide(g) {
  if (!g) return;
  if (document.activeElement !== $('#guide-on')) $('#guide-on').checked = !!g.running;
  $('#guide-state').textContent = g.running ? 'guiding' : 'off';
  $('#guide-drift').textContent = (typeof g.last_drift === 'number' && g.last_drift >= 0)
    ? g.last_drift.toFixed(1) + '″' : '—';
  $('#guide-params').textContent = 'every ' + g.interval + 's · > ' + g.threshold + '″';
  if (g.log != null) { var lp = $('#guide-log'); lp.textContent = g.log || '—'; lp.scrollTop = lp.scrollHeight; }
  drawSpark(g.points || []);
}

function guideGet() {
  return fetch('/api/guide', { cache: 'no-store' })
    .then(function (r) { return r.json(); }).then(paintGuide).catch(function () {});
}

function focalGet() {
  return fetch('/api/focal', { cache: 'no-store' })
    .then(function (r) { return r.json(); }).then(function (f) {
      if (!f) return;
      $('#solve-focal-note').textContent = 'using ' + (
        f.override != null ? f.override + ' mm (manual override)'
        : f.exif_cache != null ? f.exif_cache + ' mm (from the last frame’s EXIF)'
        : f.config + ' mm (config default)');
      if (f.override != null && document.activeElement !== $('#solve-focal'))
        $('#solve-focal').value = f.override;
    }).catch(function () {});
}

function wireObservatoryTools() {
  var solve = function (mode) {
    var apply = $('#solve-align').checked ? 1 : 0;
    post('/api/solve?mode=' + mode + '&apply=' + apply, {}).then(function (r) {
      if (r && r.started === false) setHint('<b>Solve:</b> ' + (r.reason || 'already running'));
      solveGet();
    });
  };
  $('#solve-now').addEventListener('click', function () { solve('capture'); });
  $('#solve-latest').addEventListener('click', function () { solve('latest'); });
  $('#solve-apply').addEventListener('click', function () {
    post('/api/apply', {}).then(function (r) {
      if (r && r.ok === false) setHint('<b>Apply:</b> ' + (r.error || ''));
      solveGet();
    });
  });
  $('#solve-cancel').addEventListener('click', function () { post('/api/cancel', {}).then(function () { solveGet(); }); });
  $('#solve-focal-set').addEventListener('click', function () {
    post('/api/focal', { focal: $('#solve-focal').value || 'auto' }).then(function (r) {
      if (r && r.ok === false) setHint('<b>Focal:</b> ' + (r.error || ''));
      focalGet();
    });
  });
  $('#guide-on').addEventListener('change', function () {
    var on = this.checked;
    post('/api/guide', { on: on ? 1 : 0 }).then(function (r) {
      if (r && r.ok === false) { setHint('<b>Guiding:</b> ' + (r.error || '')); $('#guide-on').checked = false; }
      guideGet();
    });
  });
  $('#st-refresh').addEventListener('click', function () { solveGet(true); });
  $('#br-port').textContent = location.port || '80';
  $('#br-lx').textContent = '10001';
  focalGet();
}

function astroPollStart() {
  clearInterval(astroPoll);
  if ($('#tab-astro').hidden) return;
  var tick = function () {
    astroRefresh(); solveGet(); guideGet();
    progGet().then(paintAstroCapture).catch(function () {});
  };
  tick();
  astroPoll = setInterval(tick, 2000);
}

/* ─────────────────────────────── free program ──────────────────────────── */

/* A keyframe timeline flown by hand: the poses live SERVER-side (captured from
   the head's live attitude), so the client only holds the display copy. */
var plcKeys = [];   /* [{t, pan, tilt, roll}] mirrored from the server */

function plcRenderKeys() {
  var ol = $('#plc-keys');
  if (!plcKeys.length) { ol.innerHTML = '<li class="dim">none yet — jog the head, then capture a pose</li>'; }
  else ol.innerHTML = plcKeys.map(function (k, i) {
    return '<li>t=' + k.t + 's · pan ' + (k.pan * 180 / Math.PI).toFixed(1) +
           '° tilt ' + (k.tilt * 180 / Math.PI).toFixed(1) + '°</li>';
  }).join('');
  var start = $('#plc-start');
  start.disabled = plcKeys.length < 2;
  start.textContent = plcKeys.length < 2 ? 'Add at least two keyframes' : 'Preview program';
  if (plcKeys.length < 2) { start.dataset.armed = '0'; start.classList.remove('armed'); $('#plc-preview').hidden = true; }
}

function wirePlc() {
  var cumT = 0;
  $('#plc-capture').addEventListener('click', function () {
    /* The time of this keyframe is the running sum of the per-segment durations;
       the first is at t=0. The server reads the head's CURRENT pose. */
    var t = plcKeys.length === 0 ? 0 : cumT + (parseInt($('#plc-segdur').value, 10) || 10);
    post('/api/prog/plc/key', { t: t }).then(function (r) {
      if (r && r.ok === false) { setHint('<b>Keyframe:</b> ' + (r.error || '')); return; }
      cumT = t;
      /* Mirror what the head reported, for the list. Pull the pose from 517. */
      var pose = S.ops['517'] ? kv(S.ops['517'].args) : {};
      plcKeys.push({ t: t, pan: parseFloat(pose.yaw || 0), tilt: parseFloat(pose.pitch || 0), roll: parseFloat(pose.roll || 0) });
      plcRenderKeys();
    });
  });
  $('#plc-clear').addEventListener('click', function () {
    post('/api/prog/plc/clear', {}).then(function () { plcKeys = []; cumT = 0; plcRenderKeys(); });
  });
  $('#plc-photo-inf').addEventListener('click', function () { $('#plc-photo-n').value = ''; });

  var start = $('#plc-start');
  start.addEventListener('click', function () {
    var params = {
      photo_interval: parseFloat($('#plc-photo-iv').value) || 0,
      photo_count: $('#plc-photo-n').value.trim() === '' ? -1 : parseInt($('#plc-photo-n').value, 10),
      hold: $('#plc-hold').checked ? 1 : 0
    };
    if (start.dataset.armed !== '1') {
      post('/api/prog/plc', params).then(function (r) {
        if (r && r.valid === false) { setHint('<b>Program:</b> ' + (r.error || '')); return; }
        setHint('');
        $('#plc-frame').textContent = r.frame;
        $('#plc-preview').hidden = false;
        start.dataset.armed = '1'; start.classList.add('armed');
        start.textContent = 'Run this move — ' + r.keys + ' keyframes';
      });
      return;
    }
    post('/api/prog/plc', Object.assign(params, { confirm: 1 })).then(function (r) {
      start.dataset.armed = '0'; start.classList.remove('armed');
      start.textContent = 'Preview program';
      $('#plc-preview').hidden = true;
      if (r && r.ok === false) setHint('<b>Program refused.</b> ' + (r.error || ''));
      progRefresh();
    });
  });
  plcRenderKeys();
}

/* ─────────────────────────── path-lapse ────────────────────────────────── */

/* A moving timelapse. Like the free program, the waypoint POSES live server-
   side (captured from the head's live attitude); the client keeps a display
   copy plus each leg's frame count so it can show the derived totals. */
var plWps = [];    /* [{pan, tilt, roll, count}] mirrored from the server */
var plPending = 0; /* waypoints requested (synchronous), to number legs safely */

function plLegInf() { return $('#pl-legframes').dataset.inf === '1'; }

function plRender() {
  var ol = $('#pl-wps');
  if (!plWps.length) {
    ol.innerHTML = '<li class="dim">none yet — jog the head, then capture a waypoint</li>';
  } else ol.innerHTML = plWps.map(function (w, i) {
    var lead = i === 0 ? 'start' : (w.count < 0 ? '∞ frames' : w.count + ' frames');
    return '<li>#' + (i + 1) + ' · pan ' + (w.pan * 180 / Math.PI).toFixed(1) +
           '° tilt ' + (w.tilt * 180 / Math.PI).toFixed(1) + '° · ' + lead + '</li>';
  }).join('');

  /* Derived: sum the legs (skip the start; an ∞ leg makes the whole run ∞). */
  var iv = parseFloat($('#pl-interval').value) || 0, total = 0, inf = false, i;
  for (i = 1; i < plWps.length; i++) {
    if (plWps[i].count < 0) inf = true; else total += plWps[i].count;
  }
  $('#pl-frames').textContent = plWps.length < 2 ? '—' : (inf ? '∞' : total);
  $('#pl-time').textContent = plWps.length < 2 ? '—' : (inf ? '∞' : fmtDuration(total * iv));

  var start = $('#pl-start');
  start.disabled = plWps.length < 2;
  start.textContent = plWps.length < 2 ? 'Add at least two waypoints' : 'Preview path-lapse';
  if (plWps.length < 2) { start.dataset.armed = '0'; start.classList.remove('armed'); $('#pl-preview').hidden = true; }
}

function wirePathlapse() {
  $('#pl-leg-inf').addEventListener('click', function () {
    var f = $('#pl-legframes'), on = f.dataset.inf === '1';
    f.dataset.inf = on ? '0' : '1';
    f.disabled = !on;                     /* disable the number field while ∞ */
    this.classList.toggle('armed', !on);
  });

  $('#pl-interval').addEventListener('input', plRender);

  $('#pl-capture').addEventListener('click', function () {
    /* The first waypoint is the start; its count is unused. Later ones carry the
       frames on the leg reaching them. The server reads the head's LIVE pose.
       `plPending` counts requested waypoints SYNCHRONOUSLY so two quick taps
       (faster than a round-trip) still number their legs correctly — keying off
       plWps.length would let the second tap think it is the start too. */
    var first = plPending === 0;
    var count = first ? 0 : (plLegInf() ? -1 : (parseInt($('#pl-legframes').value, 10) || 1));
    plPending++;
    post('/api/prog/pathlapse/wp', { count: count }).then(function (r) {
      if (r && r.ok === false) { plPending--; setHint('<b>Waypoint:</b> ' + (r.error || '')); return; }
      var pose = S.ops['517'] ? kv(S.ops['517'].args) : {};
      plWps.push({ pan: parseFloat(pose.yaw || 0), tilt: parseFloat(pose.pitch || 0),
                   roll: parseFloat(pose.roll || 0), count: count });
      plRender();
    });
  });

  $('#pl-clear').addEventListener('click', function () {
    post('/api/prog/pathlapse/clear', {}).then(function () { plWps = []; plPending = 0; plRender(); });
  });

  var start = $('#pl-start');
  start.addEventListener('click', function () {
    var params = { interval: parseFloat($('#pl-interval').value) || 4 };
    if (start.dataset.armed !== '1') {
      post('/api/prog/pathlapse', params).then(function (r) {
        if (r && r.valid === false) { setHint('<b>Path-lapse:</b> ' + (r.error || '')); return; }
        setHint('');
        $('#pl-frame').textContent = r.frame;
        $('#pl-preview').hidden = false;
        start.dataset.armed = '1'; start.classList.add('armed');
        start.textContent = 'Run it — ' + r.waypoints + ' waypoints';
      });
      return;
    }
    post('/api/prog/pathlapse', Object.assign(params, { confirm: 1 })).then(function (r) {
      start.dataset.armed = '0'; start.classList.remove('armed');
      start.textContent = 'Preview path-lapse';
      $('#pl-preview').hidden = true;
      if (r && r.ok === false) setHint('<b>Path-lapse refused.</b> ' + (r.error || ''));
      progRefresh();
    });
  });

  plRender();
}

/* ────────────────────────────── sun ────────────────────────────────────── */

/* A scheduled solar lapse. The window is validated with the CLIENT clock (the
   device clock is not UTC), so `now` rides along on every request. */
var sunEvent = 0;   /* 0 sunrise, 1 sunset */

function sunLocalValue(d) {
  /* datetime-local wants "YYYY-MM-DDTHH:MM:SS" in LOCAL time, no zone. */
  var p = function (n) { return (n < 10 ? '0' : '') + n; };
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + 'T' +
         p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
}

function sunUnix(sel) {
  var v = $(sel).value;
  if (!v) return NaN;
  return Math.floor(new Date(v).getTime() / 1000);   /* parsed as LOCAL time */
}

function sunRender() {
  var s = sunUnix('#sun-start'), e = sunUnix('#sun-end');
  var iv = parseFloat($('#sun-interval').value) || 0;
  if (isNaN(s) || isNaN(e) || e <= s) {
    $('#sun-window').textContent = '—'; $('#sun-frames').textContent = '—';
  } else {
    $('#sun-window').textContent = fmtDuration(e - s);
    $('#sun-frames').textContent = iv > 0 ? '≈ ' + Math.floor((e - s) / iv) : '—';
  }
  var b = $('#sun-start-btn');   /* editing invalidates a shown preview */
  if (b.dataset.armed === '1') { b.dataset.armed = '0'; b.classList.remove('armed'); b.textContent = 'Preview sun lapse'; $('#sun-preview').hidden = true; }
}

function wireSun() {
  $$('#tab-programs [data-sun]').forEach(function (b) {
    b.addEventListener('click', function () {
      $$('#tab-programs [data-sun]').forEach(function (x) { x.classList.toggle('on', x === b); });
      sunEvent = parseInt(b.dataset.sun, 10);
      sunRender();
    });
  });

  $('#sun-fill').addEventListener('click', function () {
    var now = new Date(), end = new Date(now.getTime() + 30 * 60 * 1000);
    $('#sun-start').value = sunLocalValue(now);
    $('#sun-end').value = sunLocalValue(end);
    sunRender();
  });

  ['#sun-interval', '#sun-start', '#sun-end'].forEach(function (sel) {
    $(sel).addEventListener('input', sunRender);
  });

  var start = $('#sun-start-btn');
  start.addEventListener('click', function () {
    var s = sunUnix('#sun-start'), e = sunUnix('#sun-end');
    if (isNaN(s) || isNaN(e)) { setHint('<b>Sun:</b> set a start and end time (try “Now → +30 min”).'); return; }
    var params = { sunset: sunEvent, interval: parseFloat($('#sun-interval').value) || 10,
                   start: s, end: e, now: Math.floor(Date.now() / 1000) };
    if (start.dataset.armed !== '1') {
      post('/api/prog/sun', params).then(function (r) {
        if (r && r.valid === false) { setHint('<b>Sun:</b> ' + (r.error || '')); return; }
        setHint('');
        $('#sun-frame').textContent = r.frame;
        $('#sun-preview').hidden = false;
        start.dataset.armed = '1'; start.classList.add('armed');
        start.textContent = 'Schedule it';
      });
      return;
    }
    post('/api/prog/sun', Object.assign(params, { confirm: 1 })).then(function (r) {
      start.dataset.armed = '0'; start.classList.remove('armed');
      start.textContent = 'Preview sun lapse';
      $('#sun-preview').hidden = true;
      if (r && r.ok === false) setHint('<b>Sun refused.</b> ' + (r.error || ''));
      progRefresh();
    });
  });

  sunRender();
}

/* ────────────────────────── holy grail ─────────────────────────────────── */

/* The day->night exposure ramp. NOT a run — it uploads a target-brightness
   curve + optional exposure limits the head applies during a timelapse/
   path-lapse, metering via the external accessory. Curve nodes live here; the
   axis ranges come from the camera's own lists so nothing is invented. */
var hgCurve = [{ dmin: 0, ev: 0 }];   /* node 0 is the mandatory anchor */

function hgInvalidate() {
  var b = $('#hg-apply');
  b.dataset.armed = '0'; b.classList.remove('armed');
  b.textContent = 'Preview ramp config'; $('#hg-preview').hidden = true;
}

function hgRenderNodes() {
  var ol = $('#hg-nodes');
  ol.innerHTML = hgCurve.map(function (n, i) {
    var h = Math.floor(n.dmin / 60), m = n.dmin % 60;
    var t = i === 0 ? 'start' : (h + ':' + (m < 10 ? '0' : '') + m);
    var evTxt = (n.ev > 0 ? '+' : '') + n.ev.toFixed(1) + ' EV';
    var anchor = i === 0 ? ' <span class="dim">(anchor)</span>' : '';
    var rm = i === 0 ? '' : ' <button class="linkbtn" data-hg-rm="' + i + '">remove</button>';
    return '<li>' + t + ' → ' + evTxt + anchor + rm + '</li>';
  }).join('');
  $$('#hg-nodes [data-hg-rm]').forEach(function (b) {
    b.addEventListener('click', function () {
      hgCurve.splice(parseInt(b.dataset.hgRm, 10), 1);
      hgInvalidate(); hgRenderNodes();
    });
  });
}

/* Fill a <select> from a camera option list (an opcode's cached R: list). mode
   'index' sends the list POSITION (shutter handles are 268 indices); 'value'
   parses the leading number from the label (ISO integers, aperture f-numbers).
   In 'value' mode non-numeric entries (the camera's "Auto") are dropped and, if
   `cap` is given, values above it too — the app does exactly this for Holy Grail
   ISO (Auto filtered, capped at 6400; features pitfall 15). */
function hgFill(sel, opcode, mode, cap) {
  var el = $(sel); if (!el) return;
  var slot = S.ops[opcode], list = slot ? kv(slot.args).R : '';
  var prev = el.value, html = '';
  if (!list) { el.innerHTML = '<option value="">— open Control tab —</option>'; return; }
  list.split(',').forEach(function (label, i) {
    if (mode === 'value') {
      var v = parseFloat(label);
      if (!(v > 0)) return;                 /* drop "Auto" / non-numeric */
      if (cap && v > cap) return;           /* ISO capped */
      html += '<option value="' + v + '">' + label + '</option>';
    } else {
      html += '<option value="' + i + '">' + label + '</option>';
    }
  });
  el.innerHTML = html || '<option value="">— open Control tab —</option>';
  if (prev) el.value = prev;
}

function hgFillAll() {
  hgFill('#hg-s0', '268', 'index'); hgFill('#hg-s1', '268', 'index');
  hgFill('#hg-s2', '268', 'index'); hgFill('#hg-s3', '268', 'index');
  hgFill('#hg-iso-lo', '265', 'value', 6400); hgFill('#hg-iso-hi', '265', 'value', 6400);
  hgFill('#hg-f-lo', '275', 'value'); hgFill('#hg-f-hi', '275', 'value');
}

/* Ask the head for its grail state + runtime brightness. `syncEnable` mirrors
   the head's enable flag into the checkbox — done only on panel-open, never on
   the background poll, so it can't fight the user mid-toggle. */
function hgFetchStatus(syncEnable) {
  return post('/api/prog/grail/status', {}).then(function (r) {
    if (!r) return;
    if (syncEnable && typeof r.enabled === 'boolean') $('#hg-enable').checked = r.enabled;
    var b = r.brightness ? kv(r.brightness).brightness : '';
    $('#hg-brightness').textContent = (b !== undefined && b !== '') ? b : '— (no accessory)';
  }).catch(function () {});
}

/* On panel-open: (re)load the camera lists into the axis selects and sync state.
   Brightness needs the external accessory, and its reply lands a poll late, so
   it converges over the Programs-tab poll rather than appearing instantly. */
function hgOnShow() { hgFillAll(); hgFetchStatus(true); }

function hgParams() {
  var p = {
    enable: $('#hg-enable').checked ? 1 : 0,
    curve: hgCurve.map(function (n) { return n.dmin + '/' + n.ev; }).join(',')
  };
  if ($('#hg-s-en').checked) {
    p.s_en = 1;
    p.shutter = [$('#hg-s0').value, $('#hg-s1').value, $('#hg-s2').value, $('#hg-s3').value].join(',');
  }
  if ($('#hg-iso-en').checked) { p.iso_en = 1; p.iso_lo = $('#hg-iso-lo').value; p.iso_hi = $('#hg-iso-hi').value; }
  if ($('#hg-f-en').checked)   { p.f_en = 1;   p.f_lo = $('#hg-f-lo').value;   p.f_hi = $('#hg-f-hi').value; }
  if ($('#hg-prio-en').checked) p.priority = '0,1,2';   /* S -> ISO -> F */
  return p;
}

function wireGrail() {
  hgRenderNodes();

  $('#hg-node-add').addEventListener('click', function () {
    var dmin = Math.round((parseInt($('#hg-node-min').value, 10) || 0) / 30) * 30;
    var ev = Math.round((parseFloat($('#hg-node-ev').value) || 0) * 2) / 2;
    if (dmin < 0) dmin = 0; if (dmin > 1440) dmin = 1440;
    if (ev < -5) ev = -5; if (ev > 5) ev = 5;
    /* replace any point at the same offset, else insert in time order */
    hgCurve = hgCurve.filter(function (n) { return n.dmin !== dmin; });
    hgCurve.push({ dmin: dmin, ev: ev });
    hgCurve.sort(function (a, b) { return a.dmin - b.dmin; });
    hgInvalidate(); hgRenderNodes();
  });

  $('#hg-node-reset').addEventListener('click', function () {
    hgCurve = [{ dmin: 0, ev: 0 }]; hgInvalidate(); hgRenderNodes();
  });

  ['#hg-enable', '#hg-s-en', '#hg-iso-en', '#hg-f-en', '#hg-prio-en',
   '#hg-s0', '#hg-s1', '#hg-s2', '#hg-s3', '#hg-iso-lo', '#hg-iso-hi', '#hg-f-lo', '#hg-f-hi']
    .forEach(function (sel) { var el = $(sel); if (el) el.addEventListener('change', hgInvalidate); });

  $('#hg-off').addEventListener('click', function () {
    post('/api/prog/grail/off', {}).then(function (r) {
      $('#hg-enable').checked = false;
      setHint(r && r.ok === false ? '<b>Holy Grail:</b> ' + (r.error || '') : '');
      hgInvalidate();
    });
  });

  var apply = $('#hg-apply');
  apply.addEventListener('click', function () {
    var params = hgParams();
    if (apply.dataset.armed !== '1') {
      post('/api/prog/grail', params).then(function (r) {
        if (r && r.valid === false) { setHint('<b>Holy Grail:</b> ' + (r.error || '')); return; }
        setHint('');
        $('#hg-frame').textContent = r.frame;
        $('#hg-preview').hidden = false;
        apply.dataset.armed = '1'; apply.classList.add('armed');
        apply.textContent = 'Upload ramp config';
      });
      return;
    }
    post('/api/prog/grail', Object.assign(params, { confirm: 1 })).then(function (r) {
      apply.dataset.armed = '0'; apply.classList.remove('armed');
      apply.textContent = 'Preview ramp config';
      $('#hg-preview').hidden = true;
      if (r && r.ok === false) setHint('<b>Holy Grail refused.</b> ' + (r.error || ''));
      else setHint('');
    });
  });
}

/* ──────────────────────────────── tabs ─────────────────────────────────── */

function wireTabs() {
  $$('#tabs button').forEach(function (b) {
    b.addEventListener('click', function () {
      $$('#tabs button').forEach(function (x) { x.classList.toggle('on', x === b); });
      $$('.tab').forEach(function (t) { t.hidden = t.id !== 'tab-' + b.dataset.tab; });
      /* Leaving the control tab must not leave an axis running. */
      if (b.dataset.tab !== 'control') stopEverything();
      if (b.dataset.tab === 'programs') progPoll(); else clearTimeout(progTimer);
      if (b.dataset.tab === 'astro') astroPollStart(); else clearInterval(astroPoll);
    });
  });
}

/* ──────────────────────────────── boot ─────────────────────────────────── */

wireTabs();
wireJog();
wireSticks();
wireStage();
wireOverlays();
wireExposure();
wireShutter();
wireSettings();
wireHeadSettings();
wirePrograms();
wireAstro();
wirePlc();
wirePathlapse();
wireSun();
wireGrail();
poll();
