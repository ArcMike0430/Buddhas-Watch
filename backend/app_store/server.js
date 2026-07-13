/**
 * server.js — Buddhas-Watch self-hosted app store backend
 *
 * REST API:
 *   GET  /api/version          — API version (watch uses this for compatibility check)
 *   GET  /api/apps             — List all available apps
 *   GET  /api/apps/:id         — Get app details + download URL
 *   GET  /api/apps/:id/download — Download app binary (streams file)
 *   POST /api/apps/:id/rate    — Submit a star rating (1-5)
 *   POST /admin/apps           — Upload a new app binary (multipart/form-data)
 *
 * App metadata is stored in ./apps/manifest.json.
 * App binaries are stored in ./apps/binaries/<id>-<version>.bin.
 *
 * Environment variables:
 *   PORT          — HTTP port (default 3000)
 *   ADMIN_TOKEN   — Token for admin endpoints (default: change_me)
 *   STORE_VERSION — API version string (default: 1.0.0)
 */

'use strict';

const express      = require('express');
const cors         = require('cors');
const multer       = require('multer');
const rateLimit    = require('express-rate-limit');
const fs           = require('fs');
const path         = require('path');
const crypto       = require('crypto');

const app = express();

// ── Configuration ─────────────────────────────────────────────────────────
const PORT          = parseInt(process.env.PORT          || '3000', 10);
const ADMIN_TOKEN   = process.env.ADMIN_TOKEN            || 'change_me';
const STORE_VERSION = process.env.STORE_VERSION          || '1.0.0';
const APPS_DIR      = path.join(__dirname, 'apps');
const BINARIES_DIR  = path.join(APPS_DIR, 'binaries');
const MANIFEST_FILE = path.join(APPS_DIR, 'manifest.json');
const RATINGS_FILE  = path.join(APPS_DIR, 'ratings.json');

// Ensure directories exist
[APPS_DIR, BINARIES_DIR].forEach(d => {
    if (!fs.existsSync(d)) fs.mkdirSync(d, { recursive: true });
});

// ── ID validation ─────────────────────────────────────────────────────────
/**
 * sanitizeId — allow only safe characters to prevent path traversal.
 * Returns the validated id string, or null if invalid.
 */
function sanitizeId(id) {
    if (!id || typeof id !== 'string') return null;
    // Only alphanumerics, hyphens, underscores — no dots or slashes
    if (!/^[a-zA-Z0-9_-]{1,64}$/.test(id)) return null;
    return id;
}

/**
 * safeBinPath — construct a binary file path and verify it is inside
 * BINARIES_DIR to prevent directory traversal attacks.
 * Returns the resolved path, or null if the path would escape the directory.
 */
function safeBinPath(id, version) {
    const sId  = sanitizeId(id);
    const sVer = sanitizeId(version);
    if (!sId || !sVer) return null;
    const resolved = path.resolve(BINARIES_DIR, `${sId}-${sVer}.bin`);
    if (!resolved.startsWith(path.resolve(BINARIES_DIR) + path.sep)) return null;
    return resolved;
}

// ── Rate limiting ─────────────────────────────────────────────────────────
/** General API: 100 requests per 15-minute window per IP */
const apiLimiter = rateLimit({
    windowMs: 15 * 60 * 1000,
    max: 100,
    standardHeaders: true,
    legacyHeaders: false,
    message: { error: 'Too many requests, please try again later.' },
});

/** Download endpoint: 20 downloads per 15-minute window per IP */
const downloadLimiter = rateLimit({
    windowMs: 15 * 60 * 1000,
    max: 20,
    standardHeaders: true,
    legacyHeaders: false,
    message: { error: 'Download rate limit exceeded.' },
});

/** Admin endpoints: 10 requests per minute */
const adminLimiter = rateLimit({
    windowMs: 60 * 1000,
    max: 10,
    standardHeaders: true,
    legacyHeaders: false,
    message: { error: 'Admin rate limit exceeded.' },
});

// ── Multer (binary upload) ────────────────────────────────────────────────
const storage = multer.diskStorage({
    destination: BINARIES_DIR,
    filename: (req, file, cb) => {
        const rawId      = (req.body && req.body.id)      || '';
        const rawVersion = (req.body && req.body.version) || '';
        const id      = sanitizeId(rawId)      || 'unknown';
        const version = sanitizeId(rawVersion) || '0-0-0';
        cb(null, `${id}-${version}.bin`);
    },
});
const upload = multer({
    storage,
    limits: { fileSize: 4 * 1024 * 1024 }, // 4 MB max binary
    fileFilter: (_req, file, cb) => {
        if (file.mimetype === 'application/octet-stream' ||
            file.originalname.endsWith('.bin')) {
            cb(null, true);
        } else {
            cb(new Error('Only .bin files are accepted'));
        }
    },
});

// ── Middleware ────────────────────────────────────────────────────────────
app.use(cors());
app.use(express.json());
app.use('/api/', apiLimiter);

// ── Helpers ───────────────────────────────────────────────────────────────
function loadManifest() {
    if (!fs.existsSync(MANIFEST_FILE)) return [];
    try {
        return JSON.parse(fs.readFileSync(MANIFEST_FILE, 'utf8'));
    } catch {
        return [];
    }
}

function saveManifest(apps) {
    fs.writeFileSync(MANIFEST_FILE, JSON.stringify(apps, null, 2));
}

function loadRatings() {
    if (!fs.existsSync(RATINGS_FILE)) return {};
    try {
        return JSON.parse(fs.readFileSync(RATINGS_FILE, 'utf8'));
    } catch {
        return {};
    }
}

function saveRatings(ratings) {
    fs.writeFileSync(RATINGS_FILE, JSON.stringify(ratings, null, 2));
}

function sha256file(filePath) {
    const data = fs.readFileSync(filePath);
    return 'sha256:' + crypto.createHash('sha256').update(data).digest('hex');
}

function requireAdmin(req, res, next) {
    const auth = req.headers.authorization || '';
    const expected = 'Bearer ' + ADMIN_TOKEN;
    if (auth !== expected) {
        return res.status(401).json({ error: 'Unauthorized' });
    }
    next();
}

// ── Routes ────────────────────────────────────────────────────────────────

/**
 * GET /api/version
 * Returns the API version for watch compatibility checking.
 */
app.get('/api/version', (_req, res) => {
    res.json({ version: STORE_VERSION, api: 'buddhas-watch-store' });
});

/**
 * GET /api/apps
 * Returns the full app list with metadata (no binary URLs exposed).
 */
app.get('/api/apps', (req, res) => {
    const apps = loadManifest();
    const ratings = loadRatings();
    const result = apps.map(a => ({
        ...a,
        rating: ratings[a.id]
            ? (ratings[a.id].total / ratings[a.id].count).toFixed(1)
            : null,
    }));
    res.json(result);
});

/**
 * GET /api/apps/:id
 * Returns a single app's full metadata including download URL.
 */
app.get('/api/apps/:id', (req, res) => {
    const appId = sanitizeId(req.params.id);
    if (!appId) return res.status(400).json({ error: 'Invalid app ID' });

    const apps  = loadManifest();
    const found = apps.find(a => a.id === appId);
    if (!found) return res.status(404).json({ error: 'App not found' });

    const ratings = loadRatings();
    res.json({
        ...found,
        rating: ratings[found.id]
            ? (ratings[found.id].total / ratings[found.id].count).toFixed(1)
            : null,
    });
});

/**
 * GET /api/apps/:id/download
 * Streams the app binary to the client (watch or browser).
 */
app.get('/api/apps/:id/download', downloadLimiter, (req, res) => {
    const appId = sanitizeId(req.params.id);
    if (!appId) return res.status(400).json({ error: 'Invalid app ID' });

    const apps  = loadManifest();
    const found = apps.find(a => a.id === appId);
    if (!found) return res.status(404).json({ error: 'App not found' });

    const binPath = safeBinPath(found.id, found.version);
    if (!binPath || !fs.existsSync(binPath)) {
        return res.status(404).json({ error: 'Binary not available' });
    }

    const stat = fs.statSync(binPath);
    res.setHeader('Content-Type',   'application/octet-stream');
    res.setHeader('Content-Length', stat.size);
    res.setHeader('Content-Disposition',
                  `attachment; filename="${found.id}-${found.version}.bin"`);
    fs.createReadStream(binPath).pipe(res);
});

/**
 * POST /api/apps/:id/rate
 * Body: { "stars": 1-5 }
 */
app.post('/api/apps/:id/rate', (req, res) => {
    const appId = sanitizeId(req.params.id);
    if (!appId) return res.status(400).json({ error: 'Invalid app ID' });

    const apps  = loadManifest();
    const found = apps.find(a => a.id === appId);
    if (!found) return res.status(404).json({ error: 'App not found' });

    const stars = parseInt(req.body.stars, 10);
    if (isNaN(stars) || stars < 1 || stars > 5) {
        return res.status(400).json({ error: 'stars must be 1–5' });
    }

    const ratings = loadRatings();
    if (!ratings[appId]) {
        ratings[appId] = { total: 0, count: 0 };
    }
    ratings[appId].total += stars;
    ratings[appId].count += 1;
    saveRatings(ratings);

    const avg = (ratings[appId].total / ratings[appId].count).toFixed(1);
    res.json({ message: 'Rating submitted', average: avg });
});

/**
 * POST /admin/apps
 * Multipart upload: fields { id, name, version, description, author,
 *                             min_firmware_version, changelog }
 *                 + file field: binary
 *
 * Requires Authorization: ******
 */
app.post('/admin/apps', adminLimiter, requireAdmin, upload.single('binary'), (req, res) => {
    if (!req.file) {
        return res.status(400).json({ error: 'No binary uploaded' });
    }
    const { name, description, author, min_firmware_version, changelog } = req.body;
    const id      = sanitizeId(req.body.id);
    const version = sanitizeId(req.body.version);

    if (!id || !name || !version) {
        fs.unlinkSync(req.file.path);
        return res.status(400).json({ error: 'id (alphanumeric/_/-), name and version are required' });
    }

    const binPath  = req.file.path;
    const checksum = sha256file(binPath);
    const fileSize = fs.statSync(binPath).size;

    const baseUrl   = `${req.protocol}://${req.get('host')}`;
    const binaryUrl = `${baseUrl}/api/apps/${id}/download`;

    const newApp = {
        id,
        name,
        version,
        description:          description          || '',
        author:               author               || 'unknown',
        icon_url:             `${baseUrl}/icons/${id}.png`,
        binary_url:           binaryUrl,
        size_bytes:           fileSize,
        checksum,
        min_firmware_version: min_firmware_version || '1.0.0',
        release_date:         new Date().toISOString().split('T')[0],
        changelog:            changelog             || 'Initial release',
    };

    const apps  = loadManifest();
    const idx   = apps.findIndex(a => a.id === id);
    if (idx >= 0) {
        /* Update existing entry — remove old binary via safe path */
        const oldBin = safeBinPath(apps[idx].id, apps[idx].version);
        if (oldBin && fs.existsSync(oldBin) && oldBin !== binPath) fs.unlinkSync(oldBin);
        apps[idx] = newApp;
    } else {
        apps.push(newApp);
    }
    saveManifest(apps);

    res.status(201).json({ message: 'App published', app: newApp });
});

/**
 * DELETE /admin/apps/:id
 * Remove an app and its binary.
 */
app.delete('/admin/apps/:id', adminLimiter, requireAdmin, (req, res) => {
    const appId = sanitizeId(req.params.id);
    if (!appId) return res.status(400).json({ error: 'Invalid app ID' });

    const apps  = loadManifest();
    const idx   = apps.findIndex(a => a.id === appId);
    if (idx < 0) return res.status(404).json({ error: 'App not found' });

    const binPath = safeBinPath(apps[idx].id, apps[idx].version);
    if (binPath && fs.existsSync(binPath)) fs.unlinkSync(binPath);

    apps.splice(idx, 1);
    saveManifest(apps);

    res.json({ message: 'App removed' });
});

// ── Static icons directory ────────────────────────────────────────────────
const ICONS_DIR = path.join(__dirname, 'icons');
if (!fs.existsSync(ICONS_DIR)) fs.mkdirSync(ICONS_DIR);
app.use('/icons', express.static(ICONS_DIR));

// ── Error handler ─────────────────────────────────────────────────────────
app.use((err, _req, res, _next) => {
    console.error(err.message);
    res.status(500).json({ error: err.message });
});

// ── Start server ──────────────────────────────────────────────────────────
app.listen(PORT, () => {
    console.log(`Buddhas-Watch App Store running on http://0.0.0.0:${PORT}`);
    console.log(`Admin token: ${ADMIN_TOKEN === 'change_me' ? '⚠  CHANGE ADMIN_TOKEN env var!' : '(set)'}`);
});

module.exports = app; // for testing
