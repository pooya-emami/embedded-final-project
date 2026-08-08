from ultralytics import YOLO

model = YOLO("yolov8n.pt")

model.export(
    format="onnx",
    opset=11,
    simplify=True,
    dynamic=False,
    imgsz=320
)