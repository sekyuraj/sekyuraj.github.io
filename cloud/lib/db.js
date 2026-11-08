const { DatabaseSync } = require("node:sqlite");
const path = require("path");

function open(dataDir) {
    const db = new DatabaseSync(path.join(dataDir, "cloud.db"));
    db.exec(`
        PRAGMA journal_mode = WAL;
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY,
            username TEXT NOT NULL UNIQUE COLLATE NOCASE,
            pass TEXT NOT NULL,
            created INTEGER NOT NULL,
            quota INTEGER NOT NULL,
            used INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS sessions (
            hash TEXT PRIMARY KEY,
            user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            created INTEGER NOT NULL,
            expires INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS invoices (
            id INTEGER PRIMARY KEY,
            token_hash TEXT NOT NULL UNIQUE,
            addr_index INTEGER NOT NULL,
            address TEXT NOT NULL,
            amount_sat INTEGER NOT NULL,
            quoted_at INTEGER NOT NULL,
            created INTEGER NOT NULL,
            status TEXT NOT NULL,             -- open, paid, used, expired
            paid_at INTEGER,
            user_id INTEGER,
            iphash TEXT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS invoices_ip ON invoices (iphash, created);
        CREATE TABLE IF NOT EXISTS shares (
            token TEXT PRIMARY KEY,
            user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            path TEXT NOT NULL,
            created INTEGER NOT NULL,
            downloads INTEGER NOT NULL DEFAULT 0
        );
    `);
    const q = (sql) => db.prepare(sql);
    const st = {
        userByName: q("SELECT * FROM users WHERE username = ?"),
        userById: q("SELECT * FROM users WHERE id = ?"),
        users: q("SELECT id FROM users"),
        addUser: q("INSERT INTO users (username, pass, created, quota) VALUES (?, ?, ?, ?) RETURNING id"),
        setPass: q("UPDATE users SET pass = ? WHERE id = ?"),
        setUsed: q("UPDATE users SET used = ? WHERE id = ?"),
        addUsed: q("UPDATE users SET used = MAX(0, used + ?) WHERE id = ?"),

        addSession: q("INSERT INTO sessions (hash, user_id, created, expires) VALUES (?, ?, ?, ?)"),
        session: q("SELECT * FROM sessions WHERE hash = ? AND expires > ?"),
        dropSession: q("DELETE FROM sessions WHERE hash = ?"),
        dropUserSessions: q("DELETE FROM sessions WHERE user_id = ? AND hash <> ?"),
        cleanSessions: q("DELETE FROM sessions WHERE expires < ?"),

        invoice: q("SELECT * FROM invoices WHERE token_hash = ?"),
        addInvoice: q(`INSERT INTO invoices (token_hash, addr_index, address, amount_sat, quoted_at, created, status, iphash)
                       VALUES (?, ?, ?, ?, ?, ?, 'open', ?) RETURNING id`),
        requote: q("UPDATE invoices SET amount_sat = ?, quoted_at = ? WHERE id = ?"),
        markPaid: q("UPDATE invoices SET status = 'paid', paid_at = ? WHERE id = ? AND status = 'open'"),
        markUsed: q("UPDATE invoices SET status = 'used', user_id = ? WHERE id = ? AND status = 'paid'"),
        markExpired: q("UPDATE invoices SET status = 'expired' WHERE id = ?"),
        staleInvoices: q("SELECT * FROM invoices WHERE status = 'open' AND created < ? ORDER BY addr_index"),
        maxIndex: q("SELECT MAX(addr_index) AS m FROM invoices"),
        invoicesFromIp: q("SELECT COUNT(*) AS n FROM invoices WHERE iphash = ? AND created > ?"),

        share: q("SELECT * FROM shares WHERE token = ?"),
        addShare: q("INSERT INTO shares (token, user_id, path, created) VALUES (?, ?, ?, ?)"),
        sharesOf: q("SELECT * FROM shares WHERE user_id = ? ORDER BY created DESC"),
        dropShare: q("DELETE FROM shares WHERE token = ? AND user_id = ?"),
        dropSharesUnder: q("DELETE FROM shares WHERE user_id = ? AND (path = ? OR path LIKE ? ESCAPE '\\')"),
        moveShares: q("UPDATE shares SET path = ? || substr(path, ?) WHERE user_id = ? AND (path = ? OR path LIKE ? ESCAPE '\\')"),
        countDownload: q("UPDATE shares SET downloads = downloads + 1 WHERE token = ?"),
    };
    return { db, st };
}

module.exports = { open };
