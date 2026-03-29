# ESP32-S3 MNIST Pipeline Integration (Verbose)

## 1. Goal and Scope

This document explains, end-to-end:

1. What was built in the Python workflow (`WES_ml`) for digit preprocessing and inference.
2. How that preprocessing was translated into C for ESP-IDF.
3. How `mnist_tiny_int8.tflite` is integrated into this ESP32-S3 project with TensorFlow Lite Micro.
4. How the custom build job works (automatic model-to-C conversion).
5. How to deploy and optimize on ESP32-S3.

This is written so it can be used as an implementation handover and as a porting guide.

---

## 2. What Was Done Before ESP Integration

In `WES_ml`, we iteratively built and tuned a staged preprocessing pipeline for difficult camera-like digit inputs:

1. Stage 1: edge/background normalization with a soft radial mask.
2. Stage 2: cloud/background suppression.
3. Stage 3: light sharpening.
4. Stage 4: adaptive thresholding + weak-stroke connectivity recovery + final inversion.

Final stage output is:

- **white digit on black background**
- 28x28 grayscale image

The model used is `mnist_tiny_int8.tflite` (int8 quantized).

---

## 3. New Integration Added to This Repository

### 3.1 New files

- `models/mnist_tiny_int8.tflite`
- `scripts/tflite_to_c_array.py`
- `components/digit_inference/CMakeLists.txt`
- `components/digit_inference/idf_component.yml`
- `components/digit_inference/Kconfig.projbuild`
- `components/digit_inference/include/digit_inference.h`
- `components/digit_inference/digit_preprocess.c`
- `components/digit_inference/digit_inference.cc`

### 3.2 Existing files updated

- `main/CMakeLists.txt` (adds dependency on `digit_inference`)
- `main/app_main.c` (adds a startup inference smoke-test)

---

## 4. Custom Build Job (Model Conversion)

A custom build step is implemented in `components/digit_inference/CMakeLists.txt`.

### What it does

At build time, it runs:

- `scripts/tflite_to_c_array.py`

to convert:

- `models/mnist_tiny_int8.tflite`

into generated C files under component build directory:

- `generated/mnist_tiny_model_data.c`
- `generated/mnist_tiny_model_data.h`

These define:

- `g_mnist_tiny_int8_model[]`
- `g_mnist_tiny_int8_model_len`

### Why this is good

1. No manual byte-array generation steps.
2. Model updates are reproducible and tied to normal `idf.py build`.
3. Easy CI usage.

---

## 5. TFLite Micro on ESP-IDF (ESP32-S3)

This integration uses Espressif's `esp-tflite-micro` component via IDF Component Manager.

`components/digit_inference/idf_component.yml` includes:

- `espressif/esp-tflite-micro: ^1.3.5`

This matches current Espressif component registry guidance.

Reference sources used:

- https://components.espressif.com/components/espressif/esp-tflite-micro
- https://github.com/espressif/esp-tflite-micro
- https://github.com/espressif/esp-tflite-micro/tree/master/examples/person_detection

---

## 6. Preprocessing Algorithm (Detailed, C-Ready)

The C implementation is in `components/digit_inference/digit_preprocess.c`.

### Input assumptions

- Input: arbitrary grayscale buffer (`uint8_t*`) with any width/height
- Digit assumption: dark digit on brighter background
- Output: `uint8_t[28*28]`, white digit on black

### Stage A: Resize to 28x28

Bilinear interpolation:

- source -> normalized float in [0, 1]
- fixed output shape 28x28

This gives deterministic model input resolution.

### Stage B: Soft radial prior mask

For pixel radius distance `d` from image center and chosen circle radius `R`:

- `radial = clamp(1 - d/R, 0, 1)`
- smoothstep shaping: `s = radial^2 * (3 - 2*radial)`
- gamma shaping based on transition width

Mask is 1 near center and smoothly decays toward edges.

Purpose:

- reduce edge/background influence
- preserve center region where digit is expected

### Stage C: Edge/background normalization

Compute bright reference using top bright percentile fraction (`top_bright_frac`).

Blend:

- `stage1 = mask * image + (1 - mask) * bright_ref`

This normalizes border/background to stable brightness.

### Stage D: Cloud suppression

Compute darkness drift:

- `darkness = clamp(bright_ref - stage1, 0, 1)`

Gaussian blur (`cloud_sigma`) gives cloud map.

Cloud component:

- subtract 35th percentile floor in masked region

Protect strong strokes via local darkness percentile (p90), then suppress only diffuse cloud-like darkness.

### Stage E: Light sharpening

Unsharp-like operation:

- `sharp = stage2 + amount * (stage2 - gaussian(stage2))`
- blend by mask (sharpen center more than edges)

### Stage F: Adaptive threshold in darkness domain

Convert to darkness:

- `darkness = 1 - stage3`

Hybrid threshold:

- Otsu threshold on masked region
- Gaussian rule threshold: `mean + k * std`
- choose conservative threshold: `min(otsu, gaussian_rule)`
- apply floor and soft transition width

Soft response:

- `response = clamp((darkness - threshold) / soft_w, 0, 1) * mask`

### Stage G: Connected weak-stroke recovery

To avoid cutting faint but connected strokes:

1. `strong` mask: darkness >= threshold
2. `weak` mask: darkness >= (threshold - stroke_link_margin)
3. Binary propagation: expand from `strong` through `weak`
4. Binary closing to reconnect tiny gaps
5. Blend in linked weak response (weighted)

This is the key fix for preserving thin/upper digit strokes.

### Stage H: Final inversion

Final output directly uses `response`:

- white foreground digit
- black background

Convert float [0,1] to uint8 [0,255] for model input.

---

## 7. C API for Application Integration

Public interface in `components/digit_inference/include/digit_inference.h`.

### Main APIs

1. `digit_inference_init()`
2. `digit_preprocess_u8(...)`
3. `digit_inference_run_u8(...)`
4. `digit_inference_run_from_gray_u8(...)`

### Typical usage

1. Acquire grayscale frame (`uint8_t*`, width, height).
2. Call `digit_inference_run_from_gray_u8(...)`.
3. Read predicted class and probability vector.

---

## 8. Runtime TFLM Details

Implemented in `components/digit_inference/digit_inference.cc`.

### Interpreter setup

1. Parse model from generated C array.
2. Build `MicroMutableOpResolver` (minimal op list for this network).
3. Allocate tensor arena.
4. `AllocateTensors()`.

### Input quantization

For int8 input tensor:

- map uint8 [0..255] to float [0..1]
- quantize using tensor `scale` and `zero_point`

### Output handling

- dequantize output logits from int8 (if needed)
- compute softmax probabilities
- return `predicted_digit`, `confidence`, and all class probabilities

---

## 9. ESP32-S3 Optimization Notes

### 9.1 Tensor arena placement

`Kconfig.projbuild` adds:

- `DIGIT_INFERENCE_TENSOR_ARENA_SIZE` (default 98304 bytes)
- `DIGIT_INFERENCE_ARENA_IN_SPIRAM` (default enabled)

Runtime allocation order:

1. PSRAM (if enabled)
2. internal RAM
3. generic 8-bit heap fallback

### 9.2 Why this strategy

- ESP32-S3 + LVGL + drivers can be RAM-constrained.
- Arena in PSRAM helps preserve internal RAM for display/UI/network tasks.

### 9.3 Important ESP-IDF references

- Heap capability allocator: `heap_caps_malloc` / capability flags
  - https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/mem_alloc.html
- External RAM/PSRAM configuration and restrictions
  - https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/external-ram.html

### 9.4 Practical tuning

1. If `AllocateTensors()` fails:
   - increase `DIGIT_INFERENCE_TENSOR_ARENA_SIZE`
2. If UI performance drops:
   - keep arena in PSRAM
   - reduce extra logging and temporary buffers
3. If latency is too high:
   - keep model int8
   - rely on ESP-NN-enabled kernels via `esp-tflite-micro`

---

## 10. Build and Deploy Steps

From repo root:

1. Set target:
   - `idf.py set-target esp32s3`
2. Optional configuration:
   - `idf.py menuconfig`
   - tune `Digit Inference` settings
   - tune PSRAM settings under ESP-IDF config
3. Build:
   - `idf.py build`
4. Flash and monitor:
   - `idf.py -p <PORT> flash monitor`

During build, model conversion runs automatically.

---

## 11. How To Integrate With Real Camera Input Next

1. Capture or receive grayscale image (or convert RGB -> grayscale).
2. Call:
   - `digit_inference_run_from_gray_u8(frame, w, h, NULL, &result, preprocessed_28x28)`
3. Use `result.predicted_digit` and `result.confidence` in your UI/business logic.
4. Optionally display `preprocessed_28x28` on LVGL for debugging.

---

## 12. Current App Hook

`main/app_main.c` now runs a smoke test (`digit_inference_init` + `digit_inference_run_u8`) at startup.

Purpose:

- verifies model conversion + interpreter + arena allocation path quickly
- gives immediate boot log signal that integration is alive

---

## 13. Known Constraints and Tradeoffs

1. Preprocessing currently uses float math for quality parity with Python.
2. This is easiest to validate but can be further optimized to fixed-point later.
3. Component currently uses static workspace for preprocessing to avoid stack overflows.
4. If inference is called from multiple tasks, add synchronization around calls.

---

## 14. Suggested Follow-Up Work (Recommended)

1. Replace smoke test with real frame path from camera/task pipeline.
2. Add a command in monitor/CLI to run inference on stored test frames.
3. Add timing logs per stage (preprocess vs invoke) for FPS budgeting.
4. Add quantized/fixed-point preprocessing path if CPU load becomes critical.
5. Add unit tests with known PNG fixtures and expected class ranking.

---

## 15. Summary

This repository is now prepared for ESP32-S3 deployment of your MNIST int8 model with:

1. Automatic model compilation into firmware (custom build job).
2. Native C preprocessing pipeline matching your tuned Python stages.
3. TensorFlow Lite Micro runtime integration via Espressif component manager.
4. App-level integration hook already wired into startup.

You can now move from offline PNG tests to real on-device camera-driven inference with minimal additional plumbing.
