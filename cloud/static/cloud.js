"use strict";
(() => {
  const BASE = "/cloud";

  // click to copy (payment amount and address)
  document.addEventListener("click", (e) => {
    const c = e.target.closest(".copy");
    if (!c) return;
    navigator.clipboard?.writeText(c.dataset.copy).then(() => {
      c.classList.add("copied");
      setTimeout(() => c.classList.remove("copied"), 1200);
    });
  });

  // ---------------------------------------------------------- signup page
  const code = document.body.dataset.invoice;
  if (code) {
    const status = document.getElementById("paystatus");
    const button = document.getElementById("signupbtn");
    const hint = document.getElementById("signuphint");
    const text = {
      waiting: "Waiting for your payment…",
      seen: "Payment seen. Waiting for 1 confirmation (about 10 minutes)…",
      paid: "Paid. Pick your login below.",
      partial: "Part of the payment arrived.",
      requoted: "The price changed, reloading…",
      used: "This payment was already used for an account.",
      expired: "This payment request expired. Reload for a new one.",
      unknown: "Can't check right now, retrying…",
    };
    let stop = false;
    async function check() {
      if (stop) return;
      try {
        const r = await fetch(`${BASE}/invoice?code=${encodeURIComponent(code)}`);
        const j = await r.json();
        if (j.state === "requoted") { location.reload(); return; }
        status.dataset.state = j.state;
        status.textContent = text[j.state] || j.state;
        if (j.state === "partial") status.textContent += ` Send ${j.missing} BTC more to the same address.`;
        if (j.state === "paid") {
          button.disabled = false;
          hint.hidden = true;
          stop = true;
        }
      } catch {
        status.textContent = text.unknown;
      }
    }
    check();
    setInterval(check, 10000);
    return;
  }

  // ---------------------------------------------------------- file browser
  const csrf = document.body.dataset.csrf;
  if (!csrf) return;

  const list = document.getElementById("list");
  const crumbs = document.getElementById("crumbs");
  const uploads = document.getElementById("uploads");
  const dlg = document.getElementById("dlg");
  let cwd = decodeURIComponent(location.hash.slice(1)) || "/";

  const join = (dir, name) => (dir === "/" ? "/" + name : dir + "/" + name);
  const size = (n) => n >= 1073741824 ? (n / 1073741824).toFixed(2) + " GB"
    : n >= 1048576 ? (n / 1048576).toFixed(1) + " MB"
    : n >= 1024 ? Math.round(n / 1024) + " KB" : n + " B";

  function el(tag, attrs = {}, ...kids) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
      else if (k === "class") e.className = v;
      else e[k] = v;
    }
    e.append(...kids);
    return e;
  }

  async function call(name, body) {
    const r = await fetch(`${BASE}/api/${name}`, {
      method: "POST",
      headers: { "Content-Type": "application/json", "X-CSRF": csrf },
      body: JSON.stringify(body),
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(j.error || "that didn't work");
    return j;
  }

  // small promise wrapper around the one <dialog>
  function ask(text, { input, okText = "OK", body } = {}) {
    document.getElementById("dlgtext").textContent = text;
    const box = document.getElementById("dlgbody");
    box.replaceChildren();
    let field = null;
    if (input !== undefined) {
      field = el("input", { value: input, autocomplete: "off" });
      box.append(field);
    }
    if (body) box.append(body);
    document.getElementById("dlgok").textContent = okText;
    dlg.returnValue = "";
    dlg.showModal();
    if (field) {
      field.focus();
      const dot = input.lastIndexOf(".");
      field.setSelectionRange(0, dot > 0 ? dot : input.length);
    }
    return new Promise((done) => {
      dlg.addEventListener("close", () => done(dlg.returnValue === "ok" ? (field ? field.value : true) : null), { once: true });
    });
  }

  async function load() {
    let j;
    try {
      const r = await fetch(`${BASE}/api/list?path=${encodeURIComponent(cwd)}`);
      j = await r.json();
      if (!r.ok) throw new Error(j.error);
    } catch (e) {
      if (cwd !== "/") { cwd = "/"; location.hash = ""; return load(); }
      list.replaceChildren(el("tr", { class: "empty" }, el("td", { colSpan: 4 }, e.message || "Couldn't load your files.")));
      return;
    }
    const pct = j.quota ? Math.min(100, (j.used / j.quota) * 100) : 0;
    const bar = document.getElementById("quotabar");
    bar.style.width = pct + "%";
    bar.parentElement.classList.toggle("full", pct > 95);
    document.getElementById("quotatext").textContent = `${size(j.used)} of ${size(j.quota)}`;

    const parts = cwd.split("/").filter(Boolean);
    crumbs.replaceChildren(el("a", { href: "#/" }, "My files"));
    parts.forEach((p, i) => crumbs.append(el("span", {}, "/"), el("a", { href: "#" + encodeURIComponent("/" + parts.slice(0, i + 1).join("/")) }, p)));

    if (!j.items.length) {
      list.replaceChildren(el("tr", { class: "empty" }, el("td", { colSpan: 4 }, "Nothing here yet. Upload something or drop files on the page.")));
      return;
    }
    list.replaceChildren(...j.items.map((it) => {
      const p = join(cwd, it.name);
      const link = it.dir
        ? el("a", { href: "#" + encodeURIComponent(p) }, it.name)
        : el("a", { href: `${BASE}/dl?view&path=${encodeURIComponent(p)}`, target: "_blank", rel: "noopener" }, it.name);
      const acts = el("td", { class: "acts" });
      if (!it.dir) {
        acts.append(el("button", { onclick: () => { location.href = `${BASE}/dl?path=${encodeURIComponent(p)}`; } }, "Download"));
        acts.append(el("button", { onclick: () => share(p) }, "Share"));
      }
      acts.append(el("button", { onclick: () => rename(p, it.name) }, "Rename"));
      acts.append(el("button", { onclick: () => remove(p, it) }, "Delete"));
      return el("tr", {},
        el("td", { class: "name" }, el("span", { class: "icon" }, it.dir ? "▸" : ""), link),
        el("td", { class: "num" }, it.dir ? "" : size(it.size)),
        el("td", { class: "when" }, new Date(it.mtime).toLocaleString([], { dateStyle: "medium", timeStyle: "short" })),
        acts);
    }));
  }

  async function act(fn) {
    try {
      await fn();
    } catch (e) {
      await ask(e.message);
    }
    load();
  }

  const rename = async (p, name) => {
    const n = await ask("New name", { input: name, okText: "Rename" });
    if (n && n !== name) act(() => call("rename", { path: p, name: n }));
  };
  const remove = async (p, it) => {
    if (await ask(`Delete ${it.name}${it.dir ? " and everything in it" : ""}?`, { okText: "Delete" })) act(() => call("delete", { path: p }));
  };
  async function share(p) {
    try {
      const { url } = await call("share", { path: p });
      const full = location.origin + url;
      await ask("Anyone with this link can download the file:", { input: full, okText: "Copy" }) && navigator.clipboard?.writeText(full);
    } catch (e) {
      ask(e.message);
    }
  }
  document.getElementById("mkdir").addEventListener("click", async () => {
    const n = await ask("Folder name", { input: "", okText: "Create" });
    if (n) act(() => call("mkdir", { path: cwd, name: n }));
  });

  document.getElementById("showshares").addEventListener("click", async () => {
    document.querySelector(".menu").open = false;
    const r = await fetch(`${BASE}/api/shares`);
    const { shares } = await r.json();
    const box = el("div", { class: "sharelist" });
    if (!shares.length) box.append(el("p", { class: "muted" }, "You haven't shared anything."));
    for (const s of shares) {
      const row = el("div", {},
        el("span", {}, s.path, el("br"), el("small", { class: "muted" }, `${s.downloads} downloads`)),
        el("button", { type: "button", onclick: async () => { await call("unshare", { token: s.token }); row.remove(); } }, "Remove link"));
      box.append(row);
    }
    ask("Shared links", { body: box, okText: "Done" });
  });

  document.getElementById("changepass").addEventListener("click", async () => {
    document.querySelector(".menu").open = false;
    const old = el("input", { type: "password", placeholder: "current password", autocomplete: "current-password" });
    const neu = el("input", { type: "password", placeholder: "new password (10+ characters)", autocomplete: "new-password" });
    if (!(await ask("Change password", { body: el("div", { class: "stack" }, old, neu), okText: "Change" }))) return;
    try {
      await call("password", { old: old.value, password: neu.value });
      ask("Password changed. Other devices were logged out.");
    } catch (e) {
      ask(e.message);
    }
  });

  // uploads, one at a time with progress
  const queue = [];
  let busy = false;
  function enqueue(files) {
    for (const f of files) {
      const bar = el("span");
      const state = el("span", { class: "muted" }, "waiting");
      const row = el("div", { class: "up" }, el("span", {}, f.name), el("div", { class: "bar" }, bar), state);
      uploads.append(row);
      queue.push({ f, dir: cwd, bar, state, row });
    }
    next();
  }
  function next() {
    if (busy || !queue.length) return;
    busy = true;
    const job = queue.shift();
    const xhr = new XMLHttpRequest();
    xhr.open("PUT", `${BASE}/api/upload?dir=${encodeURIComponent(job.dir)}&name=${encodeURIComponent(job.f.name)}`);
    xhr.setRequestHeader("X-CSRF", csrf);
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) {
        job.bar.style.width = (e.loaded / e.total) * 100 + "%";
        job.state.textContent = Math.round((e.loaded / e.total) * 100) + "%";
      }
    };
    xhr.onloadend = () => {
      busy = false;
      if (xhr.status === 200) {
        job.row.remove();
        if (job.dir === cwd) load();
      } else {
        let msg = "failed";
        try { msg = JSON.parse(xhr.responseText).error; } catch {}
        job.row.classList.add("bad");
        job.state.textContent = msg;
        setTimeout(() => job.row.remove(), 8000);
      }
      next();
    };
    job.state.textContent = "0%";
    xhr.send(job.f);
  }
  document.getElementById("upload").addEventListener("change", (e) => {
    enqueue([...e.target.files]);
    e.target.value = "";
  });

  const drop = document.getElementById("drop");
  let depth = 0;
  addEventListener("dragenter", (e) => {
    if (![...e.dataTransfer.types].includes("Files")) return;
    depth++;
    drop.classList.add("on");
  });
  addEventListener("dragleave", () => {
    if (--depth <= 0) { depth = 0; drop.classList.remove("on"); }
  });
  addEventListener("dragover", (e) => e.preventDefault());
  addEventListener("drop", (e) => {
    e.preventDefault();
    depth = 0;
    drop.classList.remove("on");
    const files = [...e.dataTransfer.files];
    if (files.length) enqueue(files);
  });

  addEventListener("hashchange", () => {
    cwd = decodeURIComponent(location.hash.slice(1)) || "/";
    load();
  });
  load();
})();
