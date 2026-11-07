// Backend for sekraj.duckdns.org. Caddy sends every /api/* request here.
// Plain Node, no npm packages. Add routes to the table below.
//
//   node server.js            listens on 127.0.0.1:3000
//   PORT=4000 node server.js

const http = require("http");
const fs = require("fs");
const path = require("path");

const PORT = Number(process.env.PORT) || 3000;
const DATA_DIR = process.env.SEKRAJ_DATA ||
    path.join(process.env.HOME || "/tmp", ".local/share/sekraj");
const DATA_FILE = path.join(DATA_DIR, "data.json");

fs.mkdirSync(DATA_DIR, { recursive: true });

let data = { visits: 0 };
try { data = JSON.parse(fs.readFileSync(DATA_FILE, "utf8")); } catch {}

function save() {
    const tmp = DATA_FILE + ".tmp";
    fs.writeFileSync(tmp, JSON.stringify(data, null, 2));
    fs.renameSync(tmp, DATA_FILE);
}

function send(res, code, obj) {
    const body = JSON.stringify(obj);
    res.writeHead(code, {
        "content-type": "application/json",
        "content-length": Buffer.byteLength(body),
        "cache-control": "no-store",
    });
    res.end(body);
}

function readBody(req, limit = 64 * 1024) {
    return new Promise((ok, fail) => {
        let size = 0;
        const parts = [];
        req.on("data", (c) => {
            size += c.length;
            if (size > limit) { fail(new Error("body too big")); req.destroy(); }
            else parts.push(c);
        });
        req.on("end", () => ok(Buffer.concat(parts).toString("utf8")));
        req.on("error", fail);
    });
}

// "METHOD /path": handler(req, res, url)
const routes = {
    "GET /api/hello": (req, res) => {
        send(res, 200, { hello: "from tux", time: new Date().toISOString() });
    },

    "GET /api/visits": (req, res) => {
        send(res, 200, { visits: data.visits });
    },

    "POST /api/visits": (req, res) => {
        data.visits++;
        save();
        send(res, 200, { visits: data.visits });
    },

    "POST /api/echo": async (req, res) => {
        const body = await readBody(req);
        send(res, 200, { youSent: body });
    },
};

http.createServer(async (req, res) => {
    const url = new URL(req.url, "http://localhost");
    const handler = routes[`${req.method} ${url.pathname}`];
    if (!handler) return send(res, 404, { error: "not found" });
    try {
        await handler(req, res, url);
    } catch (e) {
        console.error(e);
        if (!res.headersSent) send(res, 500, { error: "server error" });
    }
}).listen(PORT, "127.0.0.1", () => {
    console.log(`api on 127.0.0.1:${PORT}, data in ${DATA_FILE}`);
});
