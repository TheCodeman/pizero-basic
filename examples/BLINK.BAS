' BLINK: flash the board's LED (GP25). Ctrl-C stops.
PRINT "Blinking the LED. Ctrl-C stops."
FOR I = 1 TO 30
  PIN LED, 1: PAUSE 200
  PIN LED, 0: PAUSE 300
NEXT I
PRINT "Done"
