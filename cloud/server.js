// sekcloud: paid file storage at sekraj.duckdns.org/cloud/
// Accounts cost a one-time payment in bitcoin, checked by the local
// Bitcoin Core node against a watch-only wallet (it can't spend anything).
//
//   node server.js                    run (127.0.0.1:3002)
//   node server.js setup-wallet ZPUB  connect the Electrum wallet

const http = require("http");
const fs = require("fs");
const fsp = require("fs/promises");
const path = require("path");
const crypto = require("crypto");
const { execFile } = require("child_process");

const { open } = require("./lib/db");
const btc = require("./lib/btc");
const pages = require("./lib/pages");

const BASE = "/cloud";
const PORT = Number(process.env.PORT) || 3002;
const DATA = process.env.SEKCLOUD_DATA || path.join(process.env.HOME || "/tmp", ".local/share/sekcloud");
const STATIC = path.join(__dirname, "static");
const ADDRESS_RANGE = 2000; // addresses imported into the watch-only wallet

fs.mkdirSync(path.join(DATA, "files"), { recursive: true, mode: 0o700 });
fs.mkdirSync(path.join(DATA, "tmp"), { recursive: true, mode: 0o700 });

const CFG_FILE = path.join(DATA, "config.json");
const DEFAULTS = {
    priceUSD: 10,
    quotaGB: 20,
    invoicesPerIpPerDay: 5,
    rpcUser: "",
    rpcPassword: "",
    rpcPort: 8332,
    wallet: "sekcloud",
    descriptor: "",
};
const cfg = { ...DEFAULTS, ...(fs.existsSync(CFG_FILE) ? JSON.parse(fs.readFileSync(CFG_FILE, "utf8")) : {}) };
const saveCfg = () => fs.writeFileSync(CFG_FILE, JSON.stringify(cfg, null, 2) + "\n", { mode: 0o600 });
if (!fs.existsSync(CFG_FILE)) saveCfg();

const SECRET_FILE = path.join(DATA, "secret");
if (!fs.existsSync(SECRET_FILE)) fs.writeFileSync(SECRET_FILE, crypto.randomBytes(32).toString("hex"), { mode: 0o600 });
const SECRET = fs.readFileSync(SECRET_FILE, "utf8").trim();

const rpc = btc.makeRpc(cfg);
const QUOTA = cfg.quotaGB * 1024 ** 3;

if (process.argv[2] === "setup-wallet") {
    setupWallet(process.argv[3]).then(
        () => process.exit(0),
        (e) => { console.error("error:", e.message); process.exit(1); },
    );
    return;
}

const { st } = open(DATA);

// node server.js add-user NAME   free account for the owner, prints a random password
if (process.argv[2] === "add-user") {
    const name = process.argv[3] || "";
    if (!/^[A-Za-z0-9_.-]{3,32}$/.test(name)) {
        console.error("usage: node server.js add-user NAME (3-32 letters, numbers, . _ -)");
        process.exit(1);
    }
    if (st.userByName.get(name)) {
        console.error("that username is taken");
        process.exit(1);
    }
    const pass = crypto.randomBytes(12).toString("base64url");
    const id = st.addUser.get(name, hashPass(pass), Date.now(), QUOTA).id;
    fs.mkdirSync(path.join(DATA, "files", String(id)), { recursive: true, mode: 0o700 });
    console.log(`account ${name} created, password: ${pass}`);
    process.exit(0);
}

// ---------------------------------------------------------------- wallet setup

async function setupWallet(key) {
    if (!key) throw new Error("usage: node server.js setup-wallet <zpub from Electrum>");
    const { xpub } = btc.toXpub(key);
    const desc = `wpkh(${xpub}/0/*)`;
    const info = await rpc("getdescriptorinfo", [desc]);
    const full = `${desc}#${info.checksum}`;

    const loaded = await rpc("listwallets");
    if (!loaded.includes(cfg.wallet)) {
        try {
            await rpc("loadwallet", [cfg.wallet, true]);
        } catch {
            // createwallet name disable_private_keys blank passphrase avoid_reuse descriptors load_on_startup
            await rpc("createwallet", [cfg.wallet, true, true, "", false, true, true]);
        }
    }
    const res = await rpc("importdescriptors", [[{ desc: full, timestamp: "now", range: [0, ADDRESS_RANGE - 1], watchonly: true }]], cfg.wallet);
    if (!res[0].success) throw new Error("import failed: " + JSON.stringify(res[0].error));
    const first = (await rpc("deriveaddresses", [full, [0, 0]]))[0];
    cfg.descriptor = full;
    saveCfg();
    console.log("watch-only wallet ready.");
    console.log("First receive address:", first);
    console.log("It should match the first address in Electrum's Addresses tab.");
}

// ---------------------------------------------------------------- helpers

const hmac = (s) => crypto.createHmac("sha256", SECRET).update(s).digest("base64url");
const sha = (s) => crypto.createHash("sha256").update(s).digest("hex");

function clientIp(req) {
    const peer = req.socket.remoteAddress || "";
    const local = peer === "127.0.0.1" || peer === "::1" || peer === "::ffff:127.0.0.1";
    const xff = req.headers["x-forwarded-for"];
    return local && xff ? xff.split(",").pop().trim() : peer;
}

const SECURITY = {
    "Content-Security-Policy": "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'",
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "same-origin",
};

function send(res, code, body, type = "text/html; charset=utf-8", extra = {}) {
    res.writeHead(code, { "Content-Type": type, "Cache-Control": "no-store", ...SECURITY, ...extra });
    res.end(body);
}
const json = (res, code, obj) => send(res, code, JSON.stringify(obj), "application/json");
const redirect = (res, to, extra = {}) => {
    res.writeHead(303, { Location: to, "Cache-Control": "no-store", ...extra });
    res.end();
};

function readBody(req, limit) {
    return new Promise((ok, bad) => {
        const parts = [];
        let size = 0;
        req.on("data", (c) => {
            size += c.length;
            if (size > limit) {
                bad(new Error("too big"));
                req.destroy();
            } else parts.push(c);
        });
        req.on("end", () => ok(Buffer.concat(parts).toString("utf8")));
        req.on("error", bad);
    });
}
const formBody = async (req) => Object.fromEntries(new URLSearchParams(await readBody(req, 16384)));
async function jsonBody(req) {
    try {
        return JSON.parse(await readBody(req, 16384));
    } catch {
        return {};
    }
}

function cookies(req) {
    const out = {};
    for (const part of (req.headers.cookie || "").split(";")) {
        const i = part.indexOf("=");
        if (i > 0) out[part.slice(0, i).trim()] = decodeURIComponent(part.slice(i + 1).trim());
    }
    return out;
}

function cookie(req, name, value, maxAge) {
    const secure = req.headers["x-forwarded-proto"] === "https" ? "; Secure" : "";
    return `${name}=${value}; Path=${BASE}/; HttpOnly; SameSite=Lax; Max-Age=${maxAge}${secure}`;
}

function size(n) {
    if (n >= 1024 ** 3) return (n / 1024 ** 3).toFixed(2) + " GB";
    if (n >= 1024 ** 2) return (n / 1024 ** 2).toFixed(1) + " MB";
    if (n >= 1024) return Math.round(n / 1024) + " KB";
    return n + " B";
}

// ---------------------------------------------------------------- passwords + sessions

function hashPass(pass) {
    const salt = crypto.randomBytes(16).toString("hex");
    return salt + ":" + crypto.scryptSync(pass, salt, 32).toString("hex");
}

function checkPass(pass, stored) {
    const [salt, hash] = stored.split(":");
    return crypto.timingSafeEqual(crypto.scryptSync(pass, salt, 32), Buffer.from(hash, "hex"));
}

function startSession(req, res, userId, to) {
    const token = crypto.randomBytes(32).toString("base64url");
    const now = Date.now();
    st.addSession.run(sha(token), userId, now, now + 30 * 86400e3);
    redirect(res, to, { "Set-Cookie": cookie(req, "sc_sess", token, 30 * 86400) });
}

function session(req) {
    const t = cookies(req).sc_sess;
    if (!t) return null;
    const s = st.session.get(sha(t), Date.now());
    if (!s) return null;
    const user = st.userById.get(s.user_id);
    return user ? { user, hash: s.hash, csrf: hmac("csrf:" + s.hash) } : null;
}

const loginFails = new Map(); // iphash -> [times]
function tooManyFails(iphash) {
    const recent = (loginFails.get(iphash) || []).filter((t) => t > Date.now() - 15 * 60e3);
    loginFails.set(iphash, recent);
    return recent.length >= 10;
}

// ---------------------------------------------------------------- invoices

let nodeState = { at: 0, ok: false, ibd: true, progress: 0 };
async function node() {
    if (Date.now() - nodeState.at < 30e3) return nodeState;
    try {
        const info = await rpc("getblockchaininfo");
        const wallets = await rpc("listwallets");
        if (!wallets.includes(cfg.wallet)) await rpc("loadwallet", [cfg.wallet]).catch(() => {});
        nodeState = { at: Date.now(), ok: true, ibd: info.initialblockdownload, progress: info.verificationprogress };
    } catch {
        nodeState = { at: Date.now(), ok: false, ibd: true, progress: 0 };
    }
    return nodeState;
}

const receivedSat = async (address, minconf) => Math.round((await rpc("getreceivedbyaddress", [address, minconf], cfg.wallet)) * 1e8);

let invoiceLock = Promise.resolve();
function newInvoice(iphash) {
    const job = invoiceLock.then(async () => {
        const price = await btc.btcPrice();
        const now = Date.now();
        let index = null;
        let address = null;
        // an address handed out a week ago that never got anything can be used again
        for (const old of st.staleInvoices.all(now - 7 * 86400e3)) {
            if ((await receivedSat(old.address, 0)) === 0) {
                st.markExpired.run(old.id);
                index = old.addr_index;
                address = old.address;
                break;
            }
        }
        if (index === null) {
            index = (st.maxIndex.get().m ?? -1) + 1;
            if (index >= ADDRESS_RANGE) throw new Error("out of payment addresses, run setup-wallet with a bigger range");
            address = (await rpc("deriveaddresses", [cfg.descriptor, [index, index]]))[0];
        }
        const code = crypto.randomBytes(18).toString("base64url");
        st.addInvoice.run(sha(code), index, address, btc.satsFor(cfg.priceUSD, price), now, now, iphash);
        return code;
    });
    invoiceLock = job.catch(() => {});
    return job;
}

const statusCache = new Map(); // invoice id -> { at, value }
async function invoiceStatus(inv) {
    if (inv.status !== "open") return { state: inv.status };
    const hit = statusCache.get(inv.id);
    if (hit && Date.now() - hit.at < 5000) return hit.value;

    const [seen, confirmed] = await Promise.all([receivedSat(inv.address, 0), receivedSat(inv.address, 1)]);
    const need = Math.floor(inv.amount_sat * 0.99); // a little slack for rounding in wallets
    let value;
    if (confirmed >= need) {
        st.markPaid.run(Date.now(), inv.id);
        value = { state: "paid" };
    } else if (seen >= need) {
        value = { state: "seen" };
    } else if (seen > 0) {
        value = { state: "partial", missing: btc.btcString(inv.amount_sat - seen) };
    } else {
        // nothing paid yet: keep the price fresh
        if (Date.now() - inv.quoted_at > 24 * 3600e3) {
            const amount = btc.satsFor(cfg.priceUSD, await btc.btcPrice());
            st.requote.run(amount, Date.now(), inv.id);
            value = { state: "requoted" };
        } else value = { state: "waiting" };
    }
    statusCache.set(inv.id, { at: Date.now(), value });
    if (statusCache.size > 5000) statusCache.clear();
    return value;
}

const qrCache = new Map();
function qrSvg(text) {
    if (qrCache.has(text)) return Promise.resolve(qrCache.get(text));
    return new Promise((ok, bad) => {
        execFile("qrencode", ["-t", "SVG", "--svg-path", "-l", "M", "-m", "2", "-s", "6", "-o", "-", text], { timeout: 5000 }, (err, out) => {
            if (err) return bad(err);
            qrCache.set(text, out);
            if (qrCache.size > 500) qrCache.clear();
            ok(out);
        });
    });
}

// ---------------------------------------------------------------- files

const userRoot = (id) => path.join(DATA, "files", String(id));

function validName(n) {
    return typeof n === "string" && n.length > 0 && Buffer.byteLength(n) <= 255 && n !== "." && n !== ".." && !/[\/\0]/.test(n);
}

function resolve(user, p) {
    if (typeof p !== "string" || !p.startsWith("/")) return null;
    const segs = p.split("/").filter(Boolean);
    if (!segs.every(validName)) return null;
    const root = userRoot(user.id);
    const full = path.join(root, ...segs);
    if (full !== root && !full.startsWith(root + path.sep)) return null;
    return { full, rel: "/" + segs.join("/"), root: full === root };
}

async function treeSize(full) {
    const s = await fsp.lstat(full);
    if (!s.isDirectory()) return s.isFile() ? s.size : 0;
    let total = 0;
    for (const e of await fsp.readdir(full)) total += await treeSize(path.join(full, e));
    return total;
}

const likeEsc = (s) => s.replace(/[\\%_]/g, (c) => "\\" + c);

async function freeName(dir, name) {
    const ext = path.extname(name);
    const stem = name.slice(0, name.length - ext.length);
    for (let i = 0; i < 1000; i++) {
        const n = i ? `${stem} (${i})${ext}` : name;
        try {
            await fsp.lstat(path.join(dir, n));
        } catch {
            return n;
        }
    }
    throw new Error("too many files with that name");
}

const MIME = {
    ".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".gif": "image/gif", ".webp": "image/webp",
    ".avif": "image/avif", ".mp4": "video/mp4", ".webm": "video/webm", ".mkv": "video/x-matroska", ".mov": "video/quicktime",
    ".mp3": "audio/mpeg", ".ogg": "audio/ogg", ".opus": "audio/ogg", ".flac": "audio/flac", ".wav": "audio/wav", ".m4a": "audio/mp4",
    ".txt": "text/plain; charset=utf-8", ".md": "text/plain; charset=utf-8", ".log": "text/plain; charset=utf-8",
    ".pdf": "application/pdf", ".zip": "application/zip",
};
// only types a browser can't run code from are shown inline
const INLINE = /^(image\/(png|jpeg|gif|webp|avif)|video\/(mp4|webm)|audio\/|text\/plain)/;

async function sendFile(req, res, full, name, inline) {
    let stat;
    try {
        stat = await fsp.stat(full);
    } catch {
        return send(res, 404, pages.message("Not found", "That file doesn't exist anymore."));
    }
    if (!stat.isFile()) return send(res, 404, pages.message("Not found", "That's not a file."));
    const type = MIME[path.extname(name).toLowerCase()] || "application/octet-stream";
    const show = inline && INLINE.test(type);
    const headers = {
        "Content-Type": type,
        "Content-Disposition": `${show ? "inline" : "attachment"}; filename*=UTF-8''${encodeURIComponent(name)}`,
        "Accept-Ranges": "bytes",
        "Cache-Control": "private, no-store",
        "X-Content-Type-Options": "nosniff",
        "Content-Security-Policy": "default-src 'none'; img-src 'self'; media-src 'self'; style-src 'unsafe-inline'; sandbox",
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
        return fs.createReadStream(full, { start, end }).pipe(res);
    }
    res.writeHead(200, { ...headers, "Content-Length": stat.size });
    if (req.method === "HEAD") return res.end();
    fs.createReadStream(full).pipe(res);
}

async function upload(req, res, s, url) {
    const dir = resolve(s.user, url.searchParams.get("dir") || "/");
    const name = url.searchParams.get("name");
    if (!dir || !validName(name)) return json(res, 400, { error: "bad name" });
    try {
        if (!(await fsp.stat(dir.full)).isDirectory()) throw new Error();
    } catch {
        return json(res, 404, { error: "that folder is gone" });
    }
    const declared = Number(req.headers["content-length"]);
    const user = st.userById.get(s.user.id);
    const room = user.quota - user.used;
    if (!Number.isFinite(declared)) return json(res, 411, { error: "length required" });
    if (declared > room) return json(res, 413, { error: `not enough space (${size(Math.max(0, room))} left)` });

    const tmp = path.join(DATA, "tmp", crypto.randomBytes(12).toString("hex"));
    const out = fs.createWriteStream(tmp, { mode: 0o600 });
    let got = 0;
    let failed = false;
    await new Promise((done) => {
        req.on("data", (c) => {
            got += c.length;
            if (got > declared && !failed) stop();
        });
        const stop = () => {
            failed = true;
            req.unpipe(out);
            out.destroy();
            req.destroy();
        };
        req.on("aborted", stop);
        req.on("error", stop);
        out.on("close", done);
        out.on("error", () => {
            failed = true;
            done();
        });
        req.pipe(out);
    });
    if (failed || got !== declared) {
        await fsp.rm(tmp, { force: true });
        if (!res.headersSent && !res.destroyed) json(res, 400, { error: "upload was cut off" });
        return;
    }
    const final = await freeName(dir.full, name);
    await fsp.rename(tmp, path.join(dir.full, final));
    st.addUsed.run(got, s.user.id);
    json(res, 200, { name: final });
}

async function list(res, s, p) {
    const dir = resolve(s.user, p);
    if (!dir) return json(res, 400, { error: "bad path" });
    let entries;
    try {
        entries = await fsp.readdir(dir.full, { withFileTypes: true });
    } catch {
        return json(res, 404, { error: "that folder doesn't exist" });
    }
    const items = [];
    for (const e of entries) {
        if (!e.isFile() && !e.isDirectory()) continue;
        const stt = await fsp.stat(path.join(dir.full, e.name)).catch(() => null);
        if (stt) items.push({ name: e.name, dir: e.isDirectory(), size: e.isDirectory() ? null : stt.size, mtime: stt.mtimeMs });
    }
    items.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: "base" }));
    const u = st.userById.get(s.user.id);
    json(res, 200, { path: dir.rel, items, used: u.used, quota: u.quota });
}

async function api(req, res, s, url, name) {
    if (req.method === "GET" && name === "list") return list(res, s, url.searchParams.get("path") || "/");
    if (req.method === "GET" && name === "shares") {
        const rows = st.sharesOf.all(s.user.id);
        return json(res, 200, { shares: rows.map((r) => ({ token: r.token, path: r.path, downloads: r.downloads, url: `${BASE}/s/${r.token}` })) });
    }
    if (req.headers["x-csrf"] !== s.csrf) return json(res, 403, { error: "reload the page and try again" });
    if (req.method === "PUT" && name === "upload") return upload(req, res, s, url);
    if (req.method !== "POST") return json(res, 405, { error: "bad method" });

    const b = await jsonBody(req);
    if (name === "mkdir") {
        const parent = resolve(s.user, b.path);
        if (!parent || !validName(b.name)) return json(res, 400, { error: "that name isn't allowed" });
        try {
            await fsp.mkdir(path.join(parent.full, b.name));
        } catch (e) {
            return json(res, 409, { error: e.code === "EEXIST" ? "something with that name exists already" : "couldn't make the folder" });
        }
        return json(res, 200, {});
    }
    if (name === "delete") {
        const t = resolve(s.user, b.path);
        if (!t || t.root) return json(res, 400, { error: "bad path" });
        const bytes = await treeSize(t.full).catch(() => 0);
        await fsp.rm(t.full, { recursive: true, force: true });
        st.addUsed.run(-bytes, s.user.id);
        st.dropSharesUnder.run(s.user.id, t.rel, likeEsc(t.rel) + "/%");
        return json(res, 200, {});
    }
    if (name === "rename") {
        const t = resolve(s.user, b.path);
        if (!t || t.root || !validName(b.name)) return json(res, 400, { error: "that name isn't allowed" });
        const dest = path.join(path.dirname(t.full), b.name);
        try {
            await fsp.lstat(dest);
            return json(res, 409, { error: "something with that name exists already" });
        } catch {}
        await fsp.rename(t.full, dest);
        const newRel = path.posix.join(path.posix.dirname(t.rel), b.name);
        st.moveShares.run(newRel, t.rel.length + 1, s.user.id, t.rel, likeEsc(t.rel) + "/%");
        return json(res, 200, {});
    }
    if (name === "share") {
        const t = resolve(s.user, b.path);
        const stt = t && (await fsp.stat(t.full).catch(() => null));
        if (!stt || !stt.isFile()) return json(res, 400, { error: "only files can be shared" });
        const token = crypto.randomBytes(16).toString("base64url");
        st.addShare.run(token, s.user.id, t.rel, Date.now());
        return json(res, 200, { url: `${BASE}/s/${token}` });
    }
    if (name === "unshare") {
        st.dropShare.run(String(b.token), s.user.id);
        return json(res, 200, {});
    }
    if (name === "password") {
        const u = st.userById.get(s.user.id);
        if (!checkPass(String(b.old || ""), u.pass)) return json(res, 403, { error: "the current password is wrong" });
        if (String(b.password || "").length < 10) return json(res, 400, { error: "use at least 10 characters" });
        st.setPass.run(hashPass(b.password), u.id);
        st.dropUserSessions.run(u.id, s.hash);
        return json(res, 200, {});
    }
    json(res, 404, { error: "unknown action" });
}

// ---------------------------------------------------------------- signup + login

async function signupPage(req, res, url, error, username) {
    const n = await node();
    if (!cfg.descriptor) return send(res, 503, pages.signupClosed("Signups aren't open yet. The payment wallet isn't set up."));
    if (!n.ok) return send(res, 503, pages.signupClosed("Signups are paused right now. Try again later."));
    if (n.ibd) return send(res, 503, pages.signupClosed(`Signups open soon. The payment node is still syncing (${Math.floor(n.progress * 100)}%).`));

    const iphash = hmac("ip:" + clientIp(req));
    let code = url.searchParams.get("code") || cookies(req).sc_inv;
    let inv = code && st.invoice.get(sha(code));
    if (!inv || inv.status === "expired" || inv.status === "used") {
        if (st.invoicesFromIp.get(iphash, Date.now() - 86400e3).n >= cfg.invoicesPerIpPerDay) {
            return send(res, 429, pages.signupClosed("Too many signups from your connection today. Try again tomorrow."));
        }
        try {
            code = await newInvoice(iphash);
        } catch (e) {
            console.error("invoice:", e.message);
            return send(res, 503, pages.signupClosed("Couldn't make a payment address right now. Try again in a few minutes."));
        }
        inv = st.invoice.get(sha(code));
    }
    await invoiceStatus(inv).catch(() => {});
    inv = st.invoice.get(sha(code));
    send(res, 200, pages.signup({
        inv, code, error, username,
        btc: btc.btcString(inv.amount_sat), usd: cfg.priceUSD, quota: `${cfg.quotaGB} GB`,
    }), "text/html; charset=utf-8", { "Set-Cookie": cookie(req, "sc_inv", code, 7 * 86400) });
}

async function signupPost(req, res) {
    const f = await formBody(req);
    const inv = f.code && st.invoice.get(sha(f.code));
    const again = (msg) => {
        const url = new URL(`http://x${BASE}/signup`);
        if (f.code) url.searchParams.set("code", f.code);
        return signupPage(req, res, url, msg, f.username);
    };
    if (!inv) return again("That payment code doesn't exist. Start again.");
    const status = await invoiceStatus(inv).catch(() => ({ state: "error" }));
    if (status.state === "used") return send(res, 400, pages.signupClosed("That payment was already used for an account. Log in instead."));
    if (status.state !== "paid") return again("Your payment hasn't been confirmed yet. Wait for one confirmation, then try again.");
    const username = String(f.username || "").trim();
    if (!/^[A-Za-z0-9_.-]{3,32}$/.test(username)) return again("Usernames are 3 to 32 letters, numbers, dots, dashes or underscores.");
    if (String(f.password || "").length < 10) return again("Use a password with at least 10 characters.");
    if (f.password !== f.password2) return again("The two passwords don't match.");
    if (st.userByName.get(username)) return again("That username is taken.");

    const id = st.addUser.get(username, hashPass(f.password), Date.now(), QUOTA).id;
    if (st.markUsed.run(id, inv.id).changes !== 1) {
        return again("That payment was already used for an account.");
    }
    await fsp.mkdir(userRoot(id), { recursive: true, mode: 0o700 });
    redirect(res, `${BASE}/login?welcome`, { "Set-Cookie": cookie(req, "sc_inv", "", 0) });
}

async function loginPost(req, res) {
    const f = await formBody(req);
    const iphash = hmac("ip:" + clientIp(req));
    if (tooManyFails(iphash)) return send(res, 429, pages.login({ error: "Too many wrong tries. Wait 15 minutes.", username: f.username }));
    const u = st.userByName.get(String(f.username || "").trim());
    await new Promise((r) => setTimeout(r, 300));
    if (!u || !checkPass(String(f.password || ""), u.pass)) {
        loginFails.get(iphash).push(Date.now());
        return send(res, 403, pages.login({ error: "Wrong username or password.", username: f.username }));
    }
    startSession(req, res, u.id, `${BASE}/`);
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

    if (get && (m = /^\/static\/([\w.-]+)$/.exec(p))) {
        const type = { ".css": "text/css", ".js": "text/javascript" }[path.extname(m[1])];
        if (!type) return send(res, 404, "not found", "text/plain");
        try {
            return send(res, 200, await fsp.readFile(path.join(STATIC, m[1])), type, { "Cache-Control": "public, max-age=300" });
        } catch {
            return send(res, 404, "not found", "text/plain");
        }
    }

    if (get && (m = /^\/s\/([\w-]{10,40})(\/dl)?$/.exec(p))) {
        const sh = st.share.get(m[1]);
        const owner = sh && st.userById.get(sh.user_id);
        const t = owner && resolve(owner, sh.path);
        const stt = t && (await fsp.stat(t.full).catch(() => null));
        if (!stt || !stt.isFile()) return send(res, 404, pages.message("Link not found", "This link was removed or the file is gone."));
        const name = path.basename(t.full);
        if (!m[2]) return send(res, 200, pages.sharePage({ name, size: size(stt.size), token: m[1] }));
        st.countDownload.run(m[1]);
        return sendFile(req, res, t.full, name, false);
    }

    if (get && (m = /^\/qr\/([\w-]+)\.svg$/.exec(p))) {
        const inv = st.invoice.get(sha(m[1]));
        if (!inv || inv.status !== "open") return send(res, 404, "", "text/plain");
        const svg = await qrSvg(`bitcoin:${inv.address}?amount=${btc.btcString(inv.amount_sat)}`);
        return send(res, 200, svg, "image/svg+xml", { "Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'" });
    }

    if (p === "/login" && get) {
        if (session(req)) return redirect(res, `${BASE}/`);
        return send(res, 200, pages.login({ signedUp: url.searchParams.has("welcome") }));
    }
    if (p === "/login" && req.method === "POST") return loginPost(req, res);
    if (p === "/signup" && get) return signupPage(req, res, url);
    if (p === "/signup" && req.method === "POST") return signupPost(req, res);
    if (p === "/invoice" && get) {
        const code = url.searchParams.get("code");
        const inv = code && st.invoice.get(sha(code));
        if (!inv) return json(res, 404, { state: "missing" });
        try {
            const s = await invoiceStatus(inv);
            const fresh = st.invoice.get(sha(code));
            return json(res, 200, { ...s, btc: btc.btcString(fresh.amount_sat) });
        } catch {
            return json(res, 200, { state: "unknown" });
        }
    }

    const s = session(req);
    if (!s) {
        if (p.startsWith("/api/")) return json(res, 401, { error: "you're logged out, reload the page" });
        return redirect(res, `${BASE}/login`);
    }
    if (p === "/" && get) return send(res, 200, pages.app({ username: s.user.username, csrf: s.csrf }));
    if (p === "/logout" && req.method === "POST") {
        const f = await formBody(req);
        if (f.csrf === s.csrf) st.dropSession.run(s.hash);
        return redirect(res, `${BASE}/login`, { "Set-Cookie": cookie(req, "sc_sess", "", 0) });
    }
    if (get && p === "/dl") {
        const t = resolve(s.user, url.searchParams.get("path") || "");
        if (!t || t.root) return send(res, 400, pages.message("Bad link", "That path isn't valid."));
        return sendFile(req, res, t.full, path.basename(t.full), url.searchParams.has("view"));
    }
    if ((m = /^\/api\/(\w+)$/.exec(p))) return api(req, res, s, url, m[1]);
    send(res, 404, pages.message("Not found", "There's nothing here."));
}

// ---------------------------------------------------------------- start

async function recountUsage() {
    for (const { id } of st.users.all()) {
        const used = await treeSize(userRoot(id)).catch(() => 0);
        st.setUsed.run(used, id);
    }
}

for (const f of fs.readdirSync(path.join(DATA, "tmp"))) fs.rmSync(path.join(DATA, "tmp", f), { force: true });
st.cleanSessions.run(Date.now());
recountUsage().catch((e) => console.error("recount:", e));

const server = http.createServer((req, res) => {
    route(req, res).catch((e) => {
        console.error(new Date().toISOString(), req.method, req.url, e);
        if (!res.headersSent) send(res, 500, pages.message("Something broke", "That didn't work. Try again in a bit."));
        else res.end();
    });
});
server.requestTimeout = 0; // big uploads on slow connections take longer than the 5 minute default
server.listen(PORT, "127.0.0.1", () => {
    console.log(`sekcloud on 127.0.0.1:${PORT}, data in ${DATA}`);
    if (!cfg.descriptor) console.log("no wallet yet: node server.js setup-wallet <zpub>");
});
