#include <Arduino.h>
#include <FastLED.h>
#include <shared_hardware_config.h>

#include "EspNowHelper.h"
#include "hardware_config.h"

// =====================
// State
// =====================
enum DeviceState { STATE_IDLE, STATE_CALIBRATED };

// =====================
// Globals
// =====================
uint8_t hubAddress[] = HUB_MAC_ADDRESS;
EspNowHelper espNowHelper;
CRGB leds[NUM_RING_LEDS];
DeviceState deviceState = STATE_IDLE;

// =====================
// Forward Declarations
// =====================
void renderIdleEffect();
void renderCalibratedEffect();
void checkPhotoresistors();
void notifyHub();

// =====================
// Setup / Loop
// =====================
void setup() {
  Serial.begin(115200);

  pinMode(PHOTO_PIN_1, INPUT);
  pinMode(PHOTO_PIN_2, INPUT);

  FastLED.addLeds<WS2812B, LED_RING_PIN, GRB>(leds, NUM_RING_LEDS);
  FastLED.setBrightness(200);  // global brightness cap for all effects
  FastLED.clear(true);

  espNowHelper.begin(DEVICE_ID);
  espNowHelper.addPeer(hubAddress);
  espNowHelper.sendModuleConnected(hubAddress);
}

void loop() {
  switch (deviceState) {
    case STATE_IDLE:
      renderIdleEffect();
      checkPhotoresistors();
      break;
    case STATE_CALIBRATED:
      renderCalibratedEffect();
      break;
  }

  FastLED.show();
  delay(20);
}

// =====================
// Idle Effect
// =====================
void renderIdleEffect() {
  // Slow breathing pulse across all LEDs (cool blue)
  static uint8_t breathBrightness = 0;
  static int8_t breathDir = 1;
  static uint8_t ringOffset = 0;  // which LED the pattern starts from

  // Twinkle state
  static uint8_t twinkleLed = 255;  // 255 = no active twinkle
  static uint8_t twinkleBrightness = 0;
  static unsigned long nextTwinkleAt = 0;

  // Silent phase state
  static unsigned long silentUntil = 0;

  unsigned long now = millis();

  // Silent phase: all LEDs off until the timer expires
  if (now < silentUntil) {
    fill_solid(leds, NUM_RING_LEDS, CRGB::Black);
    return;
  }

  // Advance breath every 4 ticks (~80 ms per step; full cycle ~6 s)
  static uint8_t breathTick = 0;
  if (++breathTick >= 4) {
    breathTick = 0;
    breathBrightness += breathDir;
    if (breathBrightness >= IDLE_MAX_BRIGHTNESS)
      breathDir = -1;
    if (breathBrightness <= 2) {
      breathDir = 1;
      // Shift one LED clockwise when the pulse bottoms out
      ringOffset = (ringOffset + 1) % (NUM_RING_LEDS / IDLE_LED_COUNT);
    }
  }

  // Base colour: dim cool blue on every 3rd LED only (8 of 24), rotated by offset
  CRGB base = CRGB(0, breathBrightness / 5, breathBrightness);
  fill_solid(leds, NUM_RING_LEDS, CRGB::Black);
  for (uint8_t i = ringOffset; i < NUM_RING_LEDS; i += (NUM_RING_LEDS / IDLE_LED_COUNT)) {
    leds[i] = base;
  }

  // Schedule next twinkle when none is running
  // Seed the first twinkle with a delay so it doesn't fire on frame 0
  if (nextTwinkleAt == 0)
    nextTwinkleAt = now + random16(2000, 5000);
  if (twinkleLed == 255 && now >= nextTwinkleAt) {
    twinkleLed = random8(NUM_RING_LEDS);
    twinkleBrightness = 220;
    nextTwinkleAt = now + random16(2000, 5000);
  }

  // Render and fade active twinkle
  if (twinkleLed != 255) {
    leds[twinkleLed] = CRGB(twinkleBrightness / 5, twinkleBrightness / 5, twinkleBrightness);
    twinkleBrightness = scale8(twinkleBrightness, 210);
    if (twinkleBrightness < 4) {
      twinkleLed = 255;
      // Enter silent phase; nextTwinkleAt will be re-seeded when silence ends
      silentUntil = now + IDLE_SILENT_DURATION_MS;
      nextTwinkleAt = 0;
    }
  }
}

// =====================
// Calibrated Effect
// =====================
void renderCalibratedEffect() {
  // A bright energy-flow: two comet tails chasing around the ring in opposite
  // directions, each leaving a coloured trail that shifts through the spectrum.
  static uint16_t headA = 0;                         // Q8.8 fixed-point position, ring A
  static uint16_t headB = (NUM_RING_LEDS / 2) << 8;  // starts opposite
  static uint8_t hueA = 0;
  static uint8_t hueB = 128;

  const uint8_t TAIL_LEN = 8;   // LEDs in each comet tail
  const uint16_t SPEED_A = 90;  // Q8.8 steps per tick (~0.35 LED/tick)
  const uint16_t SPEED_B = 70;
  const uint8_t HUE_STEP = 2;

  // Advance positions
  headA = (headA + SPEED_A) % ((uint16_t)NUM_RING_LEDS << 8);
  headB = (headB + ((uint16_t)NUM_RING_LEDS << 8) - SPEED_B) %
          ((uint16_t)NUM_RING_LEDS << 8);  // travels backwards
  hueA += HUE_STEP;
  hueB += HUE_STEP;

  uint8_t posA = headA >> 8;
  uint8_t posB = headB >> 8;

  // Fade all LEDs down each frame for the trail effect
  fadeToBlackBy(leds, NUM_RING_LEDS, 60);

  // Draw comet A
  for (uint8_t i = 0; i < TAIL_LEN; i++) {
    uint8_t idx = (posA + NUM_RING_LEDS - i) % NUM_RING_LEDS;
    uint8_t brightness = 255 - (i * (255 / TAIL_LEN));
    leds[idx] += CHSV(hueA + i * 4, 230, brightness);
  }

  // Draw comet B (complementary hue)
  for (uint8_t i = 0; i < TAIL_LEN; i++) {
    uint8_t idx = (posB + i) % NUM_RING_LEDS;
    uint8_t brightness = 255 - (i * (255 / TAIL_LEN));
    leds[idx] += CHSV(hueB + 128 + i * 4, 230, brightness);
  }
}

// =====================
// Photoresistor Check
// =====================
void checkPhotoresistors() {
  static unsigned long darkSince = 0;
  static unsigned long lastPrint = 0;
  const unsigned long DEBOUNCE_MS = 1000;

  int val1 = analogRead(PHOTO_PIN_1);
  int val2 = analogRead(PHOTO_PIN_2);

  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    Serial.printf("Photo1: %4d  Photo2: %4d  (threshold: %d)\n", val1, val2, DARKNESS_THRESHOLD);
  }

  if (val1 < DARKNESS_THRESHOLD && val2 < DARKNESS_THRESHOLD) {
    if (darkSince == 0)
      darkSince = millis();
    if (millis() - darkSince >= DEBOUNCE_MS) {
      deviceState = STATE_CALIBRATED;
    }
  } else {
    darkSince = 0;
  }
}

// =====================
// Notify Hub
// =====================
void notifyHub() {
  static bool notified = false;
  if (!notified) {
    espNowHelper.sendModuleUpdated(hubAddress, true);
    notified = true;
  }
}