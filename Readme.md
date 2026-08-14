# Real-Time Person & PPE Detection with ESP32-S3 and Grove Vision AI v2

This project implements a real-time system for detecting human presence **and** checking whether the detected person is wearing the appropriate Personal Protective Equipment (PPE), based on Edge AI computer vision. It pairs an **ESP32-S3** board with a **Grove Vision AI v2** module over the **I2C** bus, using the **ESP-IDF (v6.0+)** framework.

---

## Project Evolution: From Proof of Concept to Target Detection

### Step 1: Validating the Processing Pipeline (Rock - Paper - Scissors)
Initially, a basic model (**Rock / Paper / Scissors**) was used to:
* Validate the physical wiring and the stability of the I2C bus.
* Develop and harden the low-level **SSCMA** protocol driver under ESP-IDF.
* Validate the extraction and parsing of the JSON data stream.

---

### Step 2: Deploying the Target Model (Person Detection)
With the communication layer and software architecture 100% operational, we moved on to the target use case:
1. **AI model flashing:** Using SenseCraft AI's official pre-trained person detection model on the Grove Vision AI v2 module.
2. **Application-level filtering:** Listening for the target identifier `TARGET_PERSON_ID = 0` with a minimum confidence threshold (e.g., `75%`).
3. **Debounce state machine:** Implementing a temporal confirmation system (5 consecutive frames to validate an arrival or departure) to avoid false positives.

---

## 📌 Summary of Technical Achievements & Fixes

1. **SSCMA protocol integration:**
   * Implementation of the I2C transport layer (`FEATURE_TRANSPORT = 0x10`).
   * Splitting I2C packets into chunks of 250 bytes max.

2. **Performance optimization & Base64 image issue resolution:**
   * **Continuous inference without video stream:** Using the `AT+INVOKE=-1,0,1` command (`no_image = 1`). This stops the camera from sending the Base64-encoded JPEG image.
   * **Freed bandwidth:** Removing image data prevents I2C bus saturation and avoids JSON packet truncation.
   * **Memory sizing:** Extending the receive buffer to `4096 bytes` to guarantee complete, error-free parsing via `cJSON`.

3. **State machine & logic processing (`person_tracker`):**
   * **Debounce filtering:** A counter of 5 consecutive frames to validate a person's entry (`STATE_PRESENT`) or exit (`STATE_ABSENT`).
   * **Explicit logs:**

```text
I (5476) EPI_PERSON_DETECTOR: ⏳ Suspicious detection... (1/5 frames)
...
W (6476) EPI_PERSON_DETECTOR: 🚨 [EVENT] PERSON CONFIRMED PRESENT! (Confidence: 94%)
...
I (9726) EPI_PERSON_DETECTOR: 🟢 [EVENT] THE PERSON HAS LEFT.
```

---

## 🚧 Next Steps: PPE Detection — Starting with Glasses

Now that reliable person presence detection is in place, the next milestone is to extend the system toward its actual goal: verifying that a detected person is wearing the required PPE. As a first step in that direction, the upcoming work will focus on **detecting glasses** on the person, in order to:
* Validate that the detection pipeline can reliably distinguish a specific PPE item (glasses) in addition to the person itself.
