import subprocess
import numpy as np
import cv2
import onnxruntime as ort
import pathlib
import time

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
    Run BlazeFace inference using ONNX Runtime on a 128x128 image
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
            
            # Handle different shapes
            if boxes_data is not None:
                # Flatten and reshape to (N, 16)
                if boxes_data.ndim == 0:
                    boxes_data = boxes_data.reshape(1, -1)
                elif boxes_data.ndim == 1:
                    if boxes_data.shape[0] == 16:
                        boxes_data = boxes_data.reshape(1, 16)
                    else:
                        boxes_data = boxes_data.reshape(-1, 16)
                elif boxes_data.ndim == 2:
                    if boxes_data.shape[0] == 1 and boxes_data.shape[1] != 16:
                        # Try to reshape if it's (1, something) that should be (N, 16)
                        if boxes_data.size % 16 == 0:
                            boxes_data = boxes_data.reshape(-1, 16)
                elif boxes_data.ndim == 3:
                    if boxes_data.shape[0] == 1:
                        boxes_data = boxes_data[0]
                    boxes_data = boxes_data.reshape(-1, boxes_data.shape[-1])
                
                # Ensure we have correct shape
                if boxes_data.ndim == 2 and boxes_data.shape[1] == 16:
                    boxes = boxes_data
                elif boxes_data.ndim == 1 and boxes_data.size == 16:
                    boxes = boxes_data.reshape(1, 16)
                elif boxes_data.size > 0:
                    # Try to reshape if possible
                    if boxes_data.size % 16 == 0:
                        boxes = boxes_data.reshape(-1, 16)
            
        if len(outputs) >= 2:
            scores_data = outputs[1]
            if scores_data is not None:
                if scores_data.ndim == 0:
                    scores_data = np.array([scores_data])
                elif scores_data.ndim == 1:
                    scores_data = scores_data
                elif scores_data.ndim == 2:
                    if scores_data.shape[0] == 1:
                        scores_data = scores_data[0]
                scores = scores_data
        else:
            # If no scores output, create dummy ones
            if len(boxes) > 0:
                scores = np.ones(len(boxes), dtype=np.float32)
        
        # Ensure scores match boxes
        if len(boxes) > 0 and len(scores) != len(boxes):
            scores = np.ones(len(boxes), dtype=np.float32)
        
        return boxes, scores
        
    except Exception as e:
        print(f"Inference error: {e}")
        return np.array([]), np.array([])


def nms(boxes, scores, iou_threshold=0.3):
    """
    Apply Non-Maximum Suppression to filter overlapping detections
    """
    if len(boxes) == 0:
        return []
    
    # Convert from normalized [y1, x1, y2, x2] to [x1, y1, x2, y2] for NMS
    boxes_nms = []
    for box in boxes:
        y1, x1, y2, x2 = box[0], box[1], box[2], box[3]
        boxes_nms.append([x1, y1, x2, y2])
    boxes_nms = np.array(boxes_nms)
    
    # Apply NMS
    indices = cv2.dnn.NMSBoxes(
        boxes_nms.tolist(),
        scores.tolist(),
        score_threshold=CONF_THRESH,
        nms_threshold=iou_threshold
    )
    
    if len(indices) > 0:
        return indices.flatten()
    return []


def draw_blazeface_results(img, boxes, scores, original_shape):
    """
    Draw BlazeFace detection results on image
    Returns: image with drawings
    """
    H, W = original_shape[:2]
    
    # Create a copy for drawing
    display_img = img.copy()
    
    # Check if we have any detections
    if boxes is None or len(boxes) == 0:
        cv2.putText(display_img, "No faces detected", (10, 30),
                   cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)
        return display_img
    
    # Ensure boxes is 2D
    if boxes.ndim == 1:
        boxes = boxes.reshape(1, -1)
    
    # Check box dimensions
    if boxes.shape[1] < 16:
        if boxes.size % 16 == 0:
            boxes = boxes.reshape(-1, 16)
        else:
            return display_img
    
    # Apply NMS
    if len(boxes) > 0 and len(scores) > 0:
        keep_indices = nms(boxes, scores, IOU_THRESH)
        if len(keep_indices) > 0:
            boxes = boxes[keep_indices]
            scores = scores[keep_indices]
    
    for i, det in enumerate(boxes):
        try:
            # Parse detection
            det_vals = det[:16]
            (top_y, top_x, bot_y, bot_x,
             ley_x, ley_y, rey_x, rey_y,
             nose_x, nose_y, mou_x, mou_y,
             lea_x, lea_y, rea_x, rea_y) = det_vals
            
            # Scale coordinates back to original image size
            x1 = int(top_x * W)
            y1 = int(top_y * H)
            x2 = int(bot_x * W)
            y2 = int(bot_y * H)
            
            # Skip invalid detections
            if x2 - x1 < 5 or y2 - y1 < 5 or x1 < 0 or y1 < 0 or x2 > W or y2 > H:
                continue
            
            # Get score for this detection
            score = scores[i] if i < len(scores) else 0.0
            
            # Draw bounding box
            cv2.rectangle(display_img, (x1, y1), (x2, y2), (0, 255, 0), 2)
            cv2.putText(display_img, f"face {score:.2f}", (x1, y1-10),
                       cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)
            
            # Draw landmarks
            landmarks = [
                (ley_x, ley_y, (0, 0, 255)),      # Left eye - red
                (rey_x, rey_y, (0, 0, 255)),      # Right eye - red
                (nose_x, nose_y, (255, 0, 0)),    # Nose - blue
                (mou_x, mou_y, (255, 0, 255)),    # Mouth - magenta
                (lea_x, lea_y, (0, 165, 255)),    # Left ear - orange
                (rea_x, rea_y, (0, 165, 255)),    # Right ear - orange
            ]
            
            for lx, ly, color in landmarks:
                cx = int(lx * W)
                cy = int(ly * H)
                if 0 <= cx < W and 0 <= cy < H:
                    cv2.circle(display_img, (cx, cy), 3, color, -1)
                
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
    # Load BlazeFace model with ONNX Runtime
    print(f"Loading model from: {MODEL_PATH}")
    try:
        session = ort.InferenceSession(str(pathlib.Path(MODEL_PATH)), 
                                       providers=['CPUExecutionProvider'])
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
    
    frame_count = 0
    total_time = 0
    
    try:
        while True:
            frame_bytes, buf = read_jpeg_frame(proc.stdout, buf)
            if frame_bytes is None:
                print("FFmpeg pipe closed / no more data.")
                break
            
            # Decode JPEG
            arr = np.frombuffer(frame_bytes, dtype=np.uint8)
            frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
            if frame is None:
                continue  # corrupt frame, skip
            
            # Preprocess: letterbox to 128x128 (BlazeFace input size)
            start_time = time.time()
            img128 = letterbox_to_square(frame, INPUT_SIZE)
            
            # Run BlazeFace inference
            boxes, scores = run_blazeface_inference(session, img128)
            
            # Draw results on original frame
            display_frame = draw_blazeface_results(frame, boxes, scores, frame.shape)
            
            # Calculate FPS
            inference_time = time.time() - start_time
            frame_count += 1
            total_time += inference_time
            
            # Resize for display
            display = cv2.resize(display_frame, (640, 480), interpolation=cv2.INTER_NEAREST)
            
            # Add info overlay
            avg_fps = frame_count / total_time if total_time > 0 else 0
            num_detections = len(boxes) if boxes is not None and len(boxes) > 0 else 0
            lines = [
                f"Frame: {frame_count}",
                f"Detections: {num_detections}",
                f"Inference: {inference_time*1000:.1f}ms",
                f"FPS: {avg_fps:.1f}",
                f"JPEG size: {len(frame_bytes)} bytes",
            ]
            for i, line in enumerate(lines):
                cv2.putText(
                    display, line, (10, 25 + i * 25),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2,
                )
            
            cv2.imshow("BlazeFace - FFmpeg MJPEG Pipeline", display)
            
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
        if total_time > 0:
            print(f"\nProcessed {frame_count} frames")
            print(f"Average FPS: {frame_count/total_time:.1f}")
        else:
            print("\nNo frames were processed successfully")


if __name__ == "__main__":
    main()