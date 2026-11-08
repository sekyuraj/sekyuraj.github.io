const { DatabaseSync } = require("node:sqlite");
const path = require("path");

function open(dataDir) {
    const db = new DatabaseSync(path.join(dataDir, "chan.db"));
    db.exec(`
        PRAGMA journal_mode = WAL;
        PRAGMA foreign_keys = ON;
        CREATE TABLE IF NOT EXISTS counters (board TEXT PRIMARY KEY, last INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS posts (
            board TEXT NOT NULL,
            no INTEGER NOT NULL,
            thread INTEGER NOT NULL,          -- equals no for the opening post
            time INTEGER NOT NULL,
            bumped INTEGER,                   -- only on opening posts
            name TEXT NOT NULL,
            trip TEXT,
            uid TEXT,
            subject TEXT,
            comment TEXT NOT NULL,
            html TEXT NOT NULL,
            sage INTEGER NOT NULL DEFAULT 0,
            sticky INTEGER NOT NULL DEFAULT 0,
            locked INTEGER NOT NULL DEFAULT 0,
            iphash TEXT NOT NULL,
            delpass TEXT,
            file TEXT, thumb TEXT, file_name TEXT, file_size INTEGER,
            file_w INTEGER, file_h INTEGER, tn_w INTEGER, tn_h INTEGER,
            file_sha TEXT, file_mime TEXT, spoiler INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (board, no)
        );
        CREATE INDEX IF NOT EXISTS posts_thread ON posts (board, thread, no);
        CREATE INDEX IF NOT EXISTS posts_bump ON posts (board, sticky, bumped);
        CREATE INDEX IF NOT EXISTS posts_ip ON posts (iphash);
        CREATE INDEX IF NOT EXISTS posts_time ON posts (time);
        CREATE TABLE IF NOT EXISTS bans (
            id INTEGER PRIMARY KEY,
            iphash TEXT NOT NULL,
            reason TEXT NOT NULL,
            created INTEGER NOT NULL,
            until INTEGER,                    -- null = forever
            board TEXT, no INTEGER            -- the post that got the ban
        );
        CREATE INDEX IF NOT EXISTS bans_ip ON bans (iphash);
        CREATE TABLE IF NOT EXISTS reports (
            id INTEGER PRIMARY KEY,
            board TEXT NOT NULL, no INTEGER NOT NULL,
            reason TEXT NOT NULL, time INTEGER NOT NULL, iphash TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS filebans (sha TEXT PRIMARY KEY, reason TEXT, time INTEGER);
    `);

    const q = (sql) => db.prepare(sql);
    const st = {
        nextNo: q("UPDATE counters SET last = last + 1 WHERE board = ? RETURNING last"),
        addCounter: q("INSERT OR IGNORE INTO counters (board, last) VALUES (?, 0)"),
        insert: q(`INSERT INTO posts (board, no, thread, time, bumped, name, trip, uid, subject, comment, html,
                   sage, iphash, delpass, file, thumb, file_name, file_size, file_w, file_h, tn_w, tn_h,
                   file_sha, file_mime, spoiler)
                   VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`),
        get: q("SELECT * FROM posts WHERE board = ? AND no = ?"),
        threads: q(`SELECT * FROM posts WHERE board = ? AND no = thread
                    ORDER BY sticky DESC, bumped DESC LIMIT ? OFFSET ?`),
        countThreads: q("SELECT COUNT(*) AS n FROM posts WHERE board = ? AND no = thread"),
        lastReplies: q(`SELECT * FROM (SELECT * FROM posts WHERE board = ? AND thread = ? AND no <> thread
                        ORDER BY no DESC LIMIT ?) ORDER BY no`),
        stats: q(`SELECT COUNT(*) AS replies, COUNT(file) AS images FROM posts
                  WHERE board = ? AND thread = ? AND no <> thread`),
        threadPosts: q("SELECT * FROM posts WHERE board = ? AND thread = ? ORDER BY no"),
        after: q("SELECT * FROM posts WHERE board = ? AND thread = ? AND no > ? ORDER BY no"),
        bump: q("UPDATE posts SET bumped = ? WHERE board = ? AND no = ?"),
        oldest: q(`SELECT no FROM posts WHERE board = ? AND no = thread AND sticky = 0
                   ORDER BY bumped DESC LIMIT -1 OFFSET ?`),
        filesOfThread: q("SELECT file, thumb FROM posts WHERE board = ? AND thread = ? AND file IS NOT NULL"),
        delThread: q("DELETE FROM posts WHERE board = ? AND thread = ?"),
        delPost: q("DELETE FROM posts WHERE board = ? AND no = ?"),
        delFile: q(`UPDATE posts SET file = NULL, thumb = NULL, file_name = NULL, file_size = NULL,
                    file_w = NULL, file_h = NULL, tn_w = NULL, tn_h = NULL, file_mime = NULL
                    WHERE board = ? AND no = ?`),
        setFlag: {
            sticky: q("UPDATE posts SET sticky = ? WHERE board = ? AND no = ?"),
            locked: q("UPDATE posts SET locked = ? WHERE board = ? AND no = ?"),
        },
        postsByIp: q("SELECT board, no, thread, file, thumb FROM posts WHERE iphash = ?"),
        recent: q("SELECT * FROM posts ORDER BY time DESC LIMIT ?"),
        total: q("SELECT COUNT(*) AS n FROM posts"),
        activeBan: q(`SELECT * FROM bans WHERE iphash = ? AND (until IS NULL OR until > ?)
                      ORDER BY until IS NULL DESC, until DESC LIMIT 1`),
        addBan: q("INSERT INTO bans (iphash, reason, created, until, board, no) VALUES (?, ?, ?, ?, ?, ?)"),
        bans: q("SELECT * FROM bans WHERE until IS NULL OR until > ? ORDER BY created DESC"),
        unban: q("DELETE FROM bans WHERE id = ?"),
        addReport: q("INSERT INTO reports (board, no, reason, time, iphash) VALUES (?, ?, ?, ?, ?)"),
        reportCount: q("SELECT COUNT(*) AS n FROM reports WHERE iphash = ? AND time > ?"),
        reports: q(`SELECT board, no, COUNT(*) AS n, GROUP_CONCAT(reason, ' | ') AS reasons, MAX(time) AS time
                    FROM reports GROUP BY board, no ORDER BY time DESC`),
        dismiss: q("DELETE FROM reports WHERE board = ? AND no = ?"),
        fileBanned: q("SELECT 1 FROM filebans WHERE sha = ?"),
        banFile: q("INSERT OR IGNORE INTO filebans (sha, reason, time) VALUES (?, ?, ?)"),
    };

    function tx(fn) {
        db.exec("BEGIN IMMEDIATE");
        try {
            const r = fn();
            db.exec("COMMIT");
            return r;
        } catch (e) {
            db.exec("ROLLBACK");
            throw e;
        }
    }

    return { db, st, tx };
}

module.exports = { open };
