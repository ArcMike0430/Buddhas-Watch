# Buddhas-Watch App Store Backend

A lightweight self-hosted app store for distributing firmware apps to the UeeKKoo ESP32-S3-Touch-AMOLED-2.06 watch.

## Quick Start

```bash
# Install dependencies
npm install

# Start with default settings (port 3000, admin token "change_me")
npm start

# Production: set environment variables
PORT=3000 ADMIN_TOKEN=your_secure_token npm start
```

## REST API

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/version` | API version check |
| `GET` | `/api/apps` | List all available apps |
| `GET` | `/api/apps/:id` | App details + download URL |
| `GET` | `/api/apps/:id/download` | Download app binary |
| `POST` | `/api/apps/:id/rate` | Submit star rating (1–5) |
| `POST` | `/admin/apps` | Upload a new app (admin only) |
| `DELETE` | `/admin/apps/:id` | Remove an app (admin only) |

## Uploading an App

```bash
curl -X POST http://localhost:3000/admin/apps \
  -H "Authorization: ******" \
  -F "id=my_app_v1" \
  -F "name=My App" \
  -F "version=1.0.0" \
  -F "description=Description here" \
  -F "author=Your Name" \
  -F "changelog=Initial release" \
  -F "binary=@/path/to/app.bin"
```

## App Metadata Schema

```json
{
  "id": "wifi_csi_v1",
  "name": "WiFi CSI Capture",
  "version": "1.0.0",
  "description": "Real-time WiFi CSI capture and streaming",
  "author": "Buddhas-Watch",
  "icon_url": "http://server/icons/wifi_csi_v1.png",
  "binary_url": "http://server/api/apps/wifi_csi_v1/download",
  "size_bytes": 262144,
  "checksum": "sha256:<64-char-hex>",
  "min_firmware_version": "1.0.0",
  "release_date": "2026-07-13",
  "changelog": "Initial release"
}
```

## Hosting Options (Cost-Effective)

### Free Tier
- **GitHub Pages + GitHub Releases**: Host manifest.json on Pages, binaries in Releases
- **Railway / Render**: Free tier Node.js hosting with persistent disk

### Cheap VPS ($5–10/month)
- DigitalOcean Droplet, Hetzner CX11, or Vultr
- Run with PM2: `npm install -g pm2 && pm2 start server.js`

### Reverse Proxy (nginx)
```nginx
server {
    listen 443 ssl;
    server_name store.buddhas-watch.example.com;
    location / { proxy_pass http://127.0.0.1:3000; }
}
```

## Watch Integration

The watch `app_manager.c` fetches `GET /api/apps` on startup (and when auto-update is enabled), displays available apps in the LVGL list, and downloads selected apps to the SD card at `/sdcard/apps/<id>.bin`.

Checksums are verified with SHA-256 before marking an app as installed.
