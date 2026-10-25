#include "Calibration.h"

// Passive piezo driven by an LEDC tone. Sounds are played by a separate
// FreeRTOS task so their timing is unaffected by blocking work in loop(),
// such as the calibration fits.

namespace {

const uint8_t BUZZER_LEDC_CHANNEL = 0;
const uint8_t BUZZER_LEDC_RESOLUTION = 8;

struct Note {
  uint16_t frequency;  // Hz; 0 is a rest
  uint16_t durationMs;
};

// Piezo transducers are loudest around 2-4 kHz.
const Note beepNotes[] = {{2700, 80}};
const Note successNotes[] = {{2700, 80}, {0, 80}, {2700, 80}};
const Note errorNotes[] = {{2400, 180}, {1800, 180}, {1200, 300}};

QueueHandle_t soundQueue = nullptr;

void playNotes(const Note* notes, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    ledcWriteTone(BUZZER_LEDC_CHANNEL, notes[i].frequency);
    vTaskDelay(pdMS_TO_TICKS(notes[i].durationMs));
  }
  ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);
}

void buzzerTask(void*) {
  Sound sound;
  for (;;) {
    if (xQueueReceive(soundQueue, &sound, portMAX_DELAY) != pdTRUE) continue;
    switch (sound) {
      case Sound::Beep: playNotes(beepNotes, sizeof(beepNotes) / sizeof(Note)); break;
      case Sound::Success: playNotes(successNotes, sizeof(successNotes) / sizeof(Note)); break;
      case Sound::Error: playNotes(errorNotes, sizeof(errorNotes) / sizeof(Note)); break;
    }
    // Keep consecutive sounds distinct.
    vTaskDelay(pdMS_TO_TICKS(60));
  }
}

}  // namespace

void initBuzzer() {
  ledcSetup(BUZZER_LEDC_CHANNEL, 2000, BUZZER_LEDC_RESOLUTION);
  ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CHANNEL);
  ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);
  soundQueue = xQueueCreate(4, sizeof(Sound));
  xTaskCreate(buzzerTask, "buzzer", 2048, nullptr, 1, nullptr);
}

void playSound(Sound sound) {
  if (soundQueue == nullptr) return;
  xQueueSend(soundQueue, &sound, 0);  // Drop the sound rather than block if the queue is full.
}
