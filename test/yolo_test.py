import cv2
import numpy as np
import time

MODEL_PATH = "../models/yolov8n.onnx"
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

cap = cv2.VideoCapture(0)

while True:
    ret, frame = cap.read()
    if not ret:
        break

    h, w = frame.shape[:2]

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

    if len(boxes) == 0:
        cv2.imshow("YOLOv8 ONNX Test", frame)
        if cv2.waitKey(1) == 27:
            break
        continue

    # ----------------------------------------------------
    # YOLOv8 stride + grid correction (P4 + P5 only)
    # ----------------------------------------------------
    strides = [16, 32]
    shapes = [(40, 40), (20, 20)]

    grids = []
    for (gh, gw), stride in zip(shapes, strides):
        yv, xv = np.meshgrid(np.arange(gh), np.arange(gw))
        grid = np.stack((xv, yv), axis=2).reshape(-1, 2)
        grids.append((grid, stride))

    offset = 0
    for grid, stride in grids:
        n = grid.shape[0]
        boxes[offset:offset+n, 0:2] = (boxes[offset:offset+n, 0:2] * 2 - 0.5 + grid) * stride
        boxes[offset:offset+n, 2:4] = (boxes[offset:offset+n, 2:4] * 2) ** 2 * stride
        offset += n

    # Convert to xyxy
    boxes_xyxy = xywh2xyxy(boxes)

    # Scale to original frame
    scale_x = w / IMG_SIZE
    scale_y = h / IMG_SIZE
    boxes_xyxy[:, [0, 2]] *= scale_x
    boxes_xyxy[:, [1, 3]] *= scale_y

    keep = nms(boxes_xyxy, confidences)

    for i in keep:
        x1, y1, x2, y2 = boxes_xyxy[i].astype(int)
        conf = confidences[i]
        cls = class_ids[i]

        cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 255, 0), 2)
        cv2.putText(frame, f"{cls}:{conf:.2f}", (x1, y1 - 5),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)

    cv2.imshow("YOLOv8 ONNX Test", frame)
    if cv2.waitKey(1) == 27:
        break

cap.release()
cv2.destroyAllWindows()
