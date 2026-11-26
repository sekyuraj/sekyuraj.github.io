// The Rake [BETA]: the page is a camera flight filmed in Roblox Studio, one frame per scroll position.
// Frames 0-132 are the flight (stops at 0 sign, 36 gnome, 96 cabin, 132 the Rake), 133-153 the gnome waving,
// 154-169 the Rake's face; end/ holds the 52 frames of the One Week closing card.
'use strict';
document.documentElement.classList.add('js');

const ROBLOX_URL = ''; // set once the game is published
const PATH_LAST = 132, GNOME_LOOP = [133, 21], RAKE_LOOP = [154, 16], CARD_FRAMES = 52;
// scroll progress ranges: hold on a stop, fly between them, cut to black, the end card
const PLAN = [
  { to: 0.07, hold: 0 },
  { to: 0.13, fly: [0, 36] },
  { to: 0.25, hold: 1 },
  { to: 0.47, fly: [36, 96] },
  { to: 0.58, hold: 2 },
  { to: 0.70, fly: [96, 132] },
  { to: 0.82, hold: 3 },
  { to: 0.88, cut: true },
  { to: 1.01, end: true },
];
const STOP_FRAME = [0, 36, 96, PATH_LAST];
const still = matchMedia('(prefers-reduced-motion: reduce)');

const canvas = document.querySelector('.stage__film');
const ctx = canvas.getContext('2d');
const stops = [...document.querySelectorAll('.stop')];
const credits = document.querySelector('.credits');
const soundBtn = document.querySelector('.bar__sound');

for (const a of document.querySelectorAll('[data-roblox]')) {
  if (ROBLOX_URL) { a.href = ROBLOX_URL; a.rel = 'noopener'; }
  else { a.textContent = 'Coming to Roblox soon'; a.setAttribute('aria-disabled', 'true'); a.removeAttribute('href'); }
}

// ---------- frames ----------
const dir = innerWidth * devicePixelRatio > 1100 ? 'f' : 'm';
const frames = new Array(170);
const card = new Array(CARD_FRAMES);
function load(list, i, src) {
  const im = new Image();
  im.decoding = 'async';
  im.src = src;
  im.onload = () => { list[i] = im; dirty = true; };
}
const pad3 = (n) => String(n).padStart(3, '0');
// the stops first, then the flight outward from them, then the loops and the card
const order = [...STOP_FRAME];
for (let i = 0; i <= PATH_LAST; i++) if (!order.includes(i)) order.push(i);
for (let i = PATH_LAST + 1; i < 170; i++) order.push(i);
order.forEach((i, k) => setTimeout(() => load(frames, i, `${dir}/${pad3(i)}.webp`), k < 4 ? 0 : 20 + k * 6));
for (let i = 0; i < CARD_FRAMES; i++) setTimeout(() => load(card, i, `end/end${String(i + 1).padStart(2, '0')}.jpg`), 1600 + i * 15);

// nearest loaded frame, so a slow connection shows something instead of black
function frame(i) {
  if (frames[i]) return frames[i];
  for (let d = 1; d < 40; d++) { if (frames[i - d]) return frames[i - d]; if (frames[i + d]) return frames[i + d]; }
  return null;
}

// ---------- drawing ----------
let W = 0, H = 0;
function resize() {
  const r = canvas.getBoundingClientRect();
  W = Math.round(r.width * devicePixelRatio); H = Math.round(r.height * devicePixelRatio);
  canvas.width = W; canvas.height = H; dirty = true;
}
function cover(im, alpha = 1, fit = false) {
  // cover, but on a tall phone screen never zoom past 1.35x the width: a band with black above and below keeps the shot whole
  const s = fit ? Math.min(W / im.naturalWidth, H / im.naturalHeight)
    : Math.min(Math.max(W / im.naturalWidth, H / im.naturalHeight), 1.35 * W / im.naturalWidth);
  const w = im.naturalWidth * s, h = im.naturalHeight * s;
  ctx.globalAlpha = alpha;
  ctx.drawImage(im, (W - w) / 2, (H - h) / 2, w, h);
  ctx.globalAlpha = 1;
}
function black(a) { ctx.globalAlpha = a; ctx.fillStyle = '#000'; ctx.fillRect(0, 0, W, H); ctx.globalAlpha = 1; }
// a fractional frame: the two neighbours blended, so the scrub is smooth between filmed frames
function film(f) {
  const i = Math.floor(f), a = f - i;
  const A = frame(i); if (!A) return black(1);
  cover(A);
  if (a > 0.02 && i < PATH_LAST) { const B = frame(i + 1); if (B && B !== A) cover(B, a); }
}

// ---------- the scroll ----------
let target = 0, cur = 0, dirty = true, holdT = 0, endT = 0, last = performance.now(), shown = -1;
function progress() {
  const max = document.documentElement.scrollHeight - innerHeight;
  return max > 0 ? Math.min(1, Math.max(0, scrollY / max)) : 0;
}
function where(p) {
  let from = 0;
  for (const seg of PLAN) { if (p < seg.to) return { seg, q: (p - from) / (seg.to - from) }; from = seg.to; }
  return { seg: PLAN[PLAN.length - 1], q: 1 };
}
function caption(n) {
  if (n === shown) return;
  shown = n;
  stops.forEach((s, i) => s.classList.toggle('is-on', i === n));
  credits.classList.toggle('is-on', n === 4);
}

function tick(now) {
  const dt = Math.min(0.1, (now - last) / 1000); last = now;
  target = progress();
  const before = cur;
  cur = still.matches ? target : cur + (target - cur) * (1 - Math.exp(-dt * 7));
  if (Math.abs(target - cur) < 0.0004) cur = target;
  const moving = cur !== before;
  const { seg, q } = where(cur);
  const settled = !moving && !still.matches;

  if (seg.hold !== undefined) {
    caption(seg.hold);
    // the gnome waves and the Rake breathes while you stay; the flight frame comes back the moment you scroll on
    holdT = settled ? holdT + dt : 0;
    const loop = seg.hold === 1 ? GNOME_LOOP : seg.hold === 3 ? RAKE_LOOP : null;
    if (loop && holdT > 0.15) { const k = Math.floor((holdT - 0.15) * (seg.hold === 1 ? 15 : 12)) % loop[1]; film(loop[0] + k); }
    else if (moving || dirty || holdT === 0) film(STOP_FRAME[seg.hold]);
    endT = 0; reel(false);
  } else if (seg.fly) {
    caption(-1);
    if (moving || dirty) film(seg.fly[0] + (seg.fly[1] - seg.fly[0]) * q);
    holdT = 0; endT = 0; reel(false);
  } else if (seg.cut) {
    // the game's own cut: a few hard flickers, then black
    caption(-1);
    film(PATH_LAST);
    const flick = still.matches ? q : (q < 0.55 ? (Math.floor(q * 18) % 2 ? 1 : 0.45) : 1);
    black(flick);
    endT = 0; reel(false);
  } else {
    caption(4);
    black(1);
    if (!still.matches) endT += dt;
    const im = card[cardIndex(endT)] || card.find(Boolean);
    if (im) cover(im, Math.min(1, q * 4), true);
    reel(true);
  }
  dirty = false;
  requestAnimationFrame(tick);
}
// the card as in the game: the steady frames run back and forth for 12 s, then the film's own fade, then again
function cardIndex(t) {
  if (still.matches) return 9;
  const n = Math.floor(t * 14) % (168 + 32 + 21);
  if (n < 168) { const m = n % 38; return m < 20 ? m : 38 - m; }
  if (n < 200) return 20 + (n - 168);
  return CARD_FRAMES - 1;
}

// ---------- sound: the end record, everywhere; it may only start after a click or a key ----------
let audio = null, song = null, reelSrc = null, reelBuf = null, gain = null, on = false;
async function startSound() {
  if (audio) return;
  audio = new AudioContext();
  gain = audio.createGain(); gain.gain.value = 0; gain.connect(audio.destination);
  const get = async (u) => audio.decodeAudioData(await (await fetch(u)).arrayBuffer());
  try {
    const buf = await get('song.mp3');
    song = audio.createBufferSource(); song.buffer = buf; song.loop = true; song.connect(gain); song.start();
    reelBuf = await get('reel.mp3');
  } catch (e) {
    soundBtn.hidden = true; // no sound on this browser: the page works without it
    console.warn('sound unavailable:', e);
    return;
  }
  setSound(true);
}
function setSound(v) {
  on = v;
  if (audio) { if (v) audio.resume(); gain.gain.setTargetAtTime(v ? 0.8 : 0, audio.currentTime, 0.4); }
  soundBtn.setAttribute('aria-pressed', String(v));
  soundBtn.textContent = v ? 'Sound on' : 'Sound off';
}
function reel(want) {
  if (!audio || !reelBuf) return;
  if (want && !reelSrc) {
    reelSrc = audio.createBufferSource(); reelSrc.buffer = reelBuf; reelSrc.loop = true;
    const g = audio.createGain(); g.gain.value = 0.35; reelSrc.connect(g).connect(gain); reelSrc.start();
  } else if (!want && reelSrc) { reelSrc.stop(); reelSrc = null; }
}
soundBtn.hidden = false;
soundBtn.addEventListener('click', (e) => { e.stopPropagation(); if (!audio) startSound(); else setSound(!on); });
const firstGesture = () => { startSound(); removeEventListener('pointerdown', firstGesture); removeEventListener('keydown', firstGesture); };
addEventListener('pointerdown', firstGesture);
addEventListener('keydown', firstGesture);

addEventListener('resize', resize);
resize();
requestAnimationFrame(tick);
