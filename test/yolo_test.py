import subprocess
import numpy as np
import cv2
import onnxruntime as ort
import pathlib
import time
import os

# Suppress ONNX Runtime warnings
os.environ['ORT_LOGGING_LEVEL'] = '3'  # ERROR level only

# ---------------------------------------------------------------------------
# CONFIG — adjust these for your setup
# ---------------------------------------------------------------------------

MODEL_PATH = "../models/blaze.onnx"

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

# BlazeFace configuration
CONF_THRESH = 0.5
IOU_THRESH = 0.3
MAX_DET = 25
INPUT_SIZE = 128  # BlazeFace expects 128x128

# Target FPS for processing
TARGET_FPS = 30
FRAME_TIME = 1.0 / TARGET_FPS  # Time per frame in seconds

# ---------------------------------------------------------------------------


def letterbox_to_square(img, size=INPUT_SIZE, pad_color=(114, 114, 114)):
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


def run_blazeface_inference(session, img128):
    """
    Run BlazeFace inference on a 128x128 image (HWC, BGR format)
    Returns: boxes, scores
    """
    # Convert BGR to RGB (BlazeFace expects RGB)
    img_rgb = cv2.cvtColor(img128, cv2.COLOR_BGR2RGB)
    
    # Normalize to [0, 1]
    img_normalized = img_rgb.astype(np.float32) / 255.0
    
    # Transpose to CHW and add batch dimension: (1, 3, 128, 128)
    img_input = np.transpose(img_normalized, (2, 0, 1))[None, ...]
    
    # Run inference
    try:
        outputs = session.run(None, {
            "image": img_input.astype(np.float32),
            "conf_threshold": np.array([CONF_THRESH], dtype=np.float32),
            "max_detections": np.array([MAX_DET], dtype=np.int64),
            "iou_threshold": np.array([IOU_THRESH], dtype=np.float32),
        })
        
        # Parse outputs
        boxes = np.array([])
        scores = np.array([])
        
        if len(outputs) >= 1:
            boxes_data = outputs[0]
            
            # Check if no detections (shape [1, 16] with garbage values)
            if boxes_data.shape == (1, 16):
                # Check if the box coordinates are reasonable (not all zeros or garbage)
                det = boxes_data[0]
                y1, x1, y2, x2 = det[0:4]
                # If the box is too small or invalid, treat as no detection
                if (y2 - y1) < 0.1 or (x2 - x1) < 0.1:
                    return np.array([]), np.array([])
                # Otherwise, treat as a valid single detection
                boxes = boxes_data
                scores = np.array([1.0])  # Default score
            
            elif boxes_data.ndim == 3 and boxes_data.shape[0] == 1:
                # Shape: (1, N, 16) -> (N, 16)
                boxes = boxes_data[0]
            elif boxes_data.ndim == 2 and boxes_data.shape[0] == 1:
                # Shape: (1, 16) - single detection
                boxes = boxes_data
            elif boxes_data.ndim == 1:
                # Shape: (16,) - single detection
                boxes = boxes_data.reshape(1, -1)
            else:
                boxes = boxes_data
        
        # Get scores
        if len(outputs) > 1:
            scores_data = outputs[1]
            if scores_data is not None:
                if scores_data.ndim == 2:
                    scores = scores_data[0]
                elif scores_data.ndim == 1:
                    scores = scores_data
                elif scores_data.ndim == 0:
                    scores = np.array([scores_data])
                else:
                    scores = np.array([])
        
        # If no scores, create dummy ones
        if len(boxes) > 0 and (len(scores) == 0 or len(scores) != len(boxes)):
            scores = np.ones(len(boxes), dtype=np.float32)
        
        # Ensure boxes is 2D
        if boxes.ndim == 1 and len(boxes) > 0:
            boxes = boxes.reshape(1, -1)
        
        return boxes, scores
        
    except Exception as e:
        print(f"Inference error: {e}")
        return np.array([]), np.array([])


def draw_blazeface_results(img, boxes, scores, original_shape):
    """
    Draw BlazeFace detection results on image - ONLY BOUNDING BOXES, no landmarks
    Returns: image with drawings
    """
    H, W = original_shape[:2]
    
    # Create a copy for drawing
    display_img = img.copy()
    
    # Check if we have any detections
    if boxes is None or len(boxes) == 0:
        return display_img
    
    # Ensure boxes is 2D
    if boxes.ndim == 1:
        boxes = boxes.reshape(1, -1)
    
    # Check box dimensions
    if boxes.shape[1] < 4:
        return display_img
    
    # Apply NMS to remove overlapping boxes
    if len(boxes) > 1:
        rects = []
        scores_list = []
        for i, det in enumerate(boxes):
            y1, x1, y2, x2 = det[0:4]
            x1_px = int(x1 * W)
            y1_px = int(y1 * H)
            x2_px = int(x2 * W)
            y2_px = int(y2 * H)
            rects.append([x1_px, y1_px, x2_px - x1_px, y2_px - y1_px])
            scores_list.append(scores[i] if i < len(scores) else 0.0)
        
        # Apply NMS
        indices = cv2.dnn.NMSBoxes(rects, scores_list, CONF_THRESH, IOU_THRESH)
        
        if len(indices) > 0:
            # Keep only selected boxes
            if isinstance(indices, np.ndarray):
                indices = indices.flatten()
            boxes = boxes[indices]
            scores = scores[indices] if len(scores) > 0 else scores
    
    for i, det in enumerate(boxes):
        try:
            # Parse detection: [top_y, top_x, bot_y, bot_x, ...]
            y1, x1, y2, x2 = det[0:4]
            
            # Scale coordinates back to original image size
            x1_px = int(x1 * W)
            y1_px = int(y1 * H)
            x2_px = int(x2 * W)
            y2_px = int(y2 * H)
            
            # Skip tiny detections or invalid coordinates
            if x2_px - x1_px < 5 or y2_px - y1_px < 5:
                continue
            
            # Get score for this detection
            score = scores[i] if i < len(scores) else 0.0
            
            # Draw bounding box only (no landmarks)
            cv2.rectangle(display_img, (x1_px, y1_px), (x2_px, y2_px), (0, 255, 0), 2)
            cv2.putText(display_img, f"{score:.2f}", (x1_px, y1_px-10),
                       cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)
                
        except Exception as e:
            # Skip invalid detections
            continue
    
    return display_img


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
    # Suppress ONNX Runtime warnings
    ort.set_default_logger_severity(3)  # 3 = ERROR level
    
    # Load BlazeFace model with ONNX Runtime
    print(f"Loading model from: {MODEL_PATH}")
    try:
        session = ort.InferenceSession(
            str(pathlib.Path(MODEL_PATH)),
            providers=['CPUExecutionProvider']
        )
        print("Model loaded successfully!")
        
        # Print model input/output info for debugging
        print("\nModel Inputs:")
        for input_meta in session.get_inputs():
            print(f"  {input_meta.name}: {input_meta.shape} ({input_meta.type})")
        print("\nModel Outputs:")
        for output_meta in session.get_outputs():
            print(f"  {output_meta.name}: {output_meta.shape} ({output_meta.type})")
        print()
        
    except Exception as e:
        print(f"Error loading model: {e}")
        return
    
    # Start FFmpeg process
    proc = start_ffmpeg()
    buf = b""
    
    print("Reading MJPEG from FFmpeg. Press ESC to quit.")
    print(f"Device: {DEVICE_NAME}")
    print(f"Resolution: {CAPTURE_WIDTH}x{CAPTURE_HEIGHT}")
    print(f"Target FPS: {TARGET_FPS}")
    
    frame_count = 0
    display_frame_count = 0
    total_time = 0
    display_total_time = 0
    last_frame_time = time.time()
    fps_start_time = time.time()
    
    try:
        while True:
            # Read frame from FFmpeg
            frame_bytes, buf = read_jpeg_frame(proc.stdout, buf)
            if frame_bytes is None:
                print("FFmpeg pipe closed / no more data.")
                break
            
            # Decode JPEG
            arr = np.frombuffer(frame_bytes, dtype=np.uint8)
            frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
            if frame is None:
                continue  # corrupt frame, skip
            
            frame_count += 1
            
            # Calculate time since last displayed frame
            current_time = time.time()
            time_since_last = current_time - last_frame_time
            
            # Skip frame if we're processing too fast (FPS limiting)
            if time_since_last < FRAME_TIME:
                # Calculate sleep time to maintain target FPS
                sleep_time = FRAME_TIME - time_since_last
                if sleep_time > 0:
                    time.sleep(sleep_time)
                continue
            
            # Update last frame time
            last_frame_time = time.time()
            display_frame_count += 1
            
            # Preprocess: letterbox to 128x128 (BlazeFace input size)
            start_time = time.time()
            img128 = letterbox_to_square(frame, INPUT_SIZE)
            
            # Run BlazeFace inference
            boxes, scores = run_blazeface_inference(session, img128)
            
            # Draw results on original frame (only boxes, no landmarks)
            display_frame = draw_blazeface_results(frame, boxes, scores, frame.shape)
            
            # Calculate inference time
            inference_time = time.time() - start_time
            total_time += inference_time
            
            # Resize for display (optional)
            display = cv2.resize(display_frame, (640, 480), interpolation=cv2.INTER_NEAREST)
            
            # Calculate display FPS (frames actually shown)
            elapsed = time.time() - fps_start_time
            if elapsed > 1.0:
                display_fps = display_frame_count / elapsed
                display_frame_count = 0
                fps_start_time = time.time()
            else:
                display_fps = display_frame_count / elapsed if elapsed > 0 else 0
            
            # Add info overlay
            num_detections = len(boxes) if boxes is not None and len(boxes) > 0 else 0
            lines = [
                f"Frames: {frame_count}",
                f"Faces: {num_detections}",
                f"Inference: {inference_time*1000:.1f}ms",
                f"Display FPS: {display_fps:.1f}",
            ]
            for i, line in enumerate(lines):
                cv2.putText(
                    display, line, (10, 25 + i * 25),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2,
                )
            
            cv2.imshow("BlazeFace - Face Detection (Boxes Only)", display)
            
            # Press ESC to quit
            if cv2.waitKey(1) & 0xFF == 27:
                break
                
    except KeyboardInterrupt:
        print("\nInterrupted by user")
    except Exception as e:
        print(f"Error in main loop: {e}")
        import traceback
        traceback.print_exc()
    finally:
        proc.terminate()
        cv2.destroyAllWindows()
        print(f"\nTotal frames read: {frame_count}")
        print(f"Frames displayed: {display_frame_count}")


if __name__ == "__main__":
    main()