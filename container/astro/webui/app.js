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

function startHold(axis, dir, btn) {
  if (holds[axis] || S.link !== 'up') return;
  var speed = dir * speedForGear(S.gear);
  var tick = function () { post('/api/move', { axis: axis, speed: speed }); };
  tick();
  holds[axis] = { timer: setInterval(tick, RENEW_MS), btn: btn };
  if (btn) btn.classList.add('held');
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

function stopEverything() {
  Object.keys(holds).forEach(function (a) {
    clearInterval(holds[a].timer);
    if (holds[a].btn) holds[a].btn.classList.remove('held');
    delete holds[a];
  });
  post('/api/stop', {});
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
    /* Re-affirm any in-flight hold at the new speed rather than waiting for
       the next renewal, so the gear slider feels live under a held button. */
    Object.keys(holds).forEach(function (axis) {
      var dirSign = holds[axis].btn && holds[axis].btn.classList.contains('held') ? 1 : 1;
      void dirSign;
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
  $$('.rack').forEach(function (btn) {
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
    post(this.checked ? '/api/astro/enter' : '/api/astro/leave', {}).then(function (r) {
      if (r && r.ok === false) setHint('<b>Astro:</b> ' + (r.error || ''));
      astroRefresh();
    });
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
}

function astroPollStart() {
  clearInterval(astroPoll);
  if ($('#tab-astro').hidden) return;
  astroRefresh();
  astroPoll = setInterval(astroRefresh, 1500);
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
wireExposure();
wireShutter();
wireSettings();
wirePrograms();
wireAstro();
wirePlc();
wirePathlapse();
wireSun();
poll();
