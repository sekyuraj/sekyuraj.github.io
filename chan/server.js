// tuxchan: a small imageboard at sekraj.duckdns.org/chan/
// Caddy sends /chan/* here. Plain Node + node:sqlite, ffmpeg for thumbnails.
//
//   node server.js            run (127.0.0.1:3001)
//   node server.js passwd     set the moderator password

const http = require("http");
const fs = require("fs");
const fsp = require("fs/promises");
const path = require("path");
const crypto = require("crypto");

const { open } = require("./lib/db");
const media = require("./lib/media");
const markup = require("./lib/markup");
const pages = require("./lib/pages");
const captcha = require("./lib/captcha");
const multipart = require("./lib/multipart");

const BASE = markup.BASE;
const PORT = Number(process.env.PORT) || 3001;
const DATA = process.env.TUXCHAN_DATA || path.join(process.env.HOME || "/tmp", ".local/share/tuxchan");
const STATIC = path.join(__dirname, "static");

// ---------------------------------------------------------------- config

const DEFAULTS = {
    title: "tuxchan",
    tagline: "an anonymous image board",
    boards: [
        { id: "b", name: "Random", desc: "anything goes, within the rules", ids: true },
        { id: "g", name: "Technology", desc: "computers, Linux, programming" },
        { id: "v", name: "Video Games", desc: "" },
        { id: "mu", name: "Music", desc: "" },
    ],
    rules: [
        "Nothing illegal. That gets deleted, banned and reported.",
        "No personal information about other people.",
        "No spam or flooding.",
        "Not-safe-for-work images only on /b/, and spoiler them.",
        "Use the report button for anything that breaks these rules.",
    ],
    captcha: true,
    maxFileMB: 8,
    maxComment: 3000,
    threadsPerPage: 10,
    pages: 10,
    bumpLimit: 300,
    fileLimit: 150,
    maxVideoSeconds: 300,
    threadCooldown: 300,
    replyCooldown: 20,
    modHash: "",
};

fs.mkdirSync(path.join(DATA, "src"), { recursive: true });
fs.mkdirSync(path.join(DATA, "thumb"), { recursive: true });
fs.mkdirSync(path.join(DATA, "tmp"), { recursive: true });

const CFG_FILE = path.join(DATA, "config.json");
let cfg = { ...DEFAULTS };
if (fs.existsSync(CFG_FILE)) cfg = { ...DEFAULTS, ...JSON.parse(fs.readFileSync(CFG_FILE, "utf8")) };
else fs.writeFileSync(CFG_FILE, JSON.stringify(cfg, null, 2) + "\n");
const boardsById = Object.fromEntries(cfg.boards.map((b) => [b.id, b]));

const SECRET_FILE = path.join(DATA, "secret");
if (!fs.existsSync(SECRET_FILE)) fs.writeFileSync(SECRET_FILE, crypto.randomBytes(32).toString("hex"), { mode: 0o600 });
const SECRET = fs.readFileSync(SECRET_FILE, "utf8").trim();

if (process.argv[2] === "passwd") {
    setPassword();
    return;
}

const { st, tx } = open(DATA);
for (const b of cfg.boards) st.addCounter.run(b.id);

// ---------------------------------------------------------------- helpers

const hmac = (s) => crypto.createHmac("sha256", SECRET).update(s).digest("base64url");

function clientIp(req) {
    const peer = req.socket.remoteAddress || "";
    const local = peer === "127.0.0.1" || peer === "::1" || peer === "::ffff:127.0.0.1";
    const xff = req.headers["x-forwarded-for"];
    if (local && xff) return xff.split(",").pop().trim();
    return peer;
}

function tripcode(raw) {
    const i = raw.indexOf("#");
    if (i < 0) return { name: raw, trip: null };
    const name = raw.slice(0, i);
    const pass = raw.slice(i + 1);
    if (!pass) return { name, trip: null };
    if (pass.startsWith("#")) return { name, trip: "!!" + hmac("trip:" + pass.slice(1)).slice(0, 10) };
    return { name, trip: "!" + crypto.createHash("sha256").update("tuxchan-trip:" + pass).digest("base64url").slice(0, 10) };
}

function threadOf(board, no) {
    const p = st.get.get(board, no);
    return p ? p.thread : null;
}

const SECURITY = {
    "Content-Security-Policy": "default-src 'self'; img-src 'self' data:; media-src 'self'; style-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'",
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "same-origin",
};

function send(res, code, body, type = "text/html; charset=utf-8", extra = {}) {
    res.writeHead(code, { "Content-Type": type, "Cache-Control": "no-store", ...SECURITY, ...extra });
    res.end(body);
}

function redirect(res, to, extra = {}) {
    res.writeHead(303, { Location: to, "Cache-Control": "no-store", ...extra });
    res.end();
}

function fail(res, code, title, text, back) {
    send(res, code, pages.message(cfg, title, text, back));
}

function readBody(req, limit) {
    return new Promise((ok, bad) => {
        const parts = [];
        let size = 0;
        req.on("data", (c) => {
            size += c.length;
            if (size > limit) {
                bad(Object.assign(new Error("too big"), { code: 413 }));
                req.destroy();
            } else parts.push(c);
        });
        req.on("end", () => ok(Buffer.concat(parts)));
        req.on("error", bad);
    });
}

function formFields(buf) {
    return Object.fromEntries(new URLSearchParams(buf.toString("utf8")));
}

function backlinksFor(posts) {
    const map = new Map();
    const inThread = new Set(posts.map((p) => p.no));
    for (const p of posts) {
        for (const n of markup.quotedNumbers(p.comment)) {
            if (!inThread.has(n) || n === p.no) continue;
            if (!map.has(n)) map.set(n, []);
            map.get(n).push(p.no);
        }
    }
    return map;
}

// ---------------------------------------------------------------- moderator sessions

const sessions = new Map(); // token -> expiry

function cookies(req) {
    const out = {};
    for (const part of (req.headers.cookie || "").split(";")) {
        const i = part.indexOf("=");
        if (i > 0) out[part.slice(0, i).trim()] = part.slice(i + 1).trim();
    }
    return out;
}

function modSession(req) {
    const t = cookies(req).tuxchan_mod;
    if (!t) return null;
    const exp = sessions.get(t);
    if (!exp || exp < Date.now()) {
        sessions.delete(t);
        return null;
    }
    return { token: t, csrf: hmac("csrf:" + t) };
}

function setPassword() {
    process.stdout.write("New moderator password: ");
    process.stdin.setEncoding("utf8");
    process.stdin.once("data", (line) => {
        const pass = line.trim();
        if (pass.length < 8) {
            console.error("use at least 8 characters");
            process.exit(1);
        }
        const salt = crypto.randomBytes(16).toString("hex");
        cfg.modHash = salt + ":" + crypto.scryptSync(pass, salt, 32).toString("hex");
        fs.writeFileSync(CFG_FILE, JSON.stringify(cfg, null, 2) + "\n");
        console.log("saved. Restart tuxchan for it to take effect.");
        process.exit(0);
    });
}

function checkPassword(pass) {
    if (!cfg.modHash) return false;
    const [salt, hash] = cfg.modHash.split(":");
    const got = crypto.scryptSync(pass, salt, 32);
    return crypto.timingSafeEqual(got, Buffer.from(hash, "hex"));
}

// ---------------------------------------------------------------- flood control

const lastPost = new Map(); // iphash -> { thread, reply }

function cooldown(iphash, isThread) {
    const now = Date.now();
    const l = lastPost.get(iphash) || { thread: 0, reply: 0 };
    const wait = isThread ? cfg.threadCooldown * 1000 - (now - l.thread) : cfg.replyCooldown * 1000 - (now - l.reply);
    return wait > 0 ? Math.ceil(wait / 1000) : 0;
}

function remember(iphash, isThread) {
    const l = lastPost.get(iphash) || { thread: 0, reply: 0 };
    l.reply = Date.now();
    if (isThread) l.thread = Date.now();
    lastPost.set(iphash, l);
    if (lastPost.size > 50000) lastPost.clear();
}

// ---------------------------------------------------------------- deleting

async function removeFiles(rows) {
    for (const r of rows) {
        if (r.file) await fsp.rm(path.join(DATA, "src", r.file), { force: true });
        if (r.thumb) await fsp.rm(path.join(DATA, "thumb", r.thumb), { force: true });
    }
}

async function deletePost(board, no) {
    const p = st.get.get(board, no);
    if (!p) return;
    if (p.no === p.thread) {
        const files = st.filesOfThread.all(board, no);
        st.delThread.run(board, no);
        await removeFiles(files);
    } else {
        st.delPost.run(board, no);
        await removeFiles([p]);
    }
    st.dismiss.run(board, no);
}

async function deleteFile(board, no) {
    const p = st.get.get(board, no);
    if (!p || !p.file) return;
    st.delFile.run(board, no);
    await removeFiles([p]);
}

async function prune(board) {
    const old = st.oldest.all(board, cfg.threadsPerPage * cfg.pages);
    for (const t of old) await deletePost(board, t.no);
}

// ---------------------------------------------------------------- posting

async function handleUpload(file) {
    if (file.data.length > cfg.maxFileMB * 1024 * 1024) throw new Error(`File is bigger than ${cfg.maxFileMB} MB.`);
    const type = media.sniff(file.data);
    if (!type) throw new Error("That file type isn't allowed. Use jpg, png, gif, webp, webm or mp4.");

    const sha = crypto.createHash("sha256").update(file.data).digest("hex");
    if (st.fileBanned.get(sha)) throw new Error("That file is banned here.");

    const base = Date.now().toString() + crypto.randomInt(100, 999);
    const tmp = path.join(DATA, "tmp", base + "." + type.ext);
    const final = base + "." + type.ext;
    const finalPath = path.join(DATA, "src", final);
    try {
        if (type.video) {
            await fsp.writeFile(tmp, file.data);
            const info = await media.probe(tmp, type);
            if (info.duration > cfg.maxVideoSeconds) throw new Error(`Videos can be at most ${cfg.maxVideoSeconds} seconds.`);
            await media.cleanVideo(tmp, type, finalPath);
            return { type, sha, final, info, size: (await fsp.stat(finalPath)).size };
        }
        const clean = media.stripImage(file.data, type);
        await fsp.writeFile(tmp, clean);
        const info = await media.probe(tmp, type);
        if (info.w > 12000 || info.h > 12000 || info.w * info.h > 60e6) throw new Error("That image is too large.");
        await fsp.rename(tmp, finalPath);
        return { type, sha, final, info, size: clean.length };
    } catch (e) {
        await fsp.rm(finalPath, { force: true });
        if (e.message.includes("allowed") || e.message.includes("seconds") || e.message.includes("too large")) throw e;
        throw new Error("Couldn't read that file. It may be broken.");
    } finally {
        await fsp.rm(tmp, { force: true });
    }
}

async function makeThumb(up, op) {
    const max = op ? 250 : 150;
    const thumbExt = up.type.ext === "png" || up.type.ext === "gif" || up.type.ext === "webp" ? "png" : "jpg";
    const thumb = up.final.replace(/\.\w+$/, "") + "." + thumbExt;
    await media.thumbnail(path.join(DATA, "src", up.final), up.type, path.join(DATA, "thumb", thumb), max);
    const scale = Math.min(1, max / up.info.w, max / up.info.h);
    return { thumb, tn_w: Math.max(1, Math.round(up.info.w * scale)), tn_h: Math.max(1, Math.round(up.info.h * scale)) };
}

async function newPost(req, res, board) {
    const ip = clientIp(req);
    const iphash = hmac("ip:" + ip);
    const mod = modSession(req);

    const ban = st.activeBan.get(iphash, Date.now());
    if (ban && !mod) {
        return fail(res, 403, "You are banned",
            `Reason: ${ban.reason}. ${ban.until ? "Ends " + new Date(ban.until).toISOString().slice(0, 16).replace("T", " ") + " UTC." : "This ban doesn't end."}`);
    }

    let form;
    try {
        const body = await readBody(req, (cfg.maxFileMB + 1) * 1024 * 1024);
        form = multipart.parse(body, req.headers["content-type"]);
    } catch (e) {
        return fail(res, e.code === 413 ? 413 : 400, "Post failed", e.code === 413 ? `File is bigger than ${cfg.maxFileMB} MB.` : e.message);
    }
    const f = form.fields;
    const threadNo = Number(f.thread) || 0;
    const back = threadNo ? `${BASE}/${board.id}/thread/${threadNo}` : `${BASE}/${board.id}/`;
    const no = (title, text) => fail(res, 400, title, text, back);

    if (cfg.captcha && !mod && !captcha.check(f.captcha_id, f.captcha)) return no("Wrong captcha", "The letters didn't match. Try again with a new one.");

    const comment = (f.comment || "").replace(/\r\n?/g, "\n").replace(/[‪-‮⁦-⁩]/g, "").trim();
    if (comment.length > cfg.maxComment) return no("Comment too long", `Keep it under ${cfg.maxComment} characters.`);
    const file = form.files.file;
    if (!threadNo && !file) return no("No file", "A new thread needs an image or video.");
    if (!comment && !file) return no("Empty post", "Write something or attach a file.");

    let thread = null;
    if (threadNo) {
        thread = st.get.get(board.id, threadNo);
        if (!thread || thread.no !== thread.thread) return no("Thread gone", "That thread doesn't exist anymore.");
        if (thread.locked && !mod) return no("Locked", "This thread is locked.");
        if (file && st.stats.get(board.id, threadNo).images >= cfg.fileLimit) return no("File limit", "This thread has reached its file limit. Post without a file.");
    }

    const wait = mod ? 0 : cooldown(iphash, !threadNo);
    if (wait) return no("Slow down", `You can post again in ${wait} seconds.`);

    let up = null;
    let tn = null;
    if (file) {
        try {
            up = await handleUpload(file);
            tn = await makeThumb(up, !threadNo);
        } catch (e) {
            if (up) await removeFiles([{ file: up.final }]);
            return no("File problem", e.message);
        }
    }

    const { name, trip } = tripcode((f.name || "").trim().slice(0, 60));
    const options = (f.options || "").toLowerCase();
    const sage = options.includes("sage") ? 1 : 0;
    const subject = threadNo ? null : (f.subject || "").trim().slice(0, 100) || null;
    const delpass = f.password ? hmac("del:" + f.password) : null;
    const now = Date.now();
    const origName = file ? (path.basename(file.filename).replace(/[^\p{L}\p{N}._ ()-]/gu, "_").slice(0, 80) || "file") : null;

    let postNo;
    try {
        postNo = tx(() => {
            const n = st.nextNo.get(board.id).last;
            const t = threadNo || n;
            const uid = board.ids ? hmac(`uid:${iphash}:${board.id}:${t}`).slice(0, 8) : null;
            const html = markup.render(comment, board.id, t, threadOf);
            st.insert.run(
                board.id, n, t, now, threadNo ? null : now,
                name || "Anonymous", trip, uid, subject, comment, html,
                sage, iphash, delpass,
                up ? up.final : null, tn ? tn.thumb : null, origName, up ? up.size : null,
                up ? up.info.w : null, up ? up.info.h : null, tn ? tn.tn_w : null, tn ? tn.tn_h : null,
                up ? up.sha : null, up ? up.type.mime : null, f.spoiler ? 1 : 0,
            );
            if (threadNo && !sage && st.stats.get(board.id, threadNo).replies <= cfg.bumpLimit) st.bump.run(now, board.id, threadNo);
            return n;
        });
    } catch (e) {
        if (up) await removeFiles([{ file: up.final, thumb: tn?.thumb }]);
        throw e;
    }

    remember(iphash, !threadNo);
    if (!threadNo) await prune(board.id);
    const t = threadNo || postNo;
    redirect(res, `${BASE}/${board.id}/thread/${t}?posted=${postNo}#p${postNo}`);
}

async function userDelete(req, res, board) {
    const f = formFields(await readBody(req, 4096));
    const p = st.get.get(board.id, Number(f.no));
    if (!p || !p.delpass || !f.password || hmac("del:" + f.password) !== p.delpass) {
        return send(res, 403, "Wrong password, or not your post.", "text/plain; charset=utf-8");
    }
    if (f.fileonly === "1") await deleteFile(board.id, p.no);
    else await deletePost(board.id, p.no);
    send(res, 200, "deleted", "text/plain; charset=utf-8");
}

async function userReport(req, res, board) {
    const f = formFields(await readBody(req, 4096));
    const iphash = hmac("ip:" + clientIp(req));
    const p = st.get.get(board.id, Number(f.no));
    if (!p) return send(res, 404, "That post is gone.", "text/plain; charset=utf-8");
    if (st.reportCount.get(iphash, Date.now() - 3600e3).n >= 20) return send(res, 429, "Too many reports, try later.", "text/plain; charset=utf-8");
    st.addReport.run(board.id, p.no, (f.reason || "no reason").slice(0, 200), Date.now(), iphash);
    send(res, 200, "Reported. Thanks.", "text/plain; charset=utf-8");
}

// ---------------------------------------------------------------- moderation

async function modAction(req, res) {
    const mod = modSession(req);
    const f = formFields(await readBody(req, 8192));
    if (!mod || f.csrf !== mod.csrf) return fail(res, 403, "Not allowed", "Log in again.", `${BASE}/mod`);
    const back = req.headers.referer && new URL(req.headers.referer, "http://x").pathname.startsWith(BASE) ? req.headers.referer : `${BASE}/mod`;
    const board = f.board;
    const no = Number(f.no);
    const p = board ? st.get.get(board, no) : null;
    const days = Number(f.days);
    const until = days > 0 ? Date.now() + days * 86400e3 : null;

    switch (f.act) {
        case "del": await deletePost(board, no); break;
        case "delfile": await deleteFile(board, no); break;
        case "banfile":
            if (p?.file_sha) st.banFile.run(p.file_sha, "banned by a moderator", Date.now());
            await deleteFile(board, no);
            break;
        case "sticky": case "unsticky": st.setFlag.sticky.run(f.act === "sticky" ? 1 : 0, board, no); break;
        case "lock": case "unlock": st.setFlag.locked.run(f.act === "lock" ? 1 : 0, board, no); break;
        case "ban": case "bandel":
            if (!p) break;
            st.addBan.run(p.iphash, (f.reason || "no reason").slice(0, 200), Date.now(), until, board, no);
            if (f.act === "bandel") {
                for (const q of st.postsByIp.all(p.iphash)) await deletePost(q.board, q.no);
            }
            break;
        case "unban": st.unban.run(Number(f.ban)); break;
        case "dismiss": st.dismiss.run(board, no); break;
    }
    const gone = p && !st.get.get(board, no);
    redirect(res, gone && p.no === p.thread ? `${BASE}/${board}/` : back);
}

// ---------------------------------------------------------------- static + media

const TYPES = {
    ".css": "text/css", ".js": "text/javascript", ".ttf": "font/ttf", ".png": "image/png",
    ".jpg": "image/jpeg", ".gif": "image/gif", ".webp": "image/webp", ".webm": "video/webm", ".mp4": "video/mp4",
};

async function serveFile(req, res, dir, name, cache) {
    if (!/^[\w.-]+$/.test(name) || name.startsWith(".")) return send(res, 404, "not found", "text/plain");
    const file = path.join(dir, name);
    let stat;
    try {
        stat = await fsp.stat(file);
    } catch {
        return send(res, 404, "not found", "text/plain");
    }
    const headers = {
        "Content-Type": TYPES[path.extname(name)] || "application/octet-stream",
        "Cache-Control": cache,
        "Accept-Ranges": "bytes",
        "X-Content-Type-Options": "nosniff",
        "Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'; sandbox",
    };
    const range = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || "");
    if (range && (range[1] || range[2])) {
        let start = range[1] ? Number(range[1]) : stat.size - Number(range[2]);
        let end = range[1] && range[2] ? Number(range[2]) : stat.size - 1;
        start = Math.max(0, start);
        end = Math.min(end, stat.size - 1);
        if (start > end) {
            res.writeHead(416, { "Content-Range": `bytes */${stat.size}` });
            return res.end();
        }
        res.writeHead(206, { ...headers, "Content-Range": `bytes ${start}-${end}/${stat.size}`, "Content-Length": end - start + 1 });
        return fs.createReadStream(file, { start, end }).pipe(res);
    }
    res.writeHead(200, { ...headers, "Content-Length": stat.size });
    if (req.method === "HEAD") return res.end();
    fs.createReadStream(file).pipe(res);
}

// ---------------------------------------------------------------- pages

function boardPage(req, res, board, page) {
    const mod = modSession(req);
    const total = st.countThreads.get(board.id).n;
    const pagesN = Math.max(1, Math.min(cfg.pages, Math.ceil(total / cfg.threadsPerPage)));
    if (page > pagesN) return fail(res, 404, "No such page", "That page is empty.", `${BASE}/${board.id}/`);
    const ops = st.threads.all(board.id, cfg.threadsPerPage, (page - 1) * cfg.threadsPerPage);
    const threads = ops.map((op) => {
        const replies = st.lastReplies.all(board.id, op.no, op.sticky ? 1 : 5);
        const s = st.stats.get(board.id, op.no);
        const hiddenR = s.replies - replies.length;
        const hiddenF = s.images - replies.filter((r) => r.file).length;
        const omitted = hiddenR > 0 ? `${hiddenR} ${hiddenR === 1 ? "reply" : "replies"}${hiddenF > 0 ? ` and ${hiddenF} file${hiddenF === 1 ? "" : "s"}` : ""} not shown.` : "";
        return { op, replies, omitted };
    });
    const opts = { mod: !!mod, csrf: mod?.csrf, backlinks: new Map() };
    send(res, 200, pages.boardIndex(cfg, board, threads, page, pagesN, opts));
}

function threadView(req, res, board, no) {
    const mod = modSession(req);
    const op = st.get.get(board.id, no);
    if (!op) return fail(res, 404, "Thread not found", "It was deleted or fell off the board.", `${BASE}/${board.id}/`);
    if (op.no !== op.thread) return redirect(res, `${BASE}/${board.id}/thread/${op.thread}#p${op.no}`);
    const posts = st.threadPosts.all(board.id, no);
    send(res, 200, pages.threadPage(cfg, board, posts, { mod: !!mod, csrf: mod?.csrf, backlinks: backlinksFor(posts) }));
}

function threadJson(req, res, board, no, after) {
    const mod = modSession(req);
    const op = st.get.get(board.id, no);
    if (!op || op.no !== op.thread) return send(res, 404, JSON.stringify({ gone: true }), "application/json");
    const all = st.threadPosts.all(board.id, no);
    const back = backlinksFor(all);
    const fresh = all.filter((p) => p.no > after);
    send(res, 200, JSON.stringify({
        locked: !!op.locked,
        posts: fresh.map((p) => ({ no: p.no, html: pages.post(p, board, { mod: !!mod, csrf: mod?.csrf, backlinks: back }) })),
    }), "application/json");
}

function catalogView(req, res, board) {
    const ops = st.threads.all(board.id, cfg.threadsPerPage * cfg.pages, 0);
    const threads = ops.map((op) => ({ op, ...st.stats.get(board.id, op.no) }));
    send(res, 200, pages.catalog(cfg, board, threads, !!modSession(req)));
}

// ---------------------------------------------------------------- router

async function route(req, res) {
    const url = new URL(req.url, "http://x");
    let p = url.pathname;
    if (p === BASE) return redirect(res, BASE + "/");
    if (!p.startsWith(BASE + "/")) return send(res, 404, "not found", "text/plain");
    p = p.slice(BASE.length);
    const get = req.method === "GET" || req.method === "HEAD";
    let m;

    if (get && p === "/") return send(res, 200, pages.home(cfg, st.total.get().n));
    if (get && p === "/rules") return send(res, 200, pages.rules(cfg));
    if (get && (m = /^\/static\/(fonts\/)?([\w.-]+)$/.exec(p))) {
        return serveFile(req, res, m[1] ? path.join(STATIC, "fonts") : STATIC, m[2], m[1] ? "public, max-age=31536000, immutable" : "public, max-age=300");
    }
    if (get && (m = /^\/src\/([\w.-]+)$/.exec(p))) return serveFile(req, res, path.join(DATA, "src"), m[1], "public, max-age=31536000, immutable");
    if (get && (m = /^\/thumb\/([\w.-]+)$/.exec(p))) return serveFile(req, res, path.join(DATA, "thumb"), m[1], "public, max-age=31536000, immutable");

    if (get && p === "/captcha") return send(res, 200, JSON.stringify({ id: captcha.create() }), "application/json");
    if (get && (m = /^\/captcha\/([\w-]+)\.png$/.exec(p))) {
        const img = captcha.image(m[1]);
        return img ? send(res, 200, img, "image/png") : send(res, 404, "expired", "text/plain");
    }

    if (p === "/mod" && get) {
        const mod = modSession(req);
        if (!mod) return send(res, 200, pages.modLogin(cfg, url.searchParams.has("wrong")));
        return send(res, 200, pages.modHome(cfg, {
            reports: st.reports.all(), bans: st.bans.all(Date.now()), recent: st.recent.all(30),
            boardsById, csrf: mod.csrf, lookupPost: (b, n) => st.get.get(b, n),
        }));
    }
    if (p === "/mod/login" && req.method === "POST") {
        const f = formFields(await readBody(req, 4096));
        await new Promise((r) => setTimeout(r, 500)); // slows down guessing
        if (!checkPassword(f.password || "")) return redirect(res, `${BASE}/mod?wrong`);
        const token = crypto.randomBytes(24).toString("base64url");
        sessions.set(token, Date.now() + 12 * 3600e3);
        const secure = req.headers["x-forwarded-proto"] === "https" ? "; Secure" : "";
        return redirect(res, `${BASE}/mod`, { "Set-Cookie": `tuxchan_mod=${token}; Path=${BASE}/; HttpOnly; SameSite=Strict; Max-Age=43200${secure}` });
    }
    if (p === "/mod/logout" && req.method === "POST") {
        const mod = modSession(req);
        if (mod) sessions.delete(mod.token);
        return redirect(res, `${BASE}/`, { "Set-Cookie": `tuxchan_mod=; Path=${BASE}/; Max-Age=0` });
    }
    if (p === "/mod/act" && req.method === "POST") return modAction(req, res);

    m = /^\/([a-z0-9]{1,10})(\/.*)?$/.exec(p);
    const board = m && boardsById[m[1]];
    if (!board) return fail(res, 404, "Not found", "There's nothing here.", `${BASE}/`);
    const rest = m[2] || "";

    if (!rest) return redirect(res, `${BASE}/${board.id}/`);
    if (get && rest === "/") return boardPage(req, res, board, 1);
    if (get && (m = /^\/(\d{1,2})$/.exec(rest))) return boardPage(req, res, board, Number(m[1]));
    if (get && rest === "/catalog") return catalogView(req, res, board);
    if (get && (m = /^\/thread\/(\d+)$/.exec(rest))) return threadView(req, res, board, Number(m[1]));
    if (get && (m = /^\/thread\/(\d+)\.json$/.exec(rest))) return threadJson(req, res, board, Number(m[1]), Number(url.searchParams.get("after")) || 0);
    if (get && (m = /^\/preview\/(\d+)$/.exec(rest))) {
        const post = st.get.get(board.id, Number(m[1]));
        return post ? send(res, 200, pages.post(post, board, { preview: true })) : send(res, 404, "gone", "text/plain");
    }
    if (req.method === "POST" && rest === "/post") return newPost(req, res, board);
    if (req.method === "POST" && rest === "/delete") return userDelete(req, res, board);
    if (req.method === "POST" && rest === "/report") return userReport(req, res, board);

    fail(res, 404, "Not found", "There's nothing here.", `${BASE}/${board.id}/`);
}

http.createServer((req, res) => {
    route(req, res).catch((e) => {
        console.error(new Date().toISOString(), req.method, req.url, e);
        if (!res.headersSent) fail(res, 500, "Something broke", "That didn't work. Try again in a bit.");
        else res.end();
    });
}).listen(PORT, "127.0.0.1", () => {
    console.log(`tuxchan on 127.0.0.1:${PORT}, data in ${DATA}`);
    if (!cfg.modHash) console.log("no moderator password yet: node server.js passwd");
});
