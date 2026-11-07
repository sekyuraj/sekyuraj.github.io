// The cape picker: a chip swaps the texture with one quick turn; dragging turns the cape by hand.
const stage = document.querySelector('[data-cape-stage]');
const body = stage?.querySelector('[data-cape-body]');
const img = stage?.querySelector('[data-cape-img]');
const chips = [...document.querySelectorAll('[data-cape]')];

if (stage && body && img) {
  const still = matchMedia('(prefers-reduced-motion: reduce)');
  const rest = -28;
  let angle = rest, anim = 0, dragging = null;

  const set = a => { angle = a; body.style.setProperty('--ry', a + 'deg'); };
  const back = src => body.style.setProperty('--back', `url("${src}")`);
  back(img.src);

  // ease-out: fast at first, settles gently; any new turn starts from where this one is
  const turn = (to, ms, swapAt, swap) => {
    cancelAnimationFrame(anim);
    const from = angle, t0 = performance.now();
    let swapped = false;
    const step = now => {
      const t = Math.min(1, (now - t0) / ms);
      const e = 1 - Math.pow(1 - t, 3);
      set(from + (to - from) * e);
      if (!swapped && swap && t >= swapAt) { swapped = true; swap(); }
      if (t < 1) anim = requestAnimationFrame(step);
    };
    anim = requestAnimationFrame(step);
  };

  const pick = chip => {
    chips.forEach(c => c.setAttribute('aria-checked', String(c === chip)));
    const src = `img/capes/${chip.dataset.cape}.png`;
    const swap = () => {
      img.src = src;
      back(src);
      body.style.setProperty('--edge', chip.dataset.edge);
      body.setAttribute('aria-label', `The ${chip.textContent} cape`);
    };
    if (still.matches) { swap(); return; }
    // one full turn, ending at the resting angle; the texture changes while the cape faces away
    const base = rest + 360 * Math.ceil((angle - rest) / 360 + 0.01);
    turn(base, 700, 0.3, swap);
  };

  chips.forEach((chip, i) => {
    chip.addEventListener('click', () => pick(chip));
    chip.addEventListener('keydown', e => {
      const d = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[e.key];
      if (!d) return;
      e.preventDefault();
      const next = chips[(i + d + chips.length) % chips.length];
      next.focus();
      pick(next);
    });
    chip.tabIndex = chip.getAttribute('aria-checked') === 'true' ? 0 : -1;
  });
  // keep the radio group to one tab stop that follows the picked chip
  new MutationObserver(() => chips.forEach(c => { c.tabIndex = c.getAttribute('aria-checked') === 'true' ? 0 : -1; }))
    .observe(chips[0].parentElement, { subtree: true, attributes: true, attributeFilter: ['aria-checked'] });

  stage.addEventListener('pointerdown', e => {
    cancelAnimationFrame(anim);
    dragging = { x: e.clientX, a: angle, id: e.pointerId };
    stage.setPointerCapture(e.pointerId);
  });
  stage.addEventListener('pointermove', e => {
    if (!dragging || e.pointerId !== dragging.id) return;
    set(dragging.a + (e.clientX - dragging.x) * 0.6);
  });
  const release = e => {
    if (!dragging || e.pointerId !== dragging.id) return;
    dragging = null;
    if (!still.matches) {
      // settle on the nearest resting angle, front or back
      const to = rest + Math.round((angle - rest) / 180) * 180;
      turn(to, 450);
    }
  };
  stage.addEventListener('pointerup', release);
  stage.addEventListener('pointercancel', release);
}
