# Real-Time Person & PPE Detection with ESP32-S3 and Grove Vision AI v2

By MATON Erwann

This project implements a real-time Edge AI vision system for **human presence detection** and **Personal Protective Equipment (PPE) compliance checking** (specifically safety glasses). It pairs an **ESP32-S3** microcontroller with a **Grove Vision AI v2** module (Himax WiseEye2 HX6537) over the **I2C** bus, using the **ESP-IDF (v6.0+)** framework.

---

## 🏗️ Project Architecture & Technical Roadmap

```
[ Camera (Grove Vision AI v2) ] --( I2C / SSCMA Protocol )--> [ ESP32-S3 ]
|                                                             |
|-- Edge Impulse FOMO Model (INT8)                            |-- JSON Parsing (cJSON)
|-- Himax Ethos-U55 NPU Acceleration                          |-- Dual-Tracker State Machine
```

---

## 🔑 Key Milestones & Hardware Optimization Journey

### Step 1: Protocol Validation & Bus Stability
* Integrated the low-level **SSCMA** (Seeed SenseCraft AI) protocol over I2C (`FEATURE_TRANSPORT = 0x10`).
* Managed chunked data transfers (maximum payload size: **250 bytes**) with a dedicated **4 KB** receive buffer.
* Optimized bandwidth by disabling Base64 raw image transmission (`AT+INVOKE=-1,0,1` with `no_image = 1`), leaving full bus capacity for detection metadata and preventing JSON truncation.

### Step 2: Overcoming NPU Memory Limits (`Invoke failed` Fix)
* **The problem:** Deploying standard object detection models (like YOLOv5 Nano, ~2.4M params) triggered an `Invoke failed` error due to Himax HX6537 RAM limits.
* **The solution:** Switched architecture to **FOMO** (Faster Objects, More Objects – MobileNetV2 0.35/0.1).
* **Compilation & export:** Quantized the model to INT8 and exported it via Edge Impulse targeting the Arm **Ethos-U55** NPU.
* **Result:** Inference time of **2 ms per frame** with a lightweight memory footprint.

### Step 3: Dual-Class Dataset Strategy for FOMO
Since FOMO computes single-point centroids rather than standard bounding boxes, overlapping target centers (e.g., glasses placed inside a full-head box) cause class confusion. To resolve this:
* **Person annotation** (`TARGET_PERSON_ID = 2`): centered on the upper head / forehead.
* **Glasses annotation** (`TARGET_GLASSES_ID = 1`): centered tightly over the bridge of the nose / eyes.
* **Class mapping alignment:** resolved index offsets between the SenseCraft AI Web Toolkit and the internal TFLite output tensors.

---

## ⚙️ Software Implementation & Filtering Logic

The ESP32-S3 firmware processes incoming JSON arrays containing target bounding descriptors `[x, y, w, h, score, target_id]` and routes them into a **dual-instance debounce state machine** (`tracker_t`).

### Dynamic Class Mapping

| Target ID | Label   | Focus Area              | ESP32 Logic Reference          |
|-----------|---------|--------------------------|---------------------------------|
| 1         | Glasses | Eye / nose bridge area   | `#define TARGET_GLASSES_ID 1`   |
| 2         | Person  | Forehead / top of head   | `#define TARGET_PERSON_ID 2`    |

### Dual-Tracker State Machine Design
To prevent false triggers caused by brief occlusions or single-frame inference noise, the firmware applies independent sequential frame validation:
* **Confirmation threshold** (`CONFIRMATION_FRAMES = 5`): requires 5 consecutive valid detections above **70% confidence** to confirm presence.
* **Absence threshold** (`ABSENCE_FRAMES = 5`): requires 5 consecutive empty frames to declare departure or PPE removal.

---

## 📊 Performance Benchmark

| Stage                  | Time        |
|-------------------------|-------------|
| Preprocessing           | ~1 ms       |
| NPU Inference            | ~2 ms       |
| Postprocessing           | < 1 ms      |
| **Total frame processing** | **~4 ms (~250 FPS capability on Himax NPU)** |

**Detection accuracy:** ~95.7% F1-score on validation set.

---

## 📄 Sample Log Output

```text
I (1250) EPI_SYSTEM: Starting PPE Control System (Person + Safety Glasses)...
W (3420) EPI_SYSTEM: PERSON DETECTED (Confidence: 98%)
W (3670) EPI_SYSTEM: GLASSES CONFIRMED PRESENT! (Confidence: 96%)
...
E (8910) EPI_SYSTEM: GLASSES REMOVED / ABSENT
E (11200) EPI_SYSTEM: PERSON LEFT
```

---

## 🚀 Setup & Flashing Instructions

### Edge Impulse Deployment
1. **Model type:** Object Detection (FOMO MobileNetV2).
2. **Data augmentation:** Enabled.
3. **Export:** TensorFlow Lite (INT8 quantized), passed through the Ethos-U55 Vela compiler.

### Grove Vision AI v2 Flashing
1. Load the output file into the **SenseCraft AI Web Toolkit**, or flash the `.uf2` file directly via bootloader mode.
2. Configure classes in SenseCraft:
   * Class 1 = Glasses
   * Class 2 = Person

### ESP32-S3 Build
1. Wiring: `I2C SDA -> GPIO 5`, `SCL -> GPIO 6`.
2. Compile and flash using ESP-IDF commands:

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```