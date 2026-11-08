// multipart/form-data parser for one fully-read request body.
function parse(body, contentType) {
    const m = /boundary=(?:"([^"]+)"|([^;\s]+))/i.exec(contentType || "");
    if (!m) throw new Error("not a form upload");
    const boundary = Buffer.from("--" + (m[1] || m[2]));
    const fields = {};
    const files = {};

    let pos = body.indexOf(boundary);
    if (pos < 0) throw new Error("broken form upload");
    for (;;) {
        pos += boundary.length;
        if (body[pos] === 0x2d && body[pos + 1] === 0x2d) break; // closing "--"
        pos += 2; // CRLF after the boundary
        const headEnd = body.indexOf("\r\n\r\n", pos);
        if (headEnd < 0) break;
        const next = body.indexOf(boundary, headEnd + 4);
        if (next < 0) break;
        const head = body.subarray(pos, headEnd).toString("utf8");
        const data = body.subarray(headEnd + 4, next - 2);
        const name = /[;\s]name="([^"]*)"/i.exec(head)?.[1];
        const filename = /filename="([^"]*)"/i.exec(head)?.[1];
        if (name != null) {
            if (filename !== undefined) {
                if (data.length) files[name] = { filename, data };
            } else {
                fields[name] = data.toString("utf8");
            }
        }
        pos = next;
    }
    return { fields, files };
}

module.exports = { parse };
