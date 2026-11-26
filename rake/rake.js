// The Rake [BETA]: a film filmed in the game, in stops. One scroll (wheel, swipe, arrow keys) moves one stop;
// the flight between stops plays as video at its own speed, backwards when you scroll up.
// stops: 0 the sign, 1 the gnome (waving), 2 the cabin, 3 the Rake (staring), 4 the ending (loops forever)
'use strict';
const ROBLOX_URL = ''; // set once the game is published on Roblox

// the film is 2.39:1: on a wide screen it fills the height (cover), on a tall phone it is letterboxed to the width
const needW = (innerWidth > innerHeight ? Math.max(innerWidth, innerHeight * 2.39) : innerWidth) * devicePixelRatio;
const small = needW < 1100;
const dir = small ? 'v/960/' : 'v/1920/';
const stage = document.querySelector('.stage');
const poster = document.querySelector('.poster');
const ctas = [...document.querySelectorAll('.cta')];
const soundBtn = document.querySelector('.sound');

for (const a of document.querySelectorAll('[data-roblox]')) {
  if (ROBLOX_URL) { a.href = ROBLOX_URL; a.rel = 'noopener'; }
  else { a.textContent = 'Coming to Roblox soon'; a.setAttribute('aria-disabled', 'true'); a.removeAttribute('href'); }
}

function video(name) {
  const v = document.createElement('video');
  v.muted = true; v.playsInline = true; v.preload = 'auto'; v.src = dir + name + '.mp4';
  stage.append(v);
  return v;
}
const fwd = [video('a'), video('b'), video('c')];
const rev = [video('a_r'), video('b_r'), video('c_r')];
const loop = { 1: video('gnome'), 3: video('rake'), 4: video('end') };
const black = document.createElement('div'); black.className = 'black'; stage.append(black);

let shown = null;
// put v on screen only once it has a frame to show, then take the old one away: no black flash in between
function swap(v) {
  return new Promise((done) => {
    const go = () => { v.classList.add('on'); if (shown && shown !== v) shown.classList.remove('on'); if (shown !== v) poster.style.display = 'none'; shown = v; done(); };
    let fired = false; const once = () => { if (!fired) { fired = true; go(); } };
    if (v.requestVideoFrameCallback) v.requestVideoFrameCallback(once); else v.addEventListener('timeupdate', once, { once: true });
    setTimeout(once, 400); // never wait longer than this for a frame
  });
}
function ended(v) { return new Promise((r) => v.addEventListener('ended', r, { once: true })); }
async function playFrom0(v, looped) {
  v.loop = looped; v.currentTime = 0;
  const p = swap(v); await v.play().catch(() => {}); await p;
}
async function still(v) {
  v.pause(); v.loop = false;
  if (v.currentTime !== 0 || v.readyState < 2) {
    const seeked = new Promise((r) => { v.addEventListener('seeked', r, { once: true }); v.addEventListener('loadeddata', r, { once: true }); });
    v.currentTime = 0;
    await Promise.race([seeked, sleep(1500)]);
  }
  if (v.requestVideoFrameCallback) { v.play().then(() => v.pause()).catch(() => {}); } // makes the paused frame paint
  await swap(v);
}
// what a stop looks like while you stay there
async function settle(s) {
  if (loop[s]) return playFrom0(loop[s], true);
  return still(fwd[s]); // stop 0 is the first frame of a, stop 2 the first frame of c
}

let at = 0, busy = false, want = 0;
async function step(d) {
  const to = at + d;
  if (to < 0 || to > 4) return;
  busy = true; cta(-1);
  if (d > 0 && to === 4) {
    // the game's own cut: a few hard flickers, then black, then the ending
    for (const o of [1, 0.45, 1, 0.45, 1]) { black.style.opacity = o; await sleep(60); }
    for (const v of Object.values(loop)) if (v !== loop[4]) v.pause();
    await playFrom0(loop[4], true);
    black.style.opacity = 0;
  } else if (d < 0 && at === 4) {
    black.style.opacity = 1; loop[4].pause();
    await settle(3);
    black.style.transition = 'opacity .5s'; black.style.opacity = 0; await sleep(500); black.style.transition = '';
  } else {
    const clip = d > 0 ? fwd[at] : rev[to];
    if (shown && shown !== clip) shown.pause();
    await playFrom0(clip, false);
    await ended(clip);
    await settle(to);
  }
  at = to; busy = false; cta(at);
  // keep going if the visitor kept scrolling while it flew
  if (want !== 0) { const n = Math.sign(want); want = 0; step(n); }
}
function go(d) { if (busy) { want = d; return; } step(d); }
function cta(s) { ctas.forEach((c) => c.classList.toggle('on', +c.dataset.at === s)); }
function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

// ---------- input: one gesture = one stop ----------
let acc = 0, quietUntil = 0;
addEventListener('wheel', (e) => {
  e.preventDefault();
  const now = performance.now();
  if (now < quietUntil) { quietUntil = now + 180; return; } // the tail of one flick is not a second scroll
  acc += e.deltaY;
  if (Math.abs(acc) > 30) { go(Math.sign(acc)); acc = 0; quietUntil = now + 180; }
}, { passive: false });
let touchY = null;
addEventListener('touchstart', (e) => { touchY = e.touches[0].clientY; }, { passive: true });
addEventListener('touchend', (e) => {
  if (touchY === null) return;
  const dy = touchY - e.changedTouches[0].clientY; touchY = null;
  if (Math.abs(dy) > 40) go(Math.sign(dy));
}, { passive: true });
addEventListener('keydown', (e) => {
  if (['ArrowDown', 'PageDown', ' '].includes(e.key)) { e.preventDefault(); go(1); }
  if (['ArrowUp', 'PageUp'].includes(e.key)) { e.preventDefault(); go(-1); }
});

// ---------- sound: the slowed record from the game's ending, all the time (browsers allow it after a click or key) ----------
let audio = null, gain = null, on = false;
async function startSound() {
  if (audio) return;
  audio = new AudioContext();
  gain = audio.createGain(); gain.gain.value = 0; gain.connect(audio.destination);
  try {
    const buf = await audio.decodeAudioData(await (await fetch('song.mp3')).arrayBuffer());
    const src = audio.createBufferSource(); src.buffer = buf; src.loop = true; src.connect(gain); src.start();
  } catch (err) { soundBtn.hidden = true; console.warn('no sound:', err); return; }
  setSound(true);
}
function setSound(v) {
  on = v;
  if (audio) { if (v) audio.resume(); gain.gain.setTargetAtTime(v ? 0.8 : 0, audio.currentTime, 0.3); }
  soundBtn.setAttribute('aria-pressed', String(v)); soundBtn.textContent = v ? 'Sound on' : 'Sound off';
}
soundBtn.hidden = false;
soundBtn.addEventListener('click', (e) => { e.stopPropagation(); if (!audio) startSound(); else setSound(!on); });
const first = () => { startSound(); removeEventListener('pointerdown', first); removeEventListener('keydown', first); };
addEventListener('pointerdown', first);
addEventListener('keydown', first);

settle(0).then(() => cta(0));
