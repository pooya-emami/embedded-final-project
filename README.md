# Smart Security System

**Student:** Pooya Emami Varzeghani  
**Student ID:** 404300409  
**Course:** Embedded Systems - August 2026

---

## Overview

A complete embedded security system that receives live camera feed, detects humans using YOLOv8n, and reports via HTTPS, email, and MQTT. All core logic is in C.

---

## Architecture

```
Camera → Relay → Detection → Server → HTTPS API
                ↑              ↑
                └── MQTT ──────┘
                └── SQLite ───┘
                └── Email ────┘
```

### Services

| Service | Binary | Port | Description |
|---------|--------|------|-------------|
| relay.service | security_relay | 9000 | MJPEG stream receiver |
| detection.service | detection_server | - | YOLOv8n detection |
| server.service | security_server | 8443 | HTTPS server |
| swagger.service | swagger.py | 8000 | API documentation |

---

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/telemetry` | GET | CPU temp, memory, CPU usage |
| `/api/v1/stream` | GET | MJPEG stream |
| `/api/v1/persons` | GET | Current person count |
| `/api/v1/history` | GET | Detection history (last 5) |
| `/api/v1/command` | POST | reboot/shutdown |
| `/api/v1/guard` | GET/POST | Guard mode |
| `/api/v1/stream_mode` | POST | idle/raw/processed |

---

## Quick Install

```bash
# Install dependencies
sudo apt install -y build-essential cmake git pkg-config \
    libssl-dev libmosquitto-dev mosquitto libpaho-mqtt-dev libcurl4-openssl-dev \
    libsqlite3-dev libopencv-dev 

# Install ONNX Runtime
cd /tmp && wget https://github.com/microsoft/onnxruntime/releases/download/v1.17.1/onnxruntime-linux-x64-1.17.1.tgz
tar -xzf onnxruntime-linux-x64-1.17.1.tgz
sudo cp -r onnxruntime-linux-x64-1.17.1/* /usr/local/
sudo ldconfig

# Build
cd ~/embproj/proj/
make clean && make all
sudo make install

# Install services
sudo cp systemd/*.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable relay detection server swagger
sudo systemctl start relay detection server swagger
```

---

## Configuration

Edit `src/server/server.conf`:

```ini
FRAME_INTERVAL_MS = 33
FRAME_WIDTH = 640
FRAME_HEIGHT = 480
PORT_HTTP = 8080
PORT_HTTPS = 8443
TEMP_THROTTLE_C = 70
WATCHDOG_TIMEOUT_MS = 10000

SMTP_SERVER=smtp://smtp.gmail.com:587
SMTP_USER=your-email@gmail.com
SMTP_PASS=your-app-password
SMTP_TO=your-email@gmail.com

MQTT_HOST=127.0.0.1
MQTT_PORT=1883
MQTT_USER=security_user
MQTT_PASS=your-password
```

---

## Testing

```bash
# Temperature monitoring (5 min, 30s interval)
./test/exp2-1.sh

# Memory monitoring (5 min, 30s interval)
./test/exp2-2.sh

# 50 concurrent requests (30s)
./test/exp2-3.sh

# View stream
https://localhost:8443/

# Swagger docs
http://localhost:8000/docs
```

---

## Features Summary

- HTTPS with self-signed SSL (CN=404300409)
- HTTP → HTTPS redirect (301)
- Auto-start via systemd
- YOLOv8n human detection
- Email alerts with images (debounce: 30s)
- MQTT with QoS 1, authentication, LWT
- SQLite history logging
- Thermal throttling (70°C → FPS/resolution reduction)
- Watchdog (camera disconnect detection + auto-restart)
- Guard mode (immediate alerts on first detection)
- Swagger API documentation

---

## Files

```
proj/
├── src/           # All C source code
├── models/        # YOLOv8n ONNX model
├── systemd/       # Service files
├── test/          # Experiment scripts
└── Makefile       # Build system
```

---

## SSL Certificate

Generate with:

```bash
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes -subj "/CN=404300409"
```

---

## Notes

- VM used instead of Orange Pi for development
- Recommended resolution: 640×480 (best FPS/accuracy trade-off)
- All core logic in C (Python only for Swagger/docs)
