#pragma once

// ====================
// This Devices Configuration
// ====================
#define DEVICE_ID 107

#define NUM_RING_LEDS 24
#define LED_RING_PIN 32

// Photoresistors (input-only ADC pins)
#define PHOTO_PIN_1 34
#define PHOTO_PIN_2 35
// ADC value (0-4095) below which a photoresistor is considered "dark"
#define DARKNESS_THRESHOLD 50