// Upload handling: sniff the real type from the bytes, strip metadata
// (camera EXIF can contain GPS), make a thumbnail with ffmpeg.
const { execFile } = require("child_process");

const TYPES = [
    { mime: "image/jpeg", ext: "jpg", demux: "jpeg_pipe", test: (b) => b[0] === 0xff && b[1] === 0xd8 && b[2] === 0xff },
    { mime: "image/png", ext: "png", demux: "png_pipe", test: (b) => b.readUInt32BE(0) === 0x89504e47 },
    { mime: "image/gif", ext: "gif", demux: "gif", test: (b) => b.toString("latin1", 0, 4) === "GIF8" },
    { mime: "image/webp", ext: "webp", demux: "webp_pipe", test: (b) => b.toString("latin1", 0, 4) === "RIFF" && b.toString("latin1", 8, 12) === "WEBP" },
    { mime: "video/webm", ext: "webm", demux: "matroska", video: true, test: (b) => b.readUInt32BE(0) === 0x1a45dfa3 },
    { mime: "video/mp4", ext: "mp4", demux: "mov", video: true, test: (b) => b.toString("latin1", 4, 8) === "ftyp" },
];

function sniff(buf) {
    if (buf.length < 16) return null;
    return TYPES.find((t) => t.test(buf)) || null;
}

function stripJpeg(b) {
    const out = [b.subarray(0, 2)];
    let i = 2;
    while (i + 4 <= b.length) {
        if (b[i] !== 0xff) return b; // not what we expected, keep the original
        const m = b[i + 1];
        if (m === 0xff) { i++; continue; }
        if (m === 0xda) { out.push(b.subarray(i)); return Buffer.concat(out); }
        if (m === 0xd8 || (m >= 0xd0 && m <= 0xd7) || m === 0x01) { out.push(b.subarray(i, i + 2)); i += 2; continue; }
        const len = b.readUInt16BE(i + 2);
        const seg = b.subarray(i, i + 2 + len);
        // APP1..APP15 hold EXIF/XMP/etc, except APP14 (Adobe colour info); FE is a comment
        const drop = (m >= 0xe1 && m <= 0xef && m !== 0xee) || m === 0xfe;
        if (!drop) out.push(seg);
        i += 2 + len;
    }
    return b;
}

function stripPng(b) {
    const out = [b.subarray(0, 8)];
    let i = 8;
    const drop = new Set(["tEXt", "zTXt", "iTXt", "eXIf", "tIME"]);
    while (i + 12 <= b.length) {
        const len = b.readUInt32BE(i);
        const type = b.toString("latin1", i + 4, i + 8);
        const end = i + 12 + len;
        if (end > b.length) return b;
        if (!drop.has(type)) out.push(b.subarray(i, end));
        i = end;
        if (type === "IEND") break;
    }
    return Buffer.concat(out);
}

function stripWebp(b) {
    const out = [];
    let i = 12;
    while (i + 8 <= b.length) {
        const type = b.toString("latin1", i, i + 4);
        const len = b.readUInt32LE(i + 4);
        const end = i + 8 + len + (len & 1);
        if (end > b.length + 1) return b;
        if (type !== "EXIF" && type !== "XMP ") {
            const chunk = Buffer.from(b.subarray(i, Math.min(end, b.length)));
            if (type === "VP8X") chunk[8] &= ~0x0c; // clear the EXIF and XMP flags
            out.push(chunk);
        }
        i = end;
    }
    const body = Buffer.concat(out);
    const head = Buffer.alloc(12);
    head.write("RIFF", 0, "latin1");
    head.writeUInt32LE(body.length + 4, 4);
    head.write("WEBP", 8, "latin1");
    return Buffer.concat([head, body]);
}

function stripImage(buf, type) {
    try {
        if (type.ext === "jpg") return stripJpeg(buf);
        if (type.ext === "png") return stripPng(buf);
        if (type.ext === "webp") return stripWebp(buf);
    } catch {
        // a malformed file falls through to ffmpeg, which rejects it if it's broken
    }
    return buf;
}

function run(cmd, args) {
    return new Promise((ok, fail) => {
        execFile(cmd, args, { timeout: 30000, maxBuffer: 1 << 20 }, (err, stdout, stderr) => {
            if (err) fail(new Error((stderr || err.message).toString().trim().split("\n").pop()));
            else ok(stdout);
        });
    });
}

async function probe(file, type) {
    const out = await run("ffprobe", [
        "-v", "error", "-f", type.demux, "-i", file,
        "-show_entries", "stream=codec_type,codec_name,width,height:format=duration", "-of", "json",
    ]);
    const j = JSON.parse(out);
    const v = (j.streams || []).find((s) => s.codec_type === "video");
    if (!v || !v.width || !v.height) throw new Error("no picture in that file");
    return { w: v.width, h: v.height, duration: Number(j.format?.duration) || 0, codec: v.codec_name };
}

async function thumbnail(file, type, out, max) {
    await run("ffmpeg", [
        "-nostdin", "-v", "error", "-f", type.demux, "-i", file,
        "-frames:v", "1",
        "-vf", `scale=w='min(${max},iw)':h='min(${max},ih)':force_original_aspect_ratio=decrease`,
        ...(out.endsWith(".jpg") ? ["-q:v", "4"] : []),
        "-y", out,
    ]);
}

// copies the streams without re-encoding, dropping metadata and extra tracks
async function cleanVideo(file, type, out) {
    await run("ffmpeg", [
        "-nostdin", "-v", "error", "-f", type.demux, "-i", file,
        "-map", "0:v:0", "-map", "0:a:0?", "-c", "copy", "-map_metadata", "-1",
        ...(type.ext === "mp4" ? ["-movflags", "+faststart", "-f", "mp4"] : ["-f", "webm"]),
        "-y", out,
    ]);
}

module.exports = { sniff, stripImage, probe, thumbnail, cleanVideo };

