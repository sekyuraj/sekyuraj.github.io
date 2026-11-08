// Picture captcha drawn from a tiny bitmap font straight into a PNG,
// so the answer never appears as text anywhere in the page.
const crypto = require("crypto");
const zlib = require("zlib");

const GLYPHS = {
    A: [" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
    C: [" ####", "#    ", "#    ", "#    ", "#    ", "#    ", " ####"],
    D: ["#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "],
    E: ["#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"],
    F: ["#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "],
    H: ["#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
    J: ["  ###", "   # ", "   # ", "   # ", "#  # ", "#  # ", " ##  "],
    K: ["#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"],
    L: ["#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"],
    M: ["#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"],
    N: ["#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"],
    P: ["#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "],
    R: ["#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"],
    T: ["#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "],
    U: ["#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
    V: ["#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "],
    W: ["#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"],
    X: ["#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"],
    Y: ["#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "],
    3: ["#### ", "    #", "    #", " ### ", "    #", "    #", "#### "],
    4: ["#   #", "#   #", "#   #", "#####", "    #", "    #", "    #"],
    6: [" ### ", "#    ", "#    ", "#### ", "#   #", "#   #", " ### "],
    7: ["#####", "    #", "   # ", "  #  ", "  #  ", "  #  ", "  #  "],
    9: [" ### ", "#   #", "#   #", " ####", "    #", "    #", " ### "],
};
const CHARS = Object.keys(GLYPHS).join("");
const W = 220, H = 70;

const store = new Map(); // id -> { answer, exp }

function rnd(n) {
    return crypto.randomInt(n);
}

function crcTable() {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        t[n] = c >>> 0;
    }
    return t;
}
const CRC = crcTable();
function crc32(buf) {
    let c = 0xffffffff;
    for (const b of buf) c = CRC[(c ^ b) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type, "latin1"), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
}

function png(pixels) {
    const raw = Buffer.alloc((W + 1) * H);
    for (let y = 0; y < H; y++) {
        raw[y * (W + 1)] = 0;
        pixels.copy(raw, y * (W + 1) + 1, y * W, (y + 1) * W);
    }
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(W, 0);
    ihdr.writeUInt32BE(H, 4);
    ihdr[8] = 8; // bit depth
    ihdr[9] = 0; // greyscale
    return Buffer.concat([
        Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
        chunk("IHDR", ihdr),
        chunk("IDAT", zlib.deflateSync(raw)),
        chunk("IEND", Buffer.alloc(0)),
    ]);
}

function draw(answer) {
    const px = Buffer.alloc(W * H, 235);
    const dot = (x, y, v) => {
        x = Math.round(x);
        y = Math.round(y);
        if (x >= 0 && y >= 0 && x < W && y < H) px[y * W + x] = v;
    };
    for (let i = 0; i < 500; i++) dot(rnd(W), rnd(H), 120 + rnd(100));

    let x = 14 + rnd(10);
    for (const ch of answer) {
        const g = GLYPHS[ch];
        const s = 5 + rnd(2);           // pixel size
        const y0 = 6 + rnd(H - 7 * s - 10);
        const shear = (rnd(9) - 4) / 10;
        const v = 20 + rnd(60);
        for (let gy = 0; gy < 7; gy++) {
            for (let gx = 0; gx < 5; gx++) {
                if (g[gy][gx] !== "#") continue;
                for (let dy = 0; dy < s; dy++) {
                    for (let dx = 0; dx < s; dx++) {
                        const yy = gy * s + dy;
                        dot(x + gx * s + dx + shear * (yy - 3.5 * s), y0 + yy, v);
                    }
                }
            }
        }
        x += 5 * s + 8 + rnd(8);
    }

    for (let l = 0; l < 3; l++) {
        let y = 10 + rnd(H - 20);
        const slope = (rnd(21) - 10) / 40;
        for (let xx = 0; xx < W; xx++) {
            y += slope;
            dot(xx, y, 40 + rnd(40));
            dot(xx, y + 1, 40 + rnd(40));
        }
    }
    return png(px);
}

function create() {
    const now = Date.now();
    if (store.size > 20000) {
        for (const [id, c] of store) if (c.exp < now || store.size > 15000) store.delete(id);
    }
    let answer = "";
    for (let i = 0; i < 5; i++) answer += CHARS[rnd(CHARS.length)];
    const id = crypto.randomBytes(12).toString("base64url");
    store.set(id, { answer, exp: now + 10 * 60 * 1000, image: null });
    return id;
}

function image(id) {
    const c = store.get(id);
    if (!c || c.exp < Date.now()) return null;
    c.image ??= draw(c.answer);
    return c.image;
}

// each captcha works once, right or wrong
function check(id, guess) {
    const c = store.get(id);
    store.delete(id);
    if (!c || c.exp < Date.now()) return false;
    return String(guess || "").toUpperCase().replace(/\s/g, "") === c.answer;
}

module.exports = { create, image, check };
