# This is a project to create Yet Another Cave Survey Instrument

N.B at the moment this is a very early prototype, so whilst the commit message says its working, it just means that the M01 laser, ADXL355 and RM3100 magnetometer are being read, its not yet a working survey instrument.

Care is needed in mouting the modules the Axes of the ADXL355 and RM3100 magentometer need to be aligned, X should be forwards along the laser axis, to determine the X axis of the RM3100 take readings as you rotate it and ensure the Max X reading is when the laser is pointing north. On the current hardware the RM3100 module's Y axis is reversed relative to the ADXL355; the firmware negates it when reading (`readMagnetometer()` in `ESP32/src/main.cpp`).

## Button and buzzer

- Push button: momentary switch between GPIO 27 and GND (the internal pull-up
  is used, so no resistor is needed).
- Buzzer: a *passive* piezo transducer (not a self-oscillating "active"
  buzzer, which can only make one tone) between GPIO 26 and GND, ideally with
  a 100 ohm series resistor. For more volume drive it through an NPN
  transistor or MOSFET.

Press the button once to turn the laser on and aim; press again to take the
reading (short beep). When the reading is complete it beeps twice, or plays
three descending tones if the laser shot failed (distance reported as 0) or
no bearing could be calculated. The laser turns off if no reading is taken
within 60 seconds. During calibration the button records the next point: it
takes the shot in shot calibration (`T`), records the position in the rough
accelerometer calibration (Enter), and finishes the rough magnetometer
calibration. Each recorded calibration point beeps twice and a rejected one
plays the error tones.

## Android app survey recording

On the Survey tab, choose New (creates a CSV file wherever you choose) or Open
(continues an existing one; the open survey is reopened when the app
restarts). Enter the From station, e.g. `LB1`; the To station is filled in as
the next number after the highest station with the same prefix, so going back
to `LB3` to survey a branch gives `LB10` if `LB9` is the highest so far. Every
reading from the device, whether from the button or Take Reading, is recorded
as the selected shot: Leg, or L, R, U or D at the From station. L/R/U/D
advance to the next one automatically and are saved with the next leg; tick
"Shoot L, R, U, D before each leg" to start each station at L. After a leg is
recorded, From becomes the old To. Undo Leg removes the last leg and puts it
back for re-shooting. The file has one leg per line:
`From,To,Distance_m,Bearing_deg,Inclination_deg,Left_m,Right_m,Up_m,Down_m,Time`.

## Serial and Bluetooth commands and calibration

Connect over USB serial at 115200 baud or pair with the Bluetooth Classic
serial device named `Yacksi`. Commands are single characters and are
case-insensitive on either connection:

- `R`: take a single reading: the laser measures the distance, then the
  accelerometer and magnetometer are read and one result line is sent.
- `O` then Enter: show the distance offset. `O<mm>` then Enter (e.g. `O125`
  or `O-12`) sets it, in whole millimetres between -2000 and 2000, and saves
  it. The offset is added to every laser distance, so it can correct the
  module against a steel tape (enter tape minus laser) and/or move the datum
  to the rear of the enclosure. The device replies
  `DIST_OFFSET_MM=<mm>,STATUS=CURRENT|SAVED|INVALID|FAILED`, where
  `DIST_OFFSET_MM` is always the offset now in use. In the Android app use
  Distance Offset in the menu.
- `C`: begin shot calibration (Heeb / DistoX method). This is the main
  calibration and corrects offsets, scale, cross-axis errors, magnetometer
  soft/hard iron, and the alignment of both sensors to the laser. Take 14
  groups of 4 shots; all shots in a group are aimed at the same marked point
  with the instrument rolled about 90 degrees about the laser between shots.
  The device prompts for each shot; keys: `T` take shot (hold still; the
  laser switches off briefly while sampling), `U` undo last shot, `F` fit now
  (after at least 8 complete groups), `X` abort. After the fit the RMS error
  is reported (under 0.5 degrees is good); send `Y` to save and apply it or
  `N` to discard. The raw shots are stored with the calibration.
- `F`: refit the calibration from the stored shots (then `Y`/`N`).
- `D`: dump the stored shots and the shot calibration coefficients.
- `E`: stop using the shot calibration (stored shots are kept for `F`).
- `M`: rough magnetometer calibration (ellipsoid fit). Slowly rotate through
  all orientations for at least 300 samples, then press any key to fit and
  save. Used as the starting point for the shot calibration and as the
  fallback when no shot calibration is saved.
- `A`: rough six-position accelerometer calibration. Follow the prompts to
  place each ADXL355 axis pointing up and then pointing down;
  keep the instrument still and press Enter at each position. Type `q` to
  cancel. Used as the starting point for the shot calibration and as the
  fallback when no shot calibration is saved.
- `X`: erase the saved rough magnetometer calibration. A saved shot
  calibration is unaffected; without one, bearing output is unavailable until
  `M` or `C` is run again.
- `H` or `?`: show the command list.

All device messages continue to be sent to USB serial and are also sent over
Bluetooth. Each reading is emitted on one line in the format
`DIST_MM=...,LASER_MM=...,AX=...,AY=...,AZ=...,MX=...,MY=...,MZ=...,BEARING=...,INCL=...,GLEN=...,MLEN=...,CAL=SHOT|ROUGH`.
`DIST_MM` has the distance offset applied; `LASER_MM` is the uncorrected
laser distance.
`MX/MY/MZ` are raw counts with the RM3100 Y axis already negated to match the
ADXL355. `GLEN`/`MLEN` are the calibrated gravity and field magnitudes (1.0
under the shot calibration; a large `MLEN` deviation suggests magnetic
interference). Calibration progress is reported on machine-readable lines
`CAL,STATE=...,KEY=value,...,MSG=text` for the Android app.

Perhaps the main use to others will be the working code for an AliExpress cheap laser range finder module, sometimes called an M01 which took a bit of effort to working, there is only one other example online which did not work with my module.

This is the module I have - https://manuals.plus/ae/1005009250844924
This is the other project which did not work with my module - https://github.com/Andres-ros/laser-m01-esp32


The RM3100 rotation-capture test in `ESP32/test/magTest.cpp` is available as a
PlatformIO environment. From the `ESP32` folder, build it with `pio run -e YacksiMagTest`, or upload it
with `pio run -e YacksiMagTest -t upload`. Connect over serial at 115200 baud:
send `R` to collect up to 5000 X/Y/Z samples in RAM, `S` to stop and dump the
buffer, and `C` to zero the buffer. Sending `S` again dumps the same buffer
again. The default `Yacksi` environment continues to build the main firmware.

A basic standalone test for the M01 laser module is in
[`ESP32/test/laserTest.cpp`](ESP32/test/laserTest.cpp). It's easier to see
what is going on there than in `ESP32/src/main.cpp`, which includes code for
the other modules as well. It is also a PlatformIO environment: from the
`ESP32` folder, build it with `pio run -e YacksiLaserTest`, or upload it with
`pio run -e YacksiLaserTest -t upload`, then watch the serial output at
115200 baud. It turns the laser on, takes a measurement every few seconds and
prints the raw response frame and the distance in mm.
