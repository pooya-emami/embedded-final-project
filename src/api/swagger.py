from fastapi import FastAPI, Response
from fastapi.responses import RedirectResponse
import requests
import urllib3

app = FastAPI(
    title="Embedded Security System API",
    description="REST API for Orange Pi Security System - Part 2",
    version="1.0.0"
)

BASE_URL = "https://localhost:8443"
VERIFY_SSL = False

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

@app.get("/", include_in_schema=False)
async def root():
    return RedirectResponse(url="/docs")

@app.get("/api/v1/telemetry", tags=["Telemetry"])
async def get_telemetry():
    try:
        resp = requests.get(f"{BASE_URL}/api/v1/telemetry", verify=VERIFY_SSL, timeout=5)
        return resp.json()
    except Exception as e:
        return {"error": str(e), "temp": -1, "mem": -1, "cpu": -1}

@app.get("/api/v1/stream", tags=["Stream"])
async def get_stream():
    try:
        resp = requests.get(f"{BASE_URL}/snapshot", verify=VERIFY_SSL, timeout=5)
        return Response(content=resp.content, media_type="image/jpeg")
    except Exception as e:
        return {"error": str(e)}

@app.get("/api/v1/persons", tags=["Detection"])
async def get_persons():
    try:
        resp = requests.get(f"{BASE_URL}/api/v1/persons", verify=VERIFY_SSL, timeout=5)
        return resp.json()
    except Exception as e:
        return {"error": str(e), "count": 0, "timestamp": 0}

@app.get("/api/v1/history", tags=["Detection"])
async def get_history():
    try:
        resp = requests.get(f"{BASE_URL}/api/v1/history", verify=VERIFY_SSL, timeout=5)
        return resp.json()
    except Exception as e:
        return {"error": str(e), "history": []}

@app.post("/api/v1/command", tags=["System"])
async def post_command(cmd: dict):
    try:
        resp = requests.post(f"{BASE_URL}/api/v1/command", 
                            json=cmd, 
                            verify=VERIFY_SSL, 
                            timeout=5)
        return resp.json()
    except Exception as e:
        return {"error": str(e), "status": "failed"}

@app.get("/api/v1/health", tags=["System"])
async def health_check():
    try:
        resp = requests.get(f"{BASE_URL}/api/v1/health", verify=VERIFY_SSL, timeout=5)
        return {"status": "ok", "message": "Server is running"}
    except Exception as e:
        return {"status": "error", "message": str(e)}

@app.get("/api/v1/guard", tags=["System"])
async def get_guard():
    try:
        resp = requests.get(f"{BASE_URL}/api/v1/guard", verify=VERIFY_SSL, timeout=5)
        return resp.json()
    except Exception as e:
        return {"error": str(e), "guard_enabled": False}

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)