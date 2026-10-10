# This is a project to create Yet Another Cave Survey Instrument

N.B at the moment this is a very early prototype, so whilst the commit message says its working, it just means that the M01 laser, ADXL355 and RM3100 magnetometer are being read, its not yet a working survey instrument.

Care is needed in mouting the modules the Axes of the ADXL355 and RM3100 magentometer need to be aligned, X should be forwards along the laser axis, to determine the X axis of the RM3100 take readings as you rotate it and ensure the Max X reading is when the laser is pointing north

## Serial commands and calibration

Set the USB serial monitor to 115200 baud. Commands are single characters and
are case-insensitive:

- `R`: start continuous distance and sensor output.
- `S`: stop streaming; the device idles and waits for commands.
- `M`: begin magnetometer calibration. Slowly rotate through all orientations
  for at least 300 samples, then press any key to fit and save the
  hard-iron offset and soft-iron matrix. A successful calibration is loaded
  automatically after restart.
- `A`: begin six-position accelerometer calibration. Follow the prompts to
  place each ADXL355 axis pointing up and then pointing down;
  keep the instrument still and press Enter at each position. Type `q` to
  cancel. The per-axis offsets and scale factors are saved in non-volatile
  storage and restored at startup.
- `X`: erase the saved magnetometer calibration. Bearing output is unavailable
  until a new magnetometer calibration is completed.
- `H` or `?`: show the command list.

While streaming, each completed measurement is emitted on one line in the
format `DIST_MM=...,AX=...,AY=...,AZ=...,MX=...,MY=...,MZ=...,BEARING=...,INCL=...`.
The X-axis inclination uses all three calibrated accelerometer axes, and the
compass bearing applies hard/soft-iron and tilt compensation.

Perhaps the main use to others will be the working code for an AliExpress cheap laser range finder module, sometimes called an M01 which took a bit of effort to working, there is only one other example online which did not work with my module.

This is the module I have - https://manuals.plus/ae/1005009250844924
This is the other project which did not work with my module - https://github.com/Andres-ros/laser-m01-esp32


Below is the basic test code for the M01 laser module (its easier to see what is going on than the main.c which includes coed for other modules as well

```cpp
// for module m01-v02-20269424
/*
 * M01 Laser Ranging Module + ESP32
 * Corrected for 9-byte binary frame protocol
 */

#define RXD2 33
#define TXD2 32
#define ENA_PIN 25
#define LED_PIN 5  // Changed from 2 to avoid strapping pin issues

unsigned long lastMeasurement = 0;
unsigned long lastBlink = 0;
bool ledState = false;

// Command: Single measurement
// Frame: AA 00 00 20 00 01 00 00 A0
uint8_t measureCmd[] = {0xAA, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00, 0x21};
//uint8_t measureCmd[] =   {0xAA, 0x00, 0x00, 0x22, 0x00, 0x01, 0x00, 0x00, 0x23};


// Command: Laser ON (example - may need verification for your specific unit)
// Some M01 variants use different registers for laser control
//uint8_t laserOnCmd[] = {0xAA, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00, 0x02, 0xA2};
uint8_t laserOnCmd[] = {0xAA, 0x00, 0x01, 0xBE, 0x00, 0x01, 0x00, 0x01, 0xC1};

static inline uint32_t bcd32(const uint8_t* b){
  uint32_t v=0; for(int i=0;i<4;i++){ v=v*100 + ((b[i]>>4)&0x0F)*10 + (b[i]&0x0F); } return v;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(ENA_PIN, OUTPUT);
  digitalWrite(ENA_PIN, HIGH);  // Enable module
  
  pinMode(LED_PIN, OUTPUT);
  
  Serial1.begin(9600 , SERIAL_8N1, RXD2, TXD2);
  
  Serial.println("--- M01 Laser Module Initializing ---");
  
  // Try to turn laser on
  delay(200);
  Serial1.write(laserOnCmd, sizeof(laserOnCmd));
  Serial.println("Sent: Laser ON command");
  delay(1500);
  
  // Clear any response
  while (Serial1.available()) Serial1.read();
}

void loop() {
  // --- Blink LED ---
  if (millis() - lastBlink >= 1000) {
    lastBlink = millis();
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
  }

  // --- Send measurement command every 500ms ---
 // if (millis() - lastMeasurement >= 500) {
    lastMeasurement = millis();
    
    while (Serial1.available()) Serial1.read();
    Serial1.write(laserOnCmd, sizeof(laserOnCmd));
    delay(500);

    // Clear buffer
    while (Serial1.available()) Serial1.read();    
    // Send proper frame
    Serial1.write(measureCmd, sizeof(measureCmd));
    Serial.println("Sent: Measure command");
    delay(2500);
//  }

  // --- Read response (9-byte frame) ---
  if (Serial1.available() >= 13) {
    uint8_t response[13];
    for (int i = 0; i < 13; i++) {
      response[i] = Serial1.read();
    }
    
    // Print raw hex for debugging
    Serial.print("Raw HEX: ");
    for (int i = 0; i < 13; i++) {
      if (response[i] < 0x10) Serial.print("0");
      Serial.print(response[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    
    // Check header
    if (response[0] == 0xAA) {
      // Distance is typically in bytes 3-4 (big endian, millimeters)
      //uint16_t distanceMm = (response[0] << 8) | response[10];
      uint16_t distanceMm = bcd32(&response[6]);
      Serial.print("Distance: ");
      Serial.print(distanceMm);
      Serial.println(" mm");
    } else {
      Serial.println("Invalid frame header");
    }
  }
}
```
