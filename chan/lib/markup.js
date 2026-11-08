// Turns a raw comment into HTML: >greentext, >>123 quotes,
// >>>/g/ and >>>/g/123 board links, [spoiler]...[/spoiler].
const BASE = "/chan";

function esc(s) {
    return String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
}

const TOKEN = />>>\/([a-z0-9]{1,10})\/(\d{1,10})?|>>(\d{1,10})/g;

// lookup(board, no) returns the thread number the post is in, or null
function render(comment, board, thread, lookup) {
    const lines = comment.replace(/\r\n?/g, "\n").split("\n");
    const html = lines.map((line) => {
        const green = line.startsWith(">") && !/^>>\d/.test(line) && !/^>>>\//.test(line);
        let out = "";
        let last = 0;
        for (const m of line.matchAll(TOKEN)) {
            out += esc(line.slice(last, m.index));
            last = m.index + m[0].length;
            if (m[3]) {
                const no = Number(m[3]);
                const t = lookup(board, no);
                if (t == null) out += `<span class="dead">&gt;&gt;${no}</span>`;
                else out += quoteLink(board, t, no, t !== thread);
            } else if (m[2]) {
                const no = Number(m[2]);
                const t = lookup(m[1], no);
                if (t == null) out += `<span class="dead">&gt;&gt;&gt;/${esc(m[1])}/${no}</span>`;
                else out += `<a class="ql x" href="${BASE}/${m[1]}/thread/${t}#p${no}" data-b="${m[1]}" data-no="${no}">&gt;&gt;&gt;/${esc(m[1])}/${no}</a>`;
            } else {
                out += `<a class="bl" href="${BASE}/${m[1]}/">&gt;&gt;&gt;/${esc(m[1])}/</a>`;
            }
        }
        out += esc(line.slice(last));
        out = out.replace(/\[spoiler\]([\s\S]*?)\[\/spoiler\]/g, '<span class="sp">$1</span>');
        return green ? `<span class="gt">${out}</span>` : out;
    });
    return html.join("<br>");
}

function quoteLink(board, thread, no, cross) {
    return `<a class="ql${cross ? " x" : ""}" href="${BASE}/${board}/thread/${thread}#p${no}" data-b="${board}" data-no="${no}">&gt;&gt;${no}</a>`;
}

// post numbers a comment quotes in its own board, for the "replies" line under each post
function quotedNumbers(comment) {
    const seen = new Set();
    for (const m of comment.matchAll(/(?<!>)>>(\d{1,10})/g)) seen.add(Number(m[1]));
    return [...seen];
}

module.exports = { esc, render, quotedNumbers, BASE };
