import onnxruntime as ort

model = "../models/blaze.onnx"
print("Loading:", model)

session = ort.InferenceSession(model)

print("\n=== INPUTS ===")
for i, inp in enumerate(session.get_inputs()):
    print(f"{i}: name={inp.name}, shape={inp.shape}, type={inp.type}")

print("\n=== OUTPUTS ===")
for i, out in enumerate(session.get_outputs()):
    print(f"{i}: name={out.name}, shape={out.shape}, type={out.type}")
