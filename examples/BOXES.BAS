' Colour boxes drawn with the PC line-drawing characters
CLS
DEF FNR$(C, N) = STRING$(N, C)
FOR B = 0 TO 5
  X = 3 + B * 12: Y = 3 + B * 3: W = 20: H = 6
  COLOR B + 9, B + 1
  LOCATE Y, X: PRINT CHR$(201); FNR$(205, W - 2); CHR$(187);
  FOR R = 1 TO H - 2
    LOCATE Y + R, X: PRINT CHR$(186); SPACE$(W - 2); CHR$(186);
  NEXT R
  LOCATE Y + H - 1, X: PRINT CHR$(200); FNR$(205, W - 2); CHR$(188);
  LOCATE Y + 2, X + 3: PRINT "Box"; B + 1;
NEXT B
COLOR 7, 0
LOCATE 26, 1: PRINT CHR$(176); CHR$(177); CHR$(178); CHR$(219); " shades and blocks"
SOUND 523, 3: SOUND 659, 3: SOUND 784, 6
