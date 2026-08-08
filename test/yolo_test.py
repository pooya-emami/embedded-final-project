import subprocess
import numpy as np
import cv2

# ---------------------------------------------------------------------------
# CONFIG — adjust these for your setup
# ---------------------------------------------------------------------------

MODEL_PATH = "../models/yolov8n.onnx"

# List available DirectShow devices first if you don't know the exact name:
#   ffmpeg -list_devices true -f dshow -i dummy
# Then paste the exact camera name here.
DEVICE_NAME = "USB2.0 HD UVC WebCam"

CAPTURE_WIDTH = 320
CAPTURE_HEIGHT = 240
CAPTURE_FPS = 30

# Your real relay command doesn't set -q:v explicitly, so FFmpeg uses its
# default MJPEG quality. Leave this as None to match production exactly.
# Set it to a string like "20" if you want to experiment with compression
# levels later.
FFMPEG_QSCALE = None

FRAME_SIZE_HINT = CAPTURE_WIDTH * CAPTURE_HEIGHT * 3  # generous read chunk

# ---------------------------------------------------------------------------


def letterbox_to_square(img, size=320, pad_color=(114, 114, 114)):
    """Resize keeping aspect ratio, then pad to a size x size square.
    Avoids the proportion distortion of a plain stretch resize."""
    h, w = img.shape[:2]
    scale = size / max(h, w)
    new_w, new_h = int(round(w * scale)), int(round(h * scale))
    resized = cv2.resize(img, (new_w, new_h))

    canvas = np.full((size, size, 3), pad_color, dtype=np.uint8)
    top = (size - new_h) // 2
    left = (size - new_w) // 2
    canvas[top:top + new_h, left:left + new_w] = resized
    return canvas


def run_inference(net, img320):
    blob = cv2.dnn.blobFromImage(img320, 1 / 255.0, (320, 320), swapRB=True)
    net.setInput(blob)
    out = net.forward()[0].transpose(1, 0)  # (2100, 84)

    scores = out[:, 4:]
    person_scores = scores[:, 0]
    return person_scores.max(), scores.max()


def start_ffmpeg():
    cmd = [
        "ffmpeg",
        "-f", "dshow",
        "-video_size", f"{CAPTURE_WIDTH}x{CAPTURE_HEIGHT}",
        "-i", f"video={DEVICE_NAME}",
        "-vcodec", "mjpeg",
    ]
    if FFMPEG_QSCALE is not None:
        cmd += ["-q:v", FFMPEG_QSCALE]
    cmd += [
        "-f", "mjpeg",
        "-",  # write MJPEG stream to stdout instead of tcp://...
    ]
    return subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        bufsize=10 ** 8,
    )


def read_jpeg_frame(pipe, buf):
    """Accumulate bytes from the pipe until we have one full JPEG frame
    (from SOI 0xFFD8 to EOI 0xFFD9). Returns (frame_bytes, remaining_buf)
    or (None, buf) if more data is needed."""
    while True:
        soi = buf.find(b"\xff\xd8")
        if soi == -1:
            chunk = pipe.read(FRAME_SIZE_HINT)
            if not chunk:
                return None, buf
            buf += chunk
            continue

        eoi = buf.find(b"\xff\xd9", soi + 2)
        if eoi == -1:
            chunk = pipe.read(FRAME_SIZE_HINT)
            if not chunk:
                return None, buf
            buf += chunk
            continue

        frame = buf[soi:eoi + 2]
        rest = buf[eoi + 2:]
        return frame, rest


def main():
    net = cv2.dnn.readNetFromONNX(MODEL_PATH)
    net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
    net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)

    proc = start_ffmpeg()
    buf = b""

    print("Reading MJPEG from FFmpeg. Press ESC to quit.")

    try:
        while True:
            frame_bytes, buf = read_jpeg_frame(proc.stdout, buf)
            if frame_bytes is None:
                print("FFmpeg pipe closed / no more data.")
                break

            arr = np.frombuffer(frame_bytes, dtype=np.uint8)
            frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
            if frame is None:
                continue  # corrupt frame, skip

            img320 = letterbox_to_square(frame, 320)
            person_score, any_score = run_inference(net, img320)

            display = cv2.resize(img320, (480, 480), interpolation=cv2.INTER_NEAREST)
            lines = [
                f"jpeg bytes: {len(frame_bytes)}",
                f"person score: {person_score:.4f}",
                f"max any score: {any_score:.4f}",
            ]
            for i, line in enumerate(lines):
                cv2.putText(
                    display, line, (10, 25 + i * 25),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2,
                )

            cv2.imshow("FFmpeg -> letterbox 320x320 -> YOLO", display)
            if cv2.waitKey(1) & 0xFF == 27:
                break
    finally:
        proc.terminate()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()