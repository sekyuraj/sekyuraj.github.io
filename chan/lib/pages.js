const { esc, BASE } = require("./markup");

function size(n) {
    if (n >= 1 << 20) return (n / (1 << 20)).toFixed(2) + " MB";
    if (n >= 1024) return Math.round(n / 1024) + " KB";
    return n + " B";
}

function when(ms) {
    const d = new Date(ms);
    const iso = d.toISOString();
    return `<time datetime="${iso}">${iso.slice(0, 16).replace("T", " ")} UTC</time>`;
}

function layout(cfg, { title, board, body, attrs = "" }) {
    return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(title)}</title>
<link rel="stylesheet" href="${BASE}/static/chan.css">
<script src="${BASE}/static/chan.js" defer></script>
</head>
<body${attrs}>
<header class="bar">
  <nav class="boards">${cfg.boards.map((b) => `<a href="${BASE}/${b.id}/"${board && board.id === b.id ? ' class="here"' : ""} title="${esc(b.name)}">/${b.id}/</a>`).join(" ")}</nav>
  <nav class="links"><a href="${BASE}/">${esc(cfg.title)}</a> <a href="/">sekraj</a></nav>
</header>
${body}
<footer class="foot">${esc(cfg.title)} · everything here is posted by anonymous users · <a href="${BASE}/rules">rules</a></footer>
</body>
</html>`;
}

function boardHead(board) {
    return `<div class="head"><h1>/${board.id}/ ${esc(board.name)}</h1>${board.desc ? `<p>${esc(board.desc)}</p>` : ""}</div>`;
}

function postForm(cfg, board, thread, mod, open = !thread) {
    const label = thread ? "Reply to this thread" : "Start a new thread";
    return `<details class="form"${open ? " open" : ""} id="postform">
<summary>${label}</summary>
<form method="post" action="${BASE}/${board.id}/post" enctype="multipart/form-data">
  ${thread ? `<input type="hidden" name="thread" value="${thread}">` : ""}
  <input type="hidden" name="password" class="delpass">
  <label><span>Name</span><input name="name" maxlength="60" placeholder="Anonymous" autocomplete="off"></label>
  <label><span>Options</span><input name="options" maxlength="20" placeholder="sage" autocomplete="off"></label>
  ${thread ? "" : `<label><span>Subject</span><input name="subject" maxlength="100" autocomplete="off"></label>`}
  <label><span>Comment</span><textarea name="comment" rows="5" maxlength="${cfg.maxComment}"></textarea></label>
  <label><span>File</span><input type="file" name="file" accept="image/jpeg,image/png,image/gif,image/webp,video/webm,video/mp4"${thread ? "" : " required"}>
    <span class="check"><input type="checkbox" name="spoiler" value="1"> spoiler</span></label>
  ${cfg.captcha && !mod ? `<div class="cap"><span>Captcha</span>
    <div class="capbox"><img class="capimg" alt="captcha" width="220" height="70" hidden><button type="button" class="capnew">new</button></div>
    <input type="hidden" name="captcha_id" class="capid">
    <input name="captcha" class="capin" maxlength="8" autocomplete="off" placeholder="type the letters" required>
    <noscript><p>The captcha needs JavaScript.</p></noscript></div>` : ""}
  <div class="send"><button type="submit">Post</button>
  <span class="hint">max ${cfg.maxFileMB} MB · jpg png gif webp webm mp4 · &gt;text · &gt;&gt;number · [spoiler]text[/spoiler]</span></div>
</form>
</details>`;
}

function fileBlock(p, board, op) {
    if (!p.file) return "";
    const src = `${BASE}/src/${p.file}`;
    const video = p.file_mime.startsWith("video/");
    const thumb = p.spoiler
        ? `<span class="spoilthumb${op ? " big" : ""}">spoiler</span>`
        : `<img src="${BASE}/thumb/${p.thumb}" width="${p.tn_w}" height="${p.tn_h}" loading="lazy" alt="">`;
    return `<div class="file">
  <div class="fileinfo"><a href="${src}" target="_blank" rel="noopener">${esc(p.file_name)}</a> ${size(p.file_size)}, ${p.file_w}×${p.file_h}${video ? ", video" : ""}</div>
  <a class="thumb${video ? " vid" : ""}" href="${src}" data-full="${src}" data-kind="${video ? "video" : "image"}" target="_blank" rel="noopener">${thumb}</a>
</div>`;
}

function modBar(p, board, csrf) {
    const f = (act, label, extra = "") => `<form method="post" action="${BASE}/mod/act">
<input type="hidden" name="csrf" value="${csrf}"><input type="hidden" name="board" value="${board.id}">
<input type="hidden" name="no" value="${p.no}"><input type="hidden" name="act" value="${act}">${extra}<button>${label}</button></form>`;
    const ban = `<input name="reason" placeholder="ban reason" required><input name="days" size="3" placeholder="days" title="empty = forever">`;
    return `<div class="modbar">
${f("del", "delete")}${p.file ? f("delfile", "delete file") + f("banfile", "delete + ban file") : ""}
${p.no === p.thread ? f(p.sticky ? "unsticky" : "sticky", p.sticky ? "unsticky" : "sticky") + f(p.locked ? "unlock" : "lock", p.locked ? "unlock" : "lock") : ""}
${f("ban", "ban", ban)}${f("bandel", "ban + delete all", ban)}
</div>`;
}

function post(p, board, opts = {}) {
    const op = p.no === p.thread;
    const href = `${BASE}/${board.id}/thread/${p.thread}`;
    const back = (opts.backlinks?.get(p.no) || [])
        .map((n) => `<a class="ql" href="${href}#p${n}" data-b="${board.id}" data-no="${n}">&gt;&gt;${n}</a>`)
        .join(" ");
    return `<article class="post ${op ? "op" : "reply"}" id="p${p.no}" data-no="${p.no}">
${fileBlock(p, board, op)}
<div class="info">
  ${p.subject ? `<span class="subject">${esc(p.subject)}</span>` : ""}
  <span class="name">${esc(p.name)}</span>${p.trip ? `<span class="trip">${esc(p.trip)}</span>` : ""}
  ${p.uid ? `<span class="uid" title="poster ID in this thread">ID ${esc(p.uid)}</span>` : ""}
  ${when(p.time)}
  <a class="no" href="${href}#p${p.no}">No.</a><a class="qno" href="${href}#q${p.no}" data-no="${p.no}">${p.no}</a>
  ${p.sticky ? '<span class="flag">sticky</span>' : ""}${p.locked ? '<span class="flag">locked</span>' : ""}
  ${op && opts.replyLink ? `<a class="open" href="${href}">open thread</a>` : ""}
  ${opts.preview ? "" : `<span class="acts"><button class="report" type="button">report</button><button class="delete" type="button">delete</button></span>`}
</div>
<div class="com">${p.html}</div>
${back ? `<div class="backlinks">replies: ${back}</div>` : ""}
${opts.mod && !opts.preview ? modBar(p, board, opts.csrf) : ""}
</article>`;
}

function threadBlock(op, replies, board, opts, omitted) {
    return `<section class="thread" id="t${op.no}">
${post(op, board, { ...opts, replyLink: true })}
${omitted ? `<p class="omitted">${omitted}</p>` : ""}
${replies.map((r) => post(r, board, opts)).join("\n")}
</section>`;
}

function pager(board, page, pages) {
    let out = "";
    for (let i = 1; i <= pages; i++) {
        const href = i === 1 ? `${BASE}/${board.id}/` : `${BASE}/${board.id}/${i}`;
        out += i === page ? `<b>${i}</b> ` : `<a href="${href}">${i}</a> `;
    }
    return `<nav class="pager">${out}<a href="${BASE}/${board.id}/catalog">catalog</a></nav>`;
}

function boardIndex(cfg, board, threads, page, pages, opts) {
    const body = `${boardHead(board)}
${postForm(cfg, board, 0, opts.mod)}
<nav class="pager top"><a href="${BASE}/${board.id}/catalog">catalog</a></nav>
${threads.length ? threads.map((t) => threadBlock(t.op, t.replies, board, opts, t.omitted)).join("\n") : '<p class="empty">No threads yet. Start one.</p>'}
${pager(board, page, pages)}`;
    return layout(cfg, { title: `/${board.id}/ ${board.name}`, board, body, attrs: ` data-board="${board.id}"` });
}

function threadPage(cfg, board, posts, opts) {
    const op = posts[0];
    const title = op.subject || op.comment.slice(0, 40) || `No.${op.no}`;
    const body = `${boardHead(board)}
${op.locked && !opts.mod ? '<p class="notice">This thread is locked.</p>' : postForm(cfg, board, op.no, opts.mod)}
<nav class="pager top"><a href="${BASE}/${board.id}/">back to /${board.id}/</a> <a href="${BASE}/${board.id}/catalog">catalog</a> <a href="#bottom">bottom</a></nav>
<section class="thread" id="t${op.no}">
${posts.map((p) => post(p, board, opts)).join("\n")}
</section>
<div class="tools" id="bottom"><a href="#">top</a> <button type="button" class="update">update</button> <span class="status"></span>
<span class="counts">${posts.length - 1} replies, ${posts.filter((p) => p.file).length} files</span></div>`;
    return layout(cfg, {
        title: `/${board.id}/ ${title}`,
        board,
        body,
        attrs: ` data-board="${board.id}" data-thread="${op.no}" data-last="${posts[posts.length - 1].no}"`,
    });
}

function catalog(cfg, board, threads, mod) {
    const cells = threads.map(({ op, replies, images }) => {
        const img = op.file
            ? op.spoiler ? `<span class="spoilthumb">spoiler</span>` : `<img src="${BASE}/thumb/${op.thumb}" loading="lazy" alt="">`
            : "";
        const text = op.comment.length > 180 ? op.comment.slice(0, 180) + "…" : op.comment;
        return `<a class="cell" href="${BASE}/${board.id}/thread/${op.no}">
${img}<span class="meta">R ${replies} · F ${images}${op.sticky ? " · sticky" : ""}${op.locked ? " · locked" : ""}</span>
${op.subject ? `<b>${esc(op.subject)}</b>` : ""}<span class="teaser">${esc(text)}</span></a>`;
    }).join("\n");
    const body = `${boardHead(board)}
${postForm(cfg, board, 0, mod, false)}
<nav class="pager top"><a href="${BASE}/${board.id}/">back to the index</a></nav>
<div class="catalog">${cells || '<p class="empty">No threads yet.</p>'}</div>`;
    return layout(cfg, { title: `/${board.id}/ catalog`, board, body, attrs: ` data-board="${board.id}"` });
}

function home(cfg, total) {
    const body = `<div class="head home"><h1>${esc(cfg.title)}</h1><p>${esc(cfg.tagline)}</p></div>
<ul class="boardlist">
${cfg.boards.map((b) => `<li><a href="${BASE}/${b.id}/">/${b.id}/</a> <b>${esc(b.name)}</b>${b.desc ? ` <span>${esc(b.desc)}</span>` : ""}</li>`).join("\n")}
</ul>
<p class="stats">${total} posts so far.</p>`;
    return layout(cfg, { title: cfg.title, body });
}

function rules(cfg) {
    const body = `<div class="head"><h1>rules</h1></div>
<ol class="rules">${cfg.rules.map((r) => `<li>${esc(r)}</li>`).join("\n")}</ol>`;
    return layout(cfg, { title: `${cfg.title} rules`, body });
}

function message(cfg, title, text, back) {
    const body = `<div class="msg"><h1>${esc(title)}</h1><p>${esc(text)}</p>${back ? `<p><a href="${esc(back)}">go back</a></p>` : ""}</div>`;
    return layout(cfg, { title, body });
}

function modLogin(cfg, failed) {
    const body = `<div class="msg"><h1>moderator login</h1>
${failed ? "<p>Wrong password.</p>" : ""}
<form method="post" action="${BASE}/mod/login" class="login"><input type="password" name="password" autofocus required><button>log in</button></form></div>`;
    return layout(cfg, { title: "mod login", body });
}

function modHome(cfg, { reports, bans, recent, boardsById, csrf, lookupPost }) {
    const rep = reports.map((r) => {
        const p = lookupPost(r.board, r.no);
        const board = boardsById[r.board];
        if (!p || !board) return "";
        return `<div class="report-row"><p>/${r.board}/ No.${r.no} · ${r.n} report${r.n > 1 ? "s" : ""}: ${esc(r.reasons)}
<form method="post" action="${BASE}/mod/act"><input type="hidden" name="csrf" value="${csrf}">
<input type="hidden" name="board" value="${r.board}"><input type="hidden" name="no" value="${r.no}">
<input type="hidden" name="act" value="dismiss"><button>dismiss</button></form></p>
${post(p, board, { mod: true, csrf })}</div>`;
    }).join("\n");
    const banRows = bans.map((b) => `<tr><td>${b.board ? `/${b.board}/ ${b.no}` : ""}</td><td>${esc(b.reason)}</td>
<td>${when(b.created)}</td><td>${b.until ? when(b.until) : "forever"}</td>
<td><form method="post" action="${BASE}/mod/act"><input type="hidden" name="csrf" value="${csrf}">
<input type="hidden" name="ban" value="${b.id}"><input type="hidden" name="act" value="unban"><button>unban</button></form></td></tr>`).join("\n");
    const recentPosts = recent.map((p) => {
        const board = boardsById[p.board];
        return board ? `<p class="where"><a href="${BASE}/${p.board}/thread/${p.thread}#p${p.no}">/${p.board}/ ${p.no}</a></p>${post(p, board, { mod: true, csrf })}` : "";
    }).join("\n");
    const body = `<div class="head"><h1>moderation</h1>
<form method="post" action="${BASE}/mod/logout"><input type="hidden" name="csrf" value="${csrf}"><button>log out</button></form></div>
<h2>reports</h2>${rep || '<p class="empty">No reports.</p>'}
<h2>bans</h2>${banRows ? `<table class="bans"><tr><th>post</th><th>reason</th><th>from</th><th>until</th><th></th></tr>${banRows}</table>` : '<p class="empty">No active bans.</p>'}
<h2>latest posts</h2>${recentPosts || '<p class="empty">Nothing posted yet.</p>'}`;
    return layout(cfg, { title: "moderation", body });
}

module.exports = { post, boardIndex, threadPage, catalog, home, rules, message, modLogin, modHome };
