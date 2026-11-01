' Guess the number
RANDOMIZE TIMER
newgame:
  CLS: COLOR 14: PRINT "I'm thinking of a number from 1 to 100.": COLOR 7
  N = INT(RND * 100) + 1: T = 0
guess:
  INPUT "Your guess"; G
  T = T + 1
  IF G < N THEN PRINT "Higher...": GOTO guess
  IF G > N THEN PRINT "Lower...": GOTO guess
  COLOR 10: PRINT "Got it in"; T; "tries!": COLOR 7
  BEEP
  INPUT "Play again (Y/N)"; A$
  IF LEFT$(UCASE$(A$), 1) = "Y" THEN newgame
