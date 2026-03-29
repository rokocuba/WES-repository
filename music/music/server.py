import asyncio
import base64
import json
import re
import time as _time
from datetime import datetime
from pathlib import Path
from urllib.parse import unquote, urlparse

import aiohttp
from aiohttp import web

# ── Paths ──
BASE_DIR = Path(__file__).parent
MEDIA_DIR = BASE_DIR / "media"
SOUNDS_DIR = MEDIA_DIR / "sounds"
IMAGES_DIR = MEDIA_DIR / "images"
STATIC_DIR = BASE_DIR / "static"

SOUNDS_DIR.mkdir(parents=True, exist_ok=True)
IMAGES_DIR.mkdir(parents=True, exist_ok=True)

ALLOWED_AUDIO = {".mp3", ".mp4", ".wav", ".ogg", ".m4a", ".aac", ".flac", ".webm"}
DASHBOARD_HTML = (STATIC_DIR / "index.html").read_text()

# ── Server state ──
device_ws_ref = None
web_clients: set = set()
_msg_id = 0
_pending: dict = {}
_sound_task: asyncio.Task | None = None


def next_id():
    global _msg_id
    _msg_id += 1
    return _msg_id


# ── Broadcast ──
async def broadcast_web(data: dict):
    if not web_clients:
        return
    payload = json.dumps(data)
    dead = []
    for c in list(web_clients):
        try:
            if not c.closed:
                await c.send_str(payload)
            else:
                dead.append(c)
        except Exception:
            dead.append(c)
    for c in dead:
        web_clients.discard(c)


# ── Device command helper ──
async def send_to_device(cmd: str, extra: dict = None, timeout: float = 5.0):
    if device_ws_ref is None or device_ws_ref.closed:
        return None
    mid = next_id()
    msg = {"cmd": cmd, "id": mid}
    if extra:
        msg.update(extra)
    fut = asyncio.get_event_loop().create_future()
    _pending[mid] = fut
    await device_ws_ref.send_json(msg)
    try:
        return await asyncio.wait_for(fut, timeout)
    except asyncio.TimeoutError:
        return None
    finally:
        _pending.pop(mid, None)


# ── HTTP routes ──
async def index(request):
    return web.Response(text=DASHBOARD_HTML, content_type="text/html")


async def list_images(request):
    images = []
    for f in sorted(IMAGES_DIR.iterdir(), reverse=True):
        if f.is_file():
            st = f.stat()
            images.append({
                "name": f.name,
                "size": st.st_size,
                "timestamp": datetime.fromtimestamp(st.st_mtime).isoformat(),
            })
    return web.json_response(images)


async def list_sounds(request):
    sounds = []
    for f in sorted(SOUNDS_DIR.iterdir()):
        if f.is_file() and f.suffix.lower() in ALLOWED_AUDIO:
            st = f.stat()
            sounds.append({
                "name": f.name,
                "size": st.st_size,
                "added": datetime.fromtimestamp(st.st_mtime).isoformat(),
            })
    return web.json_response(sounds)


async def download_sound(request):
    data = await request.json()
    url = data.get("url", "").strip()
    if not url:
        return web.json_response({"error": "No URL provided"}, status=400)

    # Try yt-dlp first (handles YouTube, SoundCloud, and many other sites)
    try:
        result = await _download_with_ytdlp(url)
        if result:
            return web.json_response(result)
    except Exception as e:
        print(f"[{datetime.now():%H:%M:%S}] yt-dlp failed, trying direct: {e}")

    # Fallback: direct HTTP download
    return await _download_direct(url)


async def _download_with_ytdlp(url: str) -> dict | None:
    tmpl = str(SOUNDS_DIR / "%(title).80s.%(ext)s")
    proc = await asyncio.create_subprocess_exec(
        "yt-dlp",
        "--no-playlist",
        "-x",
        "--audio-format", "mp3",
        "--audio-quality", "5",
        "--max-filesize", "50m",
        "-o", tmpl,
        "--print", "after_move:filepath",
        url,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    stdout, stderr = await asyncio.wait_for(proc.communicate(), timeout=180)

    if proc.returncode != 0:
        err = stderr.decode().strip().split("\n")[-1] if stderr else "unknown error"
        if "Unsupported URL" in (stderr.decode() if stderr else ""):
            return None
        raise RuntimeError(err)

    filepath = stdout.decode().strip().split("\n")[-1]
    if not filepath or not Path(filepath).exists():
        raise RuntimeError("yt-dlp produced no output file")

    p = Path(filepath)
    size = p.stat().st_size
    print(f"[{datetime.now():%H:%M:%S}] yt-dlp downloaded: {p.name} ({size} bytes)")
    return {"status": "ok", "name": p.name, "size": size}


async def _download_direct(url: str):
    parsed = urlparse(url)
    filename = unquote(parsed.path.split("/")[-1])
    if not filename or "." not in filename:
        filename = f"sound_{datetime.now():%Y%m%d_%H%M%S}.mp3"

    filename = re.sub(r"[^\w\-.]", "_", filename)
    ext = Path(filename).suffix.lower()
    if ext not in ALLOWED_AUDIO:
        return web.json_response(
            {"error": f"Unsupported format: {ext}. Paste a direct audio URL or a YouTube/SoundCloud link."}, status=400
        )

    try:
        async with aiohttp.ClientSession() as session:
            async with session.get(url, timeout=aiohttp.ClientTimeout(total=120)) as resp:
                if resp.status != 200:
                    return web.json_response(
                        {"error": f"Download failed (HTTP {resp.status})"}, status=400
                    )
                ct = resp.headers.get("Content-Type", "")
                if "html" in ct.lower():
                    return web.json_response(
                        {"error": "URL returned HTML, not audio. Try a YouTube or SoundCloud link instead."}, status=400
                    )
                cl = resp.headers.get("Content-Length", "0")
                if cl.isdigit() and int(cl) > 50 * 1024 * 1024:
                    return web.json_response({"error": "File too large (>50MB)"}, status=400)
                content = await resp.read()
    except Exception as e:
        return web.json_response({"error": str(e)}, status=400)

    if len(content) < 100 or content[:15].startswith(b"<!DOCTYPE") or content[:5] == b"<html":
        return web.json_response(
            {"error": "Downloaded file is not audio. Try a YouTube or SoundCloud link."}, status=400
        )

    dest = SOUNDS_DIR / filename
    if dest.exists():
        stem = dest.stem
        for i in range(1, 100):
            dest = SOUNDS_DIR / f"{stem}_{i}{ext}"
            if not dest.exists():
                break

    dest.write_bytes(content)
    print(f"[{datetime.now():%H:%M:%S}] Direct download: {dest.name} ({len(content)} bytes)")
    return web.json_response({"status": "ok", "name": dest.name, "size": len(content)})


async def delete_sound(request):
    name = request.match_info["name"]
    path = SOUNDS_DIR / name
    if not path.exists() or path.parent.resolve() != SOUNDS_DIR.resolve():
        return web.json_response({"error": "Not found"}, status=404)
    path.unlink()
    return web.json_response({"status": "ok"})


# ── Image saving ──
def save_image(result: dict):
    img_b64 = result.get("data")
    if not img_b64:
        return
    try:
        img_bytes = base64.b64decode(img_b64)
        fmt = result.get("format", "bmp")
        ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
        path = IMAGES_DIR / f"{ts}.{fmt}"
        path.write_bytes(img_bytes)
        print(f"[{datetime.now():%H:%M:%S}] Saved image: {path.name} ({len(img_bytes)} bytes)")
    except Exception as e:
        print(f"Failed to save image: {e}")


# ── Sound streaming to device ──
async def stream_sound_to_device(filename: str):
    path = SOUNDS_DIR / filename
    if not path.exists():
        await broadcast_web({"type": "sound_status", "status": "error", "filename": filename})
        return

    if not device_ws_ref or device_ws_ref.closed:
        await broadcast_web({"type": "sound_status", "status": "error", "filename": filename})
        return

    await broadcast_web({"type": "sound_status", "status": "converting", "filename": filename})

    proc = await asyncio.create_subprocess_exec(
        "ffmpeg", "-i", str(path),
        "-f", "s16le", "-ar", "22050", "-ac", "1", "-",
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
    )

    await device_ws_ref.send_json({"cmd": "audio_start", "id": next_id()})
    await broadcast_web({"type": "sound_status", "status": "playing", "filename": filename})

    chunk_bytes = 8192
    bytes_per_sec = 44100.0
    total_sent = 0
    t_start = _time.monotonic()

    try:
        while True:
            chunk = await proc.stdout.read(chunk_bytes)
            if not chunk:
                break
            if not device_ws_ref or device_ws_ref.closed:
                print(f"[{datetime.now():%H:%M:%S}] Device disconnected during playback")
                break

            b64 = base64.b64encode(chunk).decode()
            try:
                await asyncio.wait_for(
                    device_ws_ref.send_json({"cmd": "audio_data", "data": b64}),
                    timeout=5.0
                )
            except asyncio.TimeoutError:
                print(f"[{datetime.now():%H:%M:%S}] Send timeout, skipping chunk")
                continue
            except Exception as e:
                print(f"[{datetime.now():%H:%M:%S}] Send error during playback: {e}")
                break
            total_sent += len(chunk)

            expected_time = total_sent / bytes_per_sec
            elapsed = _time.monotonic() - t_start
            drift = expected_time - elapsed
            if drift > 0:
                await asyncio.sleep(drift)
    except asyncio.CancelledError:
        pass
    except Exception as e:
        print(f"[{datetime.now():%H:%M:%S}] Streaming error: {e}")
    finally:
        proc.kill()
        await proc.wait()

    try:
        if device_ws_ref and not device_ws_ref.closed:
            await device_ws_ref.send_json({"cmd": "audio_stop", "id": next_id()})
    except Exception:
        pass

    await broadcast_web({"type": "sound_status", "status": "stopped", "filename": filename})
    print(f"[{datetime.now():%H:%M:%S}] Finished streaming {filename} ({total_sent} bytes sent)")


# ── WebSocket: device ──
async def ws_device_handler(request):
    global device_ws_ref
    ws = web.WebSocketResponse(max_msg_size=1 << 20, autoping=True)
    await ws.prepare(request)
    device_ws_ref = ws
    print(f"[{datetime.now():%H:%M:%S}] Device connected")
    await broadcast_web({"type": "device_status", "connected": True})

    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.TEXT:
                data = json.loads(msg.data)
                mid = data.get("id")
                if mid and mid in _pending:
                    _pending[mid].set_result(data)

                if data.get("cmd") == "capture_image":
                    save_image(data.get("result", {}))

                if data.get("cmd") != "audio_data":
                    await broadcast_web(data)

            elif msg.type == web.WSMsgType.ERROR:
                print(f"Device WS error: {ws.exception()}")
    finally:
        device_ws_ref = None
        print(f"[{datetime.now():%H:%M:%S}] Device disconnected")
        await broadcast_web({"type": "device_status", "connected": False})

    return ws


# ── WebSocket: web client ──
async def ws_web_handler(request):
    global _sound_task
    ws = web.WebSocketResponse(max_msg_size=1 << 20)
    await ws.prepare(request)
    web_clients.add(ws)
    print(f"[{datetime.now():%H:%M:%S}] Web client connected ({len(web_clients)} total)")

    connected = device_ws_ref is not None and not device_ws_ref.closed
    await ws.send_json({"type": "device_status", "connected": connected})

    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.TEXT:
                data = json.loads(msg.data)
                cmd = data.get("cmd")

                if cmd == "play_sound":
                    if _sound_task and not _sound_task.done():
                        _sound_task.cancel()
                        await asyncio.sleep(0.2)
                    _sound_task = asyncio.create_task(
                        stream_sound_to_device(data["filename"])
                    )
                    continue

                if cmd == "stop_sound":
                    if _sound_task and not _sound_task.done():
                        _sound_task.cancel()
                    await broadcast_web({"type": "sound_status", "status": "stopped", "filename": ""})
                    continue

                # Forward to device
                if device_ws_ref and not device_ws_ref.closed:
                    if "id" not in data:
                        data["id"] = next_id()
                    await device_ws_ref.send_json(data)

            elif msg.type == web.WSMsgType.ERROR:
                print(f"Web WS error: {ws.exception()}")
    finally:
        web_clients.discard(ws)
        print(f"[{datetime.now():%H:%M:%S}] Web client disconnected")

    return ws


# ── Background: healthcheck loop ──
async def healthcheck_loop(app):
    await asyncio.sleep(5)
    while True:
        try:
            result = await send_to_device("healthcheck")
            if result:
                await broadcast_web(result)
        except Exception as e:
            print(f"[{datetime.now():%H:%M:%S}] Healthcheck error: {e}")
        await asyncio.sleep(10)


async def on_startup(app):
    app["hc_task"] = asyncio.create_task(healthcheck_loop(app))


async def on_shutdown(app):
    app["hc_task"].cancel()


# ── App ──
app = web.Application()
app.on_startup.append(on_startup)
app.on_shutdown.append(on_shutdown)

app.router.add_get("/", index)
app.router.add_get("/ws/device", ws_device_handler)
app.router.add_get("/ws/web", ws_web_handler)
app.router.add_get("/api/images", list_images)
app.router.add_get("/api/sounds", list_sounds)
app.router.add_post("/api/sounds", download_sound)
app.router.add_delete("/api/sounds/{name}", delete_sound)
app.router.add_static("/media/", str(MEDIA_DIR))

if __name__ == "__main__":
    print(f"WES server starting on http://0.0.0.0:5000")
    web.run_app(app, host="0.0.0.0", port=5000)
