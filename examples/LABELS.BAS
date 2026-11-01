' LABELS.BAS: a program without line numbers. A label (a name and a
' colon starting a line) is where GOTO, GOSUB, RESTORE and RESUME go.
CLS
PRINT "Labels demo"

menu:
  PRINT
  PRINT "1 Squares  2 Colours  3 Missing file  4 Quit"
  INPUT "Choice"; C
  ON C GOSUB squares, colours, nofile
  IF C = 4 THEN done
  GOTO menu

squares:
  FOR I = 1 TO 5: PRINT I * I;: NEXT
  PRINT
  RETURN

colours:
  RESTORE colourlist
  FOR I = 1 TO 3: READ N$: PRINT N$; " ";: NEXT
  PRINT
  RETURN
colourlist:
  DATA red, green, blue

nofile:
  ON ERROR GOTO oops
  OPEN "NOSUCH.TXT" FOR INPUT AS #1
  CLOSE #1
back: ON ERROR GOTO 0
  RETURN
oops:
  PRINT "No such file: error"; ERR; "in line"; ERL
  RESUME back

done:
  PRINT "Bye"
  END
