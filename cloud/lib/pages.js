const BASE = "/cloud";

function esc(s) {
    return String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
}

function layout({ title, body, wide = false, attrs = "" }) {
    return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(title)}</title>
<link rel="stylesheet" href="${BASE}/static/cloud.css">
<script src="${BASE}/static/cloud.js" defer></script>
</head>
<body${attrs}>
<main class="${wide ? "wide" : "narrow"}">
${body}
</main>
</body>
</html>`;
}

function brand() {
    return `<a class="brand" href="/">sek<b>cloud</b></a>`;
}

function login({ error, username = "", signedUp }) {
    return layout({
        title: "Log in · sekcloud",
        body: `${brand()}
<h1>Log in</h1>
${signedUp ? `<p class="ok">Your account is ready. Log in to start uploading.</p>` : ""}
${error ? `<p class="err">${esc(error)}</p>` : ""}
<form method="post" action="${BASE}/login" class="stack">
  <label>Username <input name="username" value="${esc(username)}" autocomplete="username" required autofocus></label>
  <label>Password <input name="password" type="password" autocomplete="current-password" required></label>
  <button>Log in</button>
</form>
<p class="muted">No account yet? <a href="${BASE}/signup">Create one</a></p>`,
    });
}

function signupClosed(reason) {
    return layout({
        title: "Sign up · sekcloud",
        body: `${brand()}
<h1>Create an account</h1>
<p class="err">${esc(reason)}</p>
<p class="muted"><a href="${BASE}/login">Back to log in</a></p>`,
    });
}

function signup({ inv, btc, usd, code, quota, error, username = "" }) {
    const uri = `bitcoin:${inv.address}?amount=${btc}`;
    return layout({
        title: "Sign up · sekcloud",
        attrs: ` data-invoice="${esc(code)}"`,
        body: `${brand()}
<h1>Create an account</h1>
<p class="muted">An account costs $${usd}, paid once in bitcoin. You get ${quota} of storage.</p>

<section class="pay" id="pay">
  <a href="${esc(uri)}" class="qr"><img src="${BASE}/qr/${esc(code)}.svg" width="220" height="220" alt="QR code for the payment"></a>
  <div class="paydetails">
    <p class="step">1. Send exactly</p>
    <p class="big"><span class="copy" data-copy="${btc}">${btc}</span> BTC</p>
    <p class="step">to</p>
    <p class="addr"><span class="copy" data-copy="${esc(inv.address)}">${esc(inv.address)}</span></p>
    <p class="status" id="paystatus" data-state="waiting">Waiting for your payment…</p>
  </div>
</section>
<p class="muted small">The price is locked for 24 hours. Tap the amount or address to copy it.
Leave this page open, or keep this link to come back:
<a href="${BASE}/signup?code=${esc(code)}">${BASE}/signup?code=…</a></p>

<h2>2. Pick your login</h2>
${error ? `<p class="err">${esc(error)}</p>` : ""}
<form method="post" action="${BASE}/signup" class="stack" id="signupform">
  <input type="hidden" name="code" value="${esc(code)}">
  <label>Username <input name="username" value="${esc(username)}" pattern="[A-Za-z0-9_.-]{3,32}" title="3 to 32 letters, numbers, dots, dashes or underscores" autocomplete="username" required></label>
  <label>Password <input name="password" type="password" minlength="10" autocomplete="new-password" required></label>
  <label>Password again <input name="password2" type="password" minlength="10" autocomplete="new-password" required></label>
  <button id="signupbtn" disabled>Create account</button>
  <p class="muted small" id="signuphint">The button unlocks once your payment has one confirmation (about 10 minutes).</p>
</form>
<p class="muted">Already have an account? <a href="${BASE}/login">Log in</a></p>`,
    });
}

function app({ username, csrf }) {
    return layout({
        title: "sekcloud",
        wide: true,
        attrs: ` data-csrf="${esc(csrf)}" data-user="${esc(username)}"`,
        body: `<header class="top">
  ${brand()}
  <div class="quota"><div class="bar"><span id="quotabar"></span></div><span id="quotatext"></span></div>
  <details class="menu"><summary>${esc(username)}</summary>
    <div class="menubox">
      <button type="button" id="showshares">Shared links</button>
      <button type="button" id="changepass">Change password</button>
      <form method="post" action="${BASE}/logout"><input type="hidden" name="csrf" value="${esc(csrf)}"><button>Log out</button></form>
    </div>
  </details>
</header>
<nav class="crumbs" id="crumbs"></nav>
<div class="toolbar">
  <label class="button primary">Upload<input type="file" id="upload" multiple hidden></label>
  <button type="button" id="mkdir">New folder</button>
  <span class="muted small" id="hint">or drop files anywhere on this page</span>
</div>
<div id="uploads"></div>
<table class="files">
  <thead><tr><th>Name</th><th class="num">Size</th><th class="when">Changed</th><th></th></tr></thead>
  <tbody id="list"><tr><td colspan="4" class="muted">Loading…</td></tr></tbody>
</table>
<div class="drop" id="drop">Drop to upload</div>
<dialog id="dlg"><form method="dialog"><p id="dlgtext"></p><div id="dlgbody"></div>
<div class="row"><button value="cancel" type="submit" class="ghost">Cancel</button><button value="ok" type="submit" id="dlgok">OK</button></div></form></dialog>`,
    });
}

function sharePage({ name, size, token }) {
    return layout({
        title: `${name} · sekcloud`,
        body: `${brand()}
<h1 class="filename">${esc(name)}</h1>
<p class="muted">${esc(size)} · shared with a link</p>
<p><a class="button primary" href="${BASE}/s/${esc(token)}/dl">Download</a></p>`,
    });
}

function message(title, text) {
    return layout({ title, body: `${brand()}<h1>${esc(title)}</h1><p>${esc(text)}</p><p><a href="${BASE}/">Back</a></p>` });
}

module.exports = { login, signup, signupClosed, app, sharePage, message, esc };
