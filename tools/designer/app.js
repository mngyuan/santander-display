// Dock display designer. Every frame is drawn by the firmware's own code
// (ui.h + render.cpp) through serve.py's /api/frame; this file only feeds it
// data, mocks the buttons and edits docks.json.
'use strict';

const W = 250, H = 122;
const MAX_SCREENS = 4, LABEL_LEN = 16;
// Same timings as the firmware's config.example.h.
const SCREEN_STALE_MS = 120e3, SCROLL_SETTLE_MS = 1500, MIN_FETCH_GAP_MS = 20e3;
const ROLES = ['home', 'dest'];
const ROLE_NAMES = { home: 'Home dock (pick up)', dest: 'Destination (drop off)' };

const SCENARIOS = {
  typical: 'Typical',
  emptyFull: 'Home empty, destination full',
  closed: 'Destination closed',
  manyE: 'Lots of e-bikes',
  battDown: 'Battery feed down',
  stale: 'Last fetch failed',
  never: 'Never fetched',
};

const $ = (sel) => document.querySelector(sel);

const store = {
  get(key, fallback) {
    try { const v = localStorage.getItem('dock-designer.' + key); return v === null ? fallback : JSON.parse(v); }
    catch { return fallback; }
  },
  set(key, value) {
    try { localStorage.setItem('dock-designer.' + key, JSON.stringify(value)); } catch { /* private window */ }
  },
};

const state = {
  mode: store.get('mode', 'sample'),
  scenario: store.get('scenario', 'typical'),
  config: { screens: [] },
  savedJson: '',
  ui: { screen: 0, nScreens: 1, status: false },
  slots: new Array(MAX_SCREENS * 2).fill(null), // per slot: counts, batt[], at (ms), key
  lastTry: 0,     // ms of the last fetch attempt
  lastOk: 0,      // ms of the last fetch that got the shown screen's counts
  lastErr: '',
  busy: false,
  fetches: 0,
  docks: [],      // TfL dock list for the dropdowns and live data
  dockById: new Map(),
};

// ------------------------------------------------------------------ helpers
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const pad = (n) => String(n).padStart(2, '0');
const hm = (ms) => { const d = new Date(ms); return `${pad(d.getHours())}:${pad(d.getMinutes())}`; };
const minutesOf = (ms) => { const d = new Date(ms); return d.getHours() * 60 + d.getMinutes(); };
const ascii = (s) => String(s).replace(/[^ -~]/g, '');

async function api(method, path, body) {
  const res = await fetch(path, {
    method,
    headers: body ? { 'Content-Type': 'application/json' } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  let data;
  try { data = await res.json(); } catch { data = { error: `HTTP ${res.status}` }; }
  if (!res.ok && !data.error && !data.errors) data.error = `HTTP ${res.status}`;
  return data;
}

function log(text, isError = false) {
  const li = document.createElement('li');
  li.textContent = `${hm(Date.now())}  ${text}`;
  if (isError) li.className = 'err';
  const list = $('#log');
  list.prepend(li);
  while (list.children.length > 80) list.lastChild.remove();
}

// "Wenlock Road , Hoxton" -> "WENLOCK RD"
const ABBREV = {
  road: 'RD', street: 'ST', station: 'STN', square: 'SQ', avenue: 'AVE', place: 'PL', lane: 'LN',
  gardens: 'GDNS', terrace: 'TER', bridge: 'BR', court: 'CT', park: 'PK', gate: 'GT', crescent: 'CRES',
  walk: 'WK', hill: 'HL', green: 'GRN', grove: 'GR', close: 'CL', north: 'N', south: 'S', east: 'E',
  west: 'W', saint: 'ST', 'st.': 'ST', embankment: 'EMB', market: 'MKT',
};
function shortLabel(name) {
  const first = String(name || '').split(',')[0];
  const words = ascii(first).split(/\s+/).filter(Boolean).map((w) => ABBREV[w.toLowerCase()] || w);
  return words.join(' ').toUpperCase().slice(0, LABEL_LEN).trim();
}

// ------------------------------------------------------------------ config
const slotRef = (slot) => state.config.screens[Math.floor(slot / 2)]?.[ROLES[slot % 2]];
const dockKey = (ref) => (ref ? (ref.id > 0 ? `id:${ref.id}` : `q:${ref.query || ''}`) : '');

function cleanConfig() {
  return {
    screens: state.config.screens.map((s) => Object.fromEntries(ROLES.map((role) => {
      const d = s[role] || {};
      const id = Number.isInteger(d.id) && d.id > 0 ? d.id : 0;
      const out = { id };
      if (id === 0) out.query = d.query || '';
      out.name = d.name || d.query || '';
      out.label = d.label || '';
      return [role, out];
    }))),
  };
}

function isDirty() { return JSON.stringify(cleanConfig()) !== state.savedJson; }

function setConfig(cfg, saved) {
  state.config = cfg;
  if (saved) state.savedJson = JSON.stringify(cleanConfig());
  state.ui.nScreens = Math.max(1, cfg.screens.length);
  if (state.ui.screen >= state.ui.nScreens) state.ui.screen = 0;
  syncSlots();
  renderEditor();
  redrawSoon();
}

// ------------------------------------------------------------------ data
function rng(seed) {
  let s = (seed >>> 0) || 1;
  return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s / 2 ** 32; };
}
function hashText(text) { let h = 2166136261; for (const c of text) h = Math.imul(h ^ c.charCodeAt(0), 16777619); return h >>> 0; }

// Made-up but plausible numbers, stable per dock, nudged by `salt` per fetch.
function sampleDock(ref, role, scenario, salt) {
  const seed = (ref?.id > 0 ? ref.id : hashText(ref?.query || '')) * 7919 + role * 31 + salt;
  const r = rng(seed);
  const docks = 18 + Math.floor(r() * 28);
  const bikes = Math.floor(r() * docks * 0.6);
  const ebikes = Math.min(Math.floor(r() * 5), docks - bikes);
  const d = { hasCounts: true, closed: false, bikes, ebikes, spaces: docks - bikes - ebikes, docks, hasBatt: true, batt: [] };
  for (let i = 0; i < ebikes; i++) d.batt.push(5 + Math.floor(r() * 96));
  switch (scenario) {
    case 'emptyFull':
      if (role === 0) Object.assign(d, { bikes: 0, ebikes: 0, batt: [], spaces: docks });
      else Object.assign(d, { bikes: docks - 2, ebikes: 2, batt: [71, 40], spaces: 0 });
      break;
    case 'closed':
      if (role === 1) d.closed = true;
      break;
    case 'manyE':
      if (role === 0) {
        const b = Math.min(bikes, 3);
        Object.assign(d, { bikes: b, ebikes: 11, batt: [95, 88, 80, 77, 61, 52, 40, 33, 20, 12, 4], spaces: Math.max(0, docks - b - 11) });
      }
      break;
    case 'battDown':
      Object.assign(d, { hasBatt: false, batt: [] });
      break;
    case 'never':
      return { hasCounts: false, hasBatt: false, batt: [] };
  }
  return d;
}

// Fills slots for docks that changed in the editor.
function syncSlots() {
  const n = state.config.screens.length * 2;
  let needLive = false;
  for (let s = 0; s < state.slots.length; s++) {
    const ref = s < n ? slotRef(s) : null;
    if (!ref) { state.slots[s] = null; continue; }
    const key = dockKey(ref);
    if (state.slots[s]?.key === key) continue;
    if (state.mode === 'sample') {
      const now = Date.now();
      const at = state.scenario === 'never' ? 0 : state.scenario === 'stale' ? now - 25 * 60e3 : now - 60e3;
      state.slots[s] = { key, at, ...sampleDock(ref, s % 2, state.scenario, state.fetches) };
    } else {
      state.slots[s] = { key, at: 0, hasCounts: false, hasBatt: false, batt: [] };
      needLive = true;
    }
  }
  if (needLive) loadLive().then(redrawSoon);
}

function setScenario(name) {
  state.scenario = name;
  store.set('scenario', name);
  const now = Date.now();
  state.fetches = 0;
  state.slots.fill(null);
  // Sample data starts as if fetched a minute ago, so the first dial press
  // isn't swallowed by the 20 s gap between fetches.
  state.lastTry = now - 60e3;
  const failed = name === 'stale' || name === 'never';
  state.lastOk = name === 'stale' ? now - 25 * 60e3 : failed ? 0 : now - 60e3;
  state.lastErr = failed ? 'wifi' : '';
  syncSlots();
}

function resolveLive(ref) {
  if (ref.id > 0) return state.dockById.get(ref.id);
  const q = (ref.query || '').toLowerCase();
  return state.docks.find((d) => d.name.toLowerCase().startsWith(q)) || state.docks.find((d) => d.name.toLowerCase().includes(q));
}

async function loadDockList() {
  const r = await api('GET', '/api/bikepoints');
  if (r.error) throw new Error(r.error);
  state.docks = r.docks;
  state.dockById = new Map(r.docks.map((d) => [d.id, d]));
  for (const screen of state.config.screens) {
    for (const role of ROLES) {
      const d = screen[role];
      if (d?.id > 0 && !d.name && state.dockById.has(d.id)) d.name = state.dockById.get(d.id).name;
    }
  }
  const list = $('#docklist');
  list.textContent = '';
  for (const d of r.docks) {
    const opt = document.createElement('option');
    opt.value = `${d.name} (${d.id})`;
    list.append(opt);
  }
  return r.docks.length;
}

// Live numbers for every configured dock, like the device's fetch.
async function loadLive() {
  state.lastTry = Date.now();
  let shownOk = true;
  try {
    await loadDockList();
  } catch (e) {
    state.lastErr = 'tfl';
    log(`live: TfL dock list failed: ${e.message}`, true);
    return;
  }
  const now = Date.now();
  const ids = new Set();
  state.slots.forEach((slot, s) => {
    if (!slot) return;
    const live = resolveLive(slotRef(s));
    if (!live) { if (Math.floor(s / 2) === state.ui.screen) shownOk = false; return; }
    ids.add(live.id);
    Object.assign(slot, { liveId: live.id, at: now, hasCounts: live.hasCounts, closed: live.closed, bikes: live.bikes, ebikes: live.ebikes, spaces: live.spaces, docks: live.docks });
  });
  const b = await api('GET', `/api/batteries?ids=${[...ids].join(',')}`);
  state.slots.forEach((slot) => {
    if (!slot) return;
    const list = b.batteries?.[String(slot.liveId)];
    slot.hasBatt = Array.isArray(list);
    slot.batt = list || [];
  });
  if (b.error) log(`live: battery feed failed: ${b.error}`, true);
  else log(`live: ${state.docks.length} TfL docks, battery feed ${b.stations} stations`);
  state.lastErr = shownOk ? (b.error ? 'gql' : '') : 'no counts';
  if (shownOk) state.lastOk = now;
}

async function loadSample() {
  await sleep(700); // a fetch takes a few seconds on the device
  state.lastTry = Date.now();
  if (state.scenario === 'stale' || state.scenario === 'never') {
    state.lastErr = 'wifi';
    log('sample: fetch "failed" (this state simulates a failure)', true);
    return;
  }
  state.fetches++;
  const now = Date.now();
  state.slots.forEach((slot, s) => {
    if (slot) Object.assign(slot, sampleDock(slotRef(s), s % 2, state.scenario, state.fetches), { at: now });
  });
  state.lastOk = now;
  state.lastErr = '';
}

// ------------------------------------------------------------------ frames
function slotPayload(slots) {
  return slots.map((slot, s) => {
    const ref = slotRef(s);
    if (!ref) return null;
    const d = slot || {};
    return {
      id: ref.id || d.liveId || 0, label: ref.label || '', hasCounts: !!d.hasCounts, closed: !!d.closed,
      bikes: d.bikes || 0, ebikes: d.ebikes || 0, spaces: d.spaces || 0, docks: d.docks ?? -1,
      hasBatt: !!d.hasBatt, batt: d.batt || [],
    };
  });
}

// Mirrors drawCurrent() in dock_display.ino.
function deviceCtx(slots, screen, lastTry, busy) {
  const a = slots[screen * 2]?.at || 0, b = slots[screen * 2 + 1]?.at || 0;
  const t = a && b ? Math.min(a, b) : 0;
  return { dataMin: t ? minutesOf(t) : -1, stale: !!lastTry && (a < lastTry || b < lastTry), busy };
}

function statusLines() {
  const s = state.ui.screen;
  const id = (slot) => slotRef(slot)?.id || state.slots[slot]?.liveId || '?';
  return [
    state.lastOk ? `synced ${hm(state.lastOk)}` : 'synced never',
    state.lastErr ? `err: ${state.lastErr}` : 'wifi -58 dBm',
    `next ${hm(Date.now() + 5 * 60e3)}`,
    `screen ${s + 1}: ${id(s * 2)}/${id(s * 2 + 1)}`,
    'fw 0.2.0',
  ];
}

function paint(canvas, bits) {
  const tint = $('#tint').checked;
  const css = getComputedStyle(document.documentElement);
  const paper = hexRgb(tint ? css.getPropertyValue('--paper') : '#ffffff');
  const ink = hexRgb(tint ? css.getPropertyValue('--ink') : '#000000');
  const ctx = canvas.getContext('2d');
  const img = ctx.createImageData(W, H);
  for (let i = 0; i < W * H; i++) {
    const c = bits.charCodeAt(i) === 49 ? ink : paper;
    img.data.set([c[0], c[1], c[2], 255], i * 4);
  }
  ctx.putImageData(img, 0, 0);
}
function hexRgb(hex) {
  const h = hex.trim().replace('#', '');
  return [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16));
}

let lastBits = null;
async function renderMain({ key = null, message = null } = {}) {
  const r = await api('POST', '/api/frame', {
    ui: state.ui, key,
    ctx: deviceCtx(state.slots, state.ui.screen, state.lastTry, state.busy),
    slots: slotPayload(state.slots), status: statusLines(), message,
  });
  const errBox = $('#render-error');
  if (r.error) { errBox.textContent = r.error; errBox.hidden = false; return null; }
  errBox.hidden = true;
  state.ui = r.ui;
  lastBits = r.bits;
  paint($('#screen'), r.bits);
  highlightCurrentScreen();
  return r;
}

// Every sample state for the shown screen, side by side.
let galleryTimer = 0;
function refreshGallery() {
  clearTimeout(galleryTimer);
  galleryTimer = setTimeout(async () => {
    const wrap = $('#gallery');
    const items = [];
    const now = Date.now();
    for (const [name, title] of Object.entries(SCENARIOS)) {
      const slots = state.slots.map((_, s) => {
        const ref = slotRef(s);
        if (!ref) return null;
        const at = name === 'never' ? 0 : name === 'stale' ? now - 25 * 60e3 : now - 60e3;
        return { at, ...sampleDock(ref, s % 2, name, 0) };
      });
      const lastTry = now - 60e3;
      items.push([title, { ui: { ...state.ui, status: false }, ctx: deviceCtx(slots, state.ui.screen, lastTry, false), slots: slotPayload(slots) }]);
    }
    items.push(['Updating', { ui: { ...state.ui, status: false }, ctx: { ...deviceCtx(state.slots, state.ui.screen, 0, true) }, slots: slotPayload(state.slots) }]);
    items.push(['Status (Menu)', { ui: { ...state.ui, status: true }, ctx: {}, slots: [], status: statusLines() }]);
    const frames = await Promise.all(items.map(([, req]) => api('POST', '/api/frame', req)));
    wrap.textContent = '';
    frames.forEach((r, i) => {
      if (r.error) return;
      const fig = document.createElement('figure');
      const c = document.createElement('canvas');
      c.width = W; c.height = H;
      const cap = document.createElement('figcaption');
      cap.textContent = items[i][0];
      fig.append(c, cap);
      wrap.append(fig);
      paint(c, r.bits);
    });
  }, 250);
}

let redrawTimer = 0;
function redrawSoon() {
  clearTimeout(redrawTimer);
  redrawTimer = setTimeout(() => { enqueue(() => renderMain()); refreshGallery(); }, 120);
}

// ------------------------------------------------------------------ buttons
// Presses run one at a time, in order, like the device's button loop.
let queue = Promise.resolve();
function enqueue(fn) {
  queue = queue.then(fn).catch((e) => log(e.message || String(e), true));
  return queue;
}

let settleTimer = 0;
function press(key) {
  flash(key);
  clearTimeout(settleTimer);
  enqueue(() => handleKey(key));
}

async function handleKey(key) {
  const before = state.ui.screen;
  const r = await renderMain({ key });
  if (!r) return;
  const names = { up: 'dial up', down: 'dial down', ok: 'dial press', back: 'exit', menu: 'menu' };
  const what = r.ui.status ? 'status screen' : `screen ${r.ui.screen + 1}`;
  log(`${names[key]}: ${r.action === 'none' ? 'nothing to do' : r.action === 'fetch' ? 'fetch' : what}${before !== r.ui.screen ? ` (from ${before + 1})` : ''}`);
  refreshGallery();
  if (r.action === 'fetch') return fetchOnScreen();
  // As apply() in the firmware: a screen with old numbers fetches once the dial is still.
  if (!r.ui.status && screenNeedsFetch()) {
    settleTimer = setTimeout(() => enqueue(fetchOnScreen), SCROLL_SETTLE_MS);
  }
}

function screenNeedsFetch() {
  const s = state.ui.screen;
  const a = state.slots[s * 2]?.at || 0, b = state.slots[s * 2 + 1]?.at || 0;
  const t = a && b ? Math.min(a, b) : 0;
  return !t || Date.now() - t > SCREEN_STALE_MS;
}

// As refreshOnScreen() in the firmware.
async function fetchOnScreen() {
  state.ui.status = false;
  if (state.lastTry && Date.now() - state.lastTry < MIN_FETCH_GAP_MS) {
    log('fetch skipped: under 20 s since the last one (as on the device)');
    await renderMain();
    return;
  }
  state.busy = true;
  await renderMain();
  try {
    if (state.mode === 'live') await loadLive();
    else await loadSample();
  } finally {
    state.busy = false;
  }
  await renderMain();
  refreshGallery();
}

function flash(key) {
  const el = document.querySelector(`[data-key="${key}"]`);
  if (!el) return;
  el.classList.add('flash');
  setTimeout(() => el.classList.remove('flash'), 150);
}

function bindButtons() {
  document.querySelectorAll('[data-key]').forEach((b) => b.addEventListener('click', () => press(b.dataset.key)));
  let acc = 0;
  $('#dial').addEventListener('wheel', (e) => {
    e.preventDefault();
    acc += e.deltaY;
    if (Math.abs(acc) < 40) return;
    press(acc > 0 ? 'down' : 'up');
    acc = 0;
  }, { passive: false });
  document.addEventListener('keydown', (e) => {
    if (e.target.closest('input, select, textarea') || e.metaKey || e.ctrlKey || e.altKey) return;
    const key = { ArrowUp: 'up', ArrowDown: 'down', Enter: 'ok', ' ': 'ok', m: 'menu', M: 'menu', Escape: 'back', Backspace: 'back' }[e.key];
    if (!key) return;
    e.preventDefault();
    press(key);
  });
  $('#boot').addEventListener('click', () => enqueue(async () => {
    await renderMain({ message: ['dock display', 'connecting...'] });
    await sleep(1500);
    await renderMain();
  }));
}

// ------------------------------------------------------------------ editor
function renderEditor() {
  const wrap = $('#screens');
  wrap.textContent = '';
  const screens = state.config.screens;
  screens.forEach((screen, i) => {
    const card = document.createElement('div');
    card.className = 'screen-card';
    card.dataset.screen = i;
    const head = document.createElement('header');
    const title = document.createElement('strong');
    title.textContent = `Screen ${i + 1}`;
    const show = button('Show', `Show screen ${i + 1} on the device`, () => {
      clearTimeout(settleTimer);
      state.ui.screen = i;
      state.ui.status = false;
      redrawSoon();
    });
    const up = button('↑', 'Move up', () => moveScreen(i, -1), i === 0);
    const down = button('↓', 'Move down', () => moveScreen(i, 1), i === screens.length - 1);
    const del = button('✕', 'Remove', () => { screens.splice(i, 1); setConfig(state.config); }, screens.length <= 1);
    head.append(title, show, up, down, del);
    card.append(head);
    for (const role of ROLES) card.append(dockRow(screen, role, i));
    wrap.append(card);
  });
  $('#add-screen').disabled = screens.length >= MAX_SCREENS;
  updateSaveState();
  highlightCurrentScreen();
}

function button(text, title, onClick, disabled = false) {
  const b = document.createElement('button');
  b.type = 'button';
  b.textContent = text;
  b.title = title;
  b.setAttribute('aria-label', title);
  b.disabled = disabled;
  b.addEventListener('click', onClick);
  return b;
}

function moveScreen(i, delta) {
  const s = state.config.screens;
  [s[i], s[i + delta]] = [s[i + delta], s[i]];
  state.slots.fill(null);
  setConfig(state.config);
}

function dockText(d) {
  if (!d) return '';
  if (d.id > 0) return `${d.name || state.dockById.get(d.id)?.name || 'Dock'} (${d.id})`;
  return d.query || '';
}

function parseDockInput(text) {
  text = text.trim();
  const m = text.match(/\((?:BikePoints_)?(\d+)\)\s*$/) || text.match(/^(?:BikePoints_)?(\d+)$/);
  if (m) {
    const id = Number(m[1]);
    const known = state.dockById.get(id);
    const typed = text.replace(/\s*\((?:BikePoints_)?\d+\)\s*$/, '').replace(/^(?:BikePoints_)?\d+$/, '');
    return { id, name: known ? known.name : ascii(typed) }; // name '' = unknown until TfL's list loads
  }
  const exact = state.docks.find((d) => d.name.toLowerCase() === text.toLowerCase());
  if (exact) return { id: exact.id, name: exact.name };
  return { id: 0, query: ascii(text), name: ascii(text) }; // the device looks it up by search
}

function dockRow(screen, role, index) {
  const d = screen[role] || (screen[role] = { id: 0, query: '', name: '', label: '' });
  const row = document.createElement('div');
  row.className = 'dock-row';
  const roleLabel = document.createElement('span');
  roleLabel.className = 'role';
  roleLabel.textContent = ROLE_NAMES[role];

  const pick = document.createElement('input');
  pick.setAttribute('list', 'docklist');
  pick.placeholder = 'Search docks, or type an ID';
  pick.value = dockText(d);
  pick.setAttribute('aria-label', `Screen ${index + 1} ${ROLE_NAMES[role]}`);

  const label = document.createElement('input');
  label.maxLength = LABEL_LEN;
  label.value = d.label || '';
  label.placeholder = 'LABEL';
  label.setAttribute('aria-label', `Screen ${index + 1} ${role} label`);

  const resolved = document.createElement('span');
  resolved.className = 'resolved';
  const describe = () => {
    if (d.id > 0) resolved.textContent = `BikePoints_${d.id}${state.docks.length && !state.dockById.has(d.id) ? ' (not in TfL\'s current list)' : ''}`;
    else resolved.textContent = d.query ? `No ID: the device will search TfL for "${d.query}" and keep the first match` : 'Pick a dock';
  };
  describe();

  pick.addEventListener('change', () => {
    const next = parseDockInput(pick.value);
    const autoLabel = !d.label || d.label === shortLabel(d.name);
    d.id = next.id;
    d.name = next.name;
    if (next.id > 0) delete d.query; else d.query = next.query;
    if (autoLabel && next.name) { d.label = shortLabel(next.name); label.value = d.label; }
    pick.value = dockText(d);
    describe();
    syncSlots();
    updateSaveState();
    redrawSoon();
  });
  label.addEventListener('input', () => {
    const clean = ascii(label.value).slice(0, LABEL_LEN);
    if (clean !== label.value) label.value = clean;
    d.label = clean;
    updateSaveState();
    redrawSoon();
  });
  row.append(roleLabel, pick, label, resolved);
  return row;
}

function highlightCurrentScreen() {
  document.querySelectorAll('.screen-card').forEach((c) => c.classList.toggle('current', Number(c.dataset.screen) === state.ui.screen));
}

function updateSaveState(message) {
  const el = $('#save-state');
  if (message) el.textContent = message;
  else el.textContent = isDirty() ? 'Unsaved changes.' : 'Matches docks.json on disk.';
}

function showErrors(errors) {
  const box = $('#config-errors');
  box.textContent = '';
  for (const e of errors || []) {
    const li = document.createElement('li');
    li.textContent = e;
    box.append(li);
  }
  box.hidden = !errors || errors.length === 0;
}

async function loadConfig() {
  const r = await api('GET', '/api/config');
  if (r.error) { showErrors([`Couldn't read docks.json: ${r.error}`]); return; }
  showErrors([]);
  setConfig(r.config, true);
}

function bindEditor() {
  $('#add-screen').addEventListener('click', () => {
    if (state.config.screens.length >= MAX_SCREENS) return;
    const last = state.config.screens.at(-1);
    // A new screen starts as the reverse of the last one: the way home.
    const copy = (d) => ({ ...(d || { id: 0, query: '', name: '', label: '' }) });
    state.config.screens.push(last ? { home: copy(last.dest), dest: copy(last.home) } : { home: copy(), dest: copy() });
    setConfig(state.config);
  });
  $('#save').addEventListener('click', async () => {
    const cfg = cleanConfig();
    const r = await api('PUT', '/api/config', cfg);
    if (r.errors || r.error) { showErrors(r.errors || [r.error]); updateSaveState('Not saved.'); return; }
    showErrors([]);
    state.savedJson = JSON.stringify(cfg);
    $('#header-preview').textContent = r.header;
    updateSaveState(`Saved ${r.paths.join(' and ')} at ${hm(Date.now())}. Flash the board to use it.`);
    log(`saved docks.json (${cfg.screens.length} screens)`);
  });
  $('#download').addEventListener('click', () => {
    const blob = new Blob([JSON.stringify(cleanConfig(), null, 2) + '\n'], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'docks.json';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  });
  $('#import').addEventListener('change', async (e) => {
    const file = e.target.files[0];
    e.target.value = '';
    if (!file) return;
    try {
      const cfg = JSON.parse(await file.text());
      if (!cfg || !Array.isArray(cfg.screens) || cfg.screens.length === 0) throw new Error('expected {"screens": [...]}');
      cfg.screens = cfg.screens.slice(0, MAX_SCREENS);
      showErrors([]);
      state.slots.fill(null);
      setConfig(cfg);
      updateSaveState(`Imported ${file.name}. Not saved yet.`);
    } catch (err) {
      showErrors([`Couldn't import ${file.name}: ${err.message}`]);
    }
  });
  $('#revert').addEventListener('click', () => { state.slots.fill(null); loadConfig(); });
  window.addEventListener('beforeunload', (e) => { if (isDirty()) e.preventDefault(); });
}

// ------------------------------------------------------------------ setup
function bindControls() {
  const sel = $('#scenario');
  for (const [value, text] of Object.entries(SCENARIOS)) {
    const opt = document.createElement('option');
    opt.value = value;
    opt.textContent = text;
    sel.append(opt);
  }
  sel.value = SCENARIOS[state.scenario] ? state.scenario : 'typical';
  sel.addEventListener('change', () => { setScenario(sel.value); redrawSoon(); });

  const zoom = $('#zoom');
  zoom.value = String(store.get('zoom', 3));
  const applyZoom = () => { $('#screen').style.width = `${W * Number(zoom.value)}px`; store.set('zoom', Number(zoom.value)); };
  zoom.addEventListener('change', applyZoom);
  applyZoom();

  const tint = $('#tint');
  tint.checked = store.get('tint', true);
  tint.addEventListener('change', () => { store.set('tint', tint.checked); redrawSoon(); });

  document.querySelectorAll('[data-mode]').forEach((b) => b.addEventListener('click', () => setMode(b.dataset.mode)));
}

function setMode(mode) {
  state.mode = mode;
  store.set('mode', mode);
  document.querySelectorAll('[data-mode]').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.mode === mode)));
  $('#scenario').disabled = mode === 'live';
  clearTimeout(settleTimer);
  if (mode === 'sample') { setScenario(state.scenario); redrawSoon(); return; }
  state.slots.fill(null);
  state.lastTry = 0;
  syncSlots(); // starts a live load
  log('live: fetching from TfL');
}

// Redraw when render.cpp (or another renderer source) changes on disk.
async function watchSources() {
  let last = null;
  for (;;) {
    try {
      const r = await api('GET', '/api/sources');
      if (last !== null && r.mtime !== last) { log('render code changed: redrawn'); redrawSoon(); }
      last = r.mtime;
    } catch { /* server restarting */ }
    await sleep(1500);
  }
}

async function main() {
  bindControls();
  bindButtons();
  bindEditor();
  await loadConfig();
  setMode(state.mode);
  loadDockList()
    .then((n) => { $('#docklist-note').textContent = `${n} docks from TfL. Type to search, or type an ID.`; renderEditor(); })
    .catch((e) => { $('#docklist-note').textContent = `Couldn't load TfL's dock list (${e.message}). Type a dock ID, or a name for the device to search for.`; });
  watchSources();
}

main();
