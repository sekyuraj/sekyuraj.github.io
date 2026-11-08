"use strict";
(() => {
  const BASE = "/chan";
  const board = document.body.dataset.board;
  const thread = Number(document.body.dataset.thread) || 0;

  const store = {
    get(k, d) { try { return JSON.parse(localStorage.getItem(k)) ?? d; } catch { return d; } },
    set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} },
  };

  // delete password: one random secret per browser, sent with every post
  let pass = store.get("tuxchan-pass", "");
  if (!pass) {
    pass = Array.from(crypto.getRandomValues(new Uint8Array(12)), (b) => b.toString(16).padStart(2, "0")).join("");
    store.set("tuxchan-pass", pass);
  }
  document.querySelectorAll(".delpass").forEach((i) => (i.value = pass));

  // remember our own posts
  const mine = new Set(store.get("tuxchan-mine", []));
  const posted = new URLSearchParams(location.search).get("posted");
  if (posted && board) {
    mine.add(board + "/" + posted);
    store.set("tuxchan-mine", [...mine].slice(-500));
    history.replaceState(null, "", location.pathname + location.hash);
  }

  function decorate(root) {
    root.querySelectorAll("time").forEach((t) => {
      const d = new Date(t.getAttribute("datetime"));
      if (!isNaN(d)) {
        t.textContent = d.toLocaleString([], { year: "2-digit", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit" });
        t.title = d.toString();
      }
    });
    if (!board) return;
    root.querySelectorAll(".post").forEach((p) => {
      if (mine.has(board + "/" + p.dataset.no)) p.classList.add("you");
    });
    root.querySelectorAll("a.ql").forEach((a) => {
      if (mine.has(a.dataset.b + "/" + a.dataset.no)) a.classList.add("you");
    });
  }
  decorate(document);

  // captcha: fetched when the form is first used
  function loadCaptcha(form) {
    const img = form.querySelector(".capimg");
    if (!img) return;
    fetch(BASE + "/captcha").then((r) => r.json()).then(({ id }) => {
      form.querySelector(".capid").value = id;
      img.onload = () => (img.hidden = false);
      img.src = `${BASE}/captcha/${id}.png`;
      form.querySelector(".capin").value = "";
    });
  }
  document.querySelectorAll("#postform form").forEach((form) => {
    let loaded = false;
    const first = () => { if (!loaded) { loaded = true; loadCaptcha(form); } };
    form.addEventListener("focusin", first);
    if (form.closest("details").open && form.querySelector(".capimg")) {
      new IntersectionObserver((e, o) => { if (e[0].isIntersecting) { first(); o.disconnect(); } }).observe(form);
    }
    form.querySelector(".capnew")?.addEventListener("click", () => loadCaptcha(form));
    form.addEventListener("submit", (e) => {
      const file = form.querySelector("input[type=file]").files[0];
      const max = 8 * 1024 * 1024;
      if (file && file.size > max) {
        e.preventDefault();
        alert("That file is bigger than 8 MB.");
        return;
      }
      form.querySelector("button[type=submit]").disabled = true;
    });
  });

  // clicking a post number quotes it in the form
  document.addEventListener("click", (e) => {
    const q = e.target.closest("a.qno");
    if (!q) return;
    const form = document.querySelector("#postform");
    if (!form) return;
    e.preventDefault();
    if (thread === 0) {
      const u = new URL(q.href);
      u.hash = "#p" + q.dataset.no;
      u.search = "?quote=" + q.dataset.no;
      location.href = u;
      return;
    }
    form.open = true;
    const ta = form.querySelector("textarea");
    const add = ">>" + q.dataset.no + "\n";
    const at = ta.selectionStart ?? ta.value.length;
    ta.value = ta.value.slice(0, at) + add + ta.value.slice(at);
    ta.focus();
    ta.setSelectionRange(at + add.length, at + add.length);
  });
  const quote = new URLSearchParams(location.search).get("quote");
  if (quote && thread) {
    const f = document.querySelector("#postform");
    if (f) { f.open = true; f.querySelector("textarea").value = ">>" + quote + "\n"; }
  }

  // hover previews for quote links
  let tip = null;
  const cache = new Map();
  function showTip(a, html) {
    hideTip();
    tip = document.createElement("div");
    tip.className = "preview";
    tip.innerHTML = html;
    decorate(tip);
    document.body.append(tip);
    const r = a.getBoundingClientRect();
    const w = tip.offsetWidth, h = tip.offsetHeight;
    let x = r.right + 10, y = r.top - 10;
    if (x + w > innerWidth - 8) x = Math.max(8, r.left - w - 10);
    if (y + h > innerHeight - 8) y = Math.max(8, innerHeight - h - 8);
    tip.style.left = x + "px";
    tip.style.top = y + "px";
  }
  function hideTip() { tip?.remove(); tip = null; }
  document.addEventListener("mouseover", async (e) => {
    const a = e.target.closest("a.ql");
    if (!a) return;
    const id = a.dataset.b + "/" + a.dataset.no;
    const here = a.dataset.b === board && document.getElementById("p" + a.dataset.no);
    if (here) {
      const r = here.getBoundingClientRect();
      if (r.top >= 0 && r.bottom <= innerHeight) { here.classList.add("hl"); a.addEventListener("mouseout", () => here.classList.remove("hl"), { once: true }); return; }
      showTip(a, here.outerHTML);
    } else {
      if (!cache.has(id)) {
        const r = await fetch(`${BASE}/${a.dataset.b}/preview/${a.dataset.no}`);
        cache.set(id, r.ok ? await r.text() : `<p>That post is gone.</p>`);
      }
      if (a.matches(":hover")) showTip(a, cache.get(id));
    }
    a.addEventListener("mouseout", hideTip, { once: true });
  });

  // click a thumbnail to open it in place
  document.addEventListener("click", (e) => {
    const a = e.target.closest("a.thumb");
    if (!a || e.ctrlKey || e.metaKey || e.shiftKey || e.button !== 0) return;
    e.preventDefault();
    const box = a.closest(".file");
    if (a.dataset.kind === "video") {
      const v = document.createElement("video");
      v.src = a.dataset.full;
      v.controls = v.autoplay = v.loop = true;
      const close = document.createElement("a");
      close.href = "#";
      close.className = "closevid";
      close.textContent = "[close]";
      close.addEventListener("click", (ev) => { ev.preventDefault(); v.remove(); close.remove(); a.hidden = false; box.classList.remove("big"); });
      a.hidden = true;
      box.classList.add("big");
      box.append(close, v);
      return;
    }
    let img = a.querySelector("img");
    if (!img) {
      img = document.createElement("img");
      a.replaceChildren(img);
    }
    if (!a.classList.contains("open")) {
      a.dataset.thumb = img.getAttribute("src") || "";
      a.dataset.tw = img.getAttribute("width") || "";
      a.dataset.th = img.getAttribute("height") || "";
      img.removeAttribute("width");
      img.removeAttribute("height");
      img.src = a.dataset.full;
      a.classList.add("open");
      box.classList.add("big");
    } else if (a.dataset.thumb) {
      img.src = a.dataset.thumb;
      img.setAttribute("width", a.dataset.tw);
      img.setAttribute("height", a.dataset.th);
      a.classList.remove("open");
      box.classList.remove("big");
    }
  });

  async function postForm(url, data) {
    const r = await fetch(url, { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: new URLSearchParams(data) });
    return [r.ok, await r.text()];
  }

  document.addEventListener("click", async (e) => {
    const b = e.target.closest(".acts button");
    if (!b) return;
    const p = b.closest(".post");
    const no = p.dataset.no;
    if (b.classList.contains("report")) {
      const reason = prompt("Why should a moderator look at No." + no + "?");
      if (reason === null) return;
      const [, text] = await postForm(`${BASE}/${board}/report`, { no, reason });
      alert(text);
    } else {
      if (!confirm("Delete No." + no + "? (only works on your own posts)")) return;
      const [ok, text] = await postForm(`${BASE}/${board}/delete`, { no, password: pass });
      if (ok) {
        if (Number(no) === thread) location.href = `${BASE}/${board}/`;
        else p.remove();
      } else alert(text);
    }
  });

  // live updates on thread pages
  if (thread) {
    const status = document.querySelector(".tools .status");
    const list = document.querySelector(".thread");
    let last = Number(document.body.dataset.last) || 0;
    let delay = 15;
    let left = delay;
    async function update() {
      status.textContent = "updating...";
      try {
        const r = await fetch(`${BASE}/${board}/thread/${thread}.json?after=${last}`);
        if (r.status === 404) { status.textContent = "this thread was deleted"; clearInterval(timer); return; }
        const j = await r.json();
        for (const p of j.posts) {
          const box = document.createElement("div");
          box.innerHTML = p.html;
          const el = box.firstElementChild;
          list.append(el);
          decorate(el);
          el.querySelectorAll(".com a.ql").forEach((a) => {
            const target = a.dataset.b === board && document.getElementById("p" + a.dataset.no);
            if (!target || target === el) return;
            let bl = target.querySelector(":scope > .backlinks");
            if (!bl) {
              bl = document.createElement("div");
              bl.className = "backlinks";
              bl.textContent = "replies: ";
              target.querySelector(":scope > .com").after(bl);
            }
            const link = document.createElement("a");
            link.className = "ql";
            link.href = "#p" + p.no;
            link.dataset.b = board;
            link.dataset.no = p.no;
            link.textContent = ">>" + p.no;
            bl.append(link, " ");
          });
          last = p.no;
        }
        delay = j.posts.length ? 15 : Math.min(delay * 1.5, 120);
        status.textContent = j.posts.length ? `${j.posts.length} new` : "";
      } catch {
        status.textContent = "couldn't update";
      }
      left = Math.round(delay);
    }
    const timer = setInterval(() => {
      if (document.hidden) return;
      left--;
      if (left <= 0) update();
      else if (!status.textContent.endsWith("new") || left < delay - 5) status.textContent = `next update in ${left}s`;
    }, 1000);
    document.querySelector(".tools .update").addEventListener("click", update);
  }
})();
