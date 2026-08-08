import cv2
import numpy as np

MODEL_PATH = "../models/yolov5nu.onnx"
IMG_SIZE = 320

net = cv2.dnn.readNetFromONNX(MODEL_PATH)
net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)

def xywh2xyxy(x):
    y = np.copy(x)
    y[:, 0] = x[:, 0] - x[:, 2] / 2
    y[:, 1] = x[:, 1] - x[:, 3] / 2
    y[:, 2] = x[:, 0] + x[:, 2] / 2
    y[:, 3] = x[:, 1] + x[:, 3] / 2
    return y

def nms(boxes, scores, iou_threshold=0.45):
    idxs = cv2.dnn.NMSBoxes(
        bboxes=boxes.tolist(),
        scores=scores.tolist(),
        score_threshold=0.25,
        nms_threshold=iou_threshold
    )
    if idxs is None or len(idxs) == 0:
        return []
    if isinstance(idxs[0], (int, np.integer)):
        return idxs
    return [i[0] for i in idxs]

# Test with a sample image
cap = cv2.VideoCapture(0)
ret, frame = cap.read()
cap.release()

if not ret:
    print("Failed to read frame")
    exit()

print(f"Original frame shape: {frame.shape}")
h, w = frame.shape[:2]

# Method 1: Your Python approach (works)
blob = cv2.dnn.blobFromImage(frame, 1/255.0, (IMG_SIZE, IMG_SIZE), swapRB=True)
net.setInput(blob)
out = net.forward()[0]  # (84, 2100)
out = out.transpose(1, 0)  # (2100, 84)

boxes = out[:, :4]
scores = out[:, 4:]
class_ids = np.argmax(scores, axis=1)
confidences = np.max(scores, axis=1)

mask = confidences > 0.25
boxes = boxes[mask]
confidences = confidences[mask]
class_ids = class_ids[mask]

print(f"Python approach - Detections: {len(boxes)}")

if len(boxes) > 0:
    boxes_xyxy = xywh2xyxy(boxes)
    scale_x = w / IMG_SIZE
    scale_y = h / IMG_SIZE
    boxes_xyxy[:, [0, 2]] *= scale_x
    boxes_xyxy[:, [1, 3]] *= scale_y
    
    keep = nms(boxes_xyxy, confidences)
    print(f"After NMS: {len(keep)}")
    
    for i in keep[:5]:  # Print first 5
        x1, y1, x2, y2 = boxes_xyxy[i].astype(int)
        conf = confidences[i]
        cls = class_ids[i]
        print(f"  Person: ({x1},{y1})-({x2},{y2}), conf={conf:.2f}")

# Method 2: Test with pre-resized image (simulating your C++ approach)
frame_320 = cv2.resize(frame, (320, 320))
h320, w320 = frame_320.shape[:2]

# C++ preprocessing: BGR->RGB, scale to [0,1]
rgb = cv2.cvtColor(frame_320, cv2.COLOR_BGR2RGB)
rgb = rgb.astype(np.float32) / 255.0

# Convert to CHW format (like C++ code)
input_data = np.zeros((1, 3, 320, 320), dtype=np.float32)
for y in range(320):
    for x in range(320):
        p = rgb[y, x]
        input_data[0, 0, y, x] = p[0]  # R
        input_data[0, 1, y, x] = p[1]  # G
        input_data[0, 2, y, x] = p[2]  # B

net.setInput(input_data)
out2 = net.forward()[0]  # (84, 2100)
out2 = out2.transpose(1, 0)  # (2100, 84)

boxes2 = out2[:, :4]
scores2 = out2[:, 4:]
class_ids2 = np.argmax(scores2, axis=1)
confidences2 = np.max(scores2, axis=1)

mask2 = confidences2 > 0.25
boxes2 = boxes2[mask2]
confidences2 = confidences2[mask2]
class_ids2 = class_ids2[mask2]

print(f"\nC++ preprocessing approach - Detections: {len(boxes2)}")

if len(boxes2) > 0:
    boxes_xyxy2 = xywh2xyxy(boxes2)
    scale_x = w / 320
    scale_y = h / 320
    boxes_xyxy2[:, [0, 2]] *= scale_x
    boxes_xyxy2[:, [1, 3]] *= scale_y
    
    keep2 = nms(boxes_xyxy2, confidences2)
    print(f"After NMS: {len(keep2)}")
    
    for i in keep2[:5]:
        x1, y1, x2, y2 = boxes_xyxy2[i].astype(int)
        conf = confidences2[i]
        cls = class_ids2[i]
        print(f"  Person: ({x1},{y1})-({x2},{y2}), conf={conf:.2f}")