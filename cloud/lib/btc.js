// Bitcoin bits: talks to the local Bitcoin Core node over RPC, converts
// Electrum's zpub into the xpub form Bitcoin Core descriptors want,
// and gets the USD price.
const http = require("http");
const crypto = require("crypto");

const B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

function sha256d(b) {
    return crypto.createHash("sha256").update(crypto.createHash("sha256").update(b).digest()).digest();
}

function b58decode(s) {
    let n = 0n;
    for (const c of s) {
        const i = B58.indexOf(c);
        if (i < 0) throw new Error("not base58");
        n = n * 58n + BigInt(i);
    }
    const hex = n.toString(16);
    let bytes = Buffer.from(hex.length % 2 ? "0" + hex : hex, "hex");
    const zeros = s.match(/^1*/)[0].length;
    if (zeros) bytes = Buffer.concat([Buffer.alloc(zeros), bytes]);
    return bytes;
}

function b58encode(b) {
    let n = BigInt("0x" + (b.toString("hex") || "0"));
    let out = "";
    while (n > 0n) {
        out = B58[Number(n % 58n)] + out;
        n /= 58n;
    }
    for (const byte of b) {
        if (byte !== 0) break;
        out = "1" + out;
    }
    return out;
}

// zpub (BIP84, what Electrum shows for native segwit wallets) -> xpub
function toXpub(key) {
    key = key.replace(/\s+/g, ""); // copying from Electrum can split it over two lines
    const raw = b58decode(key);
    if (raw.length !== 82) throw new Error("that doesn't look like an extended public key");
    const body = raw.subarray(0, 78);
    if (!sha256d(body).subarray(0, 4).equals(raw.subarray(78))) throw new Error("checksum is wrong, check for a typo");
    const version = body.readUInt32BE(0);
    if (version === 0x0488b21e) return { xpub: key, script: "unknown" };
    if (version !== 0x04b24746) throw new Error("expected a zpub (native segwit) key from Electrum");
    const x = Buffer.from(body);
    x.writeUInt32BE(0x0488b21e, 0);
    return { xpub: b58encode(Buffer.concat([x, sha256d(x).subarray(0, 4)])), script: "wpkh" };
}

function makeRpc(cfg) {
    const auth = "Basic " + Buffer.from(`${cfg.rpcUser}:${cfg.rpcPassword}`).toString("base64");
    return function rpc(method, params = [], wallet = null) {
        const body = JSON.stringify({ jsonrpc: "2.0", id: 1, method, params });
        return new Promise((ok, bad) => {
            const req = http.request({
                host: "127.0.0.1",
                port: cfg.rpcPort || 8332,
                path: wallet ? `/wallet/${encodeURIComponent(wallet)}` : "/",
                method: "POST",
                headers: { "Content-Type": "application/json", Authorization: auth, "Content-Length": Buffer.byteLength(body) },
                timeout: 15000,
            }, (res) => {
                let s = "";
                res.on("data", (c) => (s += c));
                res.on("end", () => {
                    let j;
                    try {
                        j = JSON.parse(s);
                    } catch {
                        return bad(new Error(`bitcoind answered ${res.statusCode}`));
                    }
                    if (j.error) bad(Object.assign(new Error(j.error.message), { rpcCode: j.error.code }));
                    else ok(j.result);
                });
            });
            req.on("timeout", () => req.destroy(new Error("bitcoind timed out")));
            req.on("error", bad);
            req.end(body);
        });
    };
}

let priceCache = { usd: 0, at: 0 };

async function fetchJson(url) {
    const r = await fetch(url, { signal: AbortSignal.timeout(8000), headers: { "User-Agent": "sekcloud" } });
    if (!r.ok) throw new Error(url + " -> " + r.status);
    return r.json();
}

// USD per bitcoin, cached for 10 minutes
async function btcPrice() {
    if (priceCache.usd && Date.now() - priceCache.at < 600e3) return priceCache.usd;
    let usd = 0;
    try {
        usd = Number((await fetchJson("https://mempool.space/api/v1/prices")).USD);
    } catch {
        try {
            const j = await fetchJson("https://api.kraken.com/0/public/Ticker?pair=XBTUSD");
            usd = Number(Object.values(j.result)[0].c[0]);
        } catch {
            usd = 0;
        }
    }
    if (usd > 1000) priceCache = { usd, at: Date.now() };
    if (!priceCache.usd) throw new Error("couldn't get the bitcoin price");
    return priceCache.usd;
}

function satsFor(usd, price) {
    return Math.ceil((usd / price) * 1e8);
}

function btcString(sat) {
    return (sat / 1e8).toFixed(8);
}

module.exports = { toXpub, makeRpc, btcPrice, satsFor, btcString };
