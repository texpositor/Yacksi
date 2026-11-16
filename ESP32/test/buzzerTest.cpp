#include "Calibration.h"

// Cycles through every sound in Buzzer.cpp using the real buzzer task, and
// prints how long each playSound() call takes to show that it never waits
// for the sound to finish.
//
// Serial commands:
//   b / s / e  play Beep / Success / Error
//   f          flood: queue 8 sounds at once (queue holds 4, rest are dropped)

static void play(Sound sound, const char* name) {
  uint32_t t0 = micros();
  playSound(sound);
  Serial.printf("%-8s playSound() returned after %lu us\n", name, (unsigned long)(micros() - t0));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  initBuzzer();
  Serial.println("Buzzer test: cycling Beep, Success, Error. Commands: b s e f");
}

void loop() {
  if (Serial.available()) {
    switch (Serial.read()) {
      case 'b': play(Sound::Beep, "Beep"); break;
      case 's': play(Sound::Success, "Success"); break;
      case 'e': play(Sound::Error, "Error"); break;
      case 'f':
        for (int i = 0; i < 8; ++i) play(Sound::Beep, "Flood");
        break;
    }
  }

  static uint32_t last = 0;
  static uint8_t next = 0;
  if (millis() - last >= 1500) {
    last = millis();
    switch (next++ % 3) {
      case 0: play(Sound::Beep, "Beep"); break;
      case 1: play(Sound::Success, "Success"); break;
      case 2: play(Sound::Error, "Error"); break;
    }
  }
}
