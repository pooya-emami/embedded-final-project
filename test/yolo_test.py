import cv2
import numpy as np

MODEL_PATH = "../models/yolov8n.onnx"

net = cv2.dnn.readNetFromONNX(MODEL_PATH)
net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)

img = np.zeros((320, 320, 3), dtype=np.uint8)
blob = cv2.dnn.blobFromImage(img, 1/255.0, (320, 320), swapRB=True)
net.setInput(blob)

out = net.forward()

print("Python output dims:", out.ndim)
print("Python output shape:", out.shape)
