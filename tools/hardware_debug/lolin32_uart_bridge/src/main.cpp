#include <Arduino.h>

// UART bridge: ESP32 (LOLIN32) <-> AK3918AV130 camera
//
// Wiring:
//   GPIO16 (RX1) -> Camera TX
//   GPIO17 (TX1) -> Camera RX
//   GND          -> Camera GND
//
// Auto-exploit: detects U-Boot banner and spams Ctrl+C to interrupt autoboot,
// dropping the camera into the U-Boot shell. After that, all input/output is
// forwarded transparently so you can type U-Boot commands from your PC.

#define CAM_SERIAL Serial1
#define PC_SERIAL  Serial

static const uint8_t CTRL_C = 0x03;

// Number of Ctrl+C bursts and spacing. U-Boot countdown is ~1 s; we spread
// bursts across that window so at least one lands while its input loop is live.
static const int    CTRLC_BURSTS   = 15;
static const int    CTRLC_DELAY_MS = 80;

static String buffer          = "";
static bool   exploitTriggered = false;

void setup() {
  PC_SERIAL.begin(115200);
  CAM_SERIAL.begin(115200, SERIAL_8N1, 16, 17);
  PC_SERIAL.println("[bridge] ESP32 UART exploit bridge ready. Power-cycle the camera now.");
}

void loop() {
  // Camera -> PC (inspect for trigger phrase)
  while (CAM_SERIAL.available()) {
    char c = CAM_SERIAL.read();
    PC_SERIAL.write(c);

    if (!exploitTriggered) {
      buffer += c;

      // "U-Boot" appears in the banner well before the countdown timer starts,
      // giving us the full autoboot window to send Ctrl+C.
      if (buffer.indexOf("U-Boot") != -1) {
        PC_SERIAL.println("\n[bridge] U-Boot detected! Sending Ctrl+C bursts...");

        for (int i = 0; i < CTRLC_BURSTS; i++) {
          CAM_SERIAL.write(CTRL_C);
          delay(CTRLC_DELAY_MS);
        }

        PC_SERIAL.println("[bridge] Done. You should now be at the U-Boot prompt.");
        exploitTriggered = true;
        buffer = "";
      }

      // Keep buffer bounded in case the banner never appears
      if (buffer.length() > 128) {
        buffer = buffer.substring(64);
      }
    }
  }

  // PC -> Camera (transparent passthrough for manual U-Boot commands)
  while (PC_SERIAL.available()) {
    CAM_SERIAL.write(PC_SERIAL.read());
  }
}
