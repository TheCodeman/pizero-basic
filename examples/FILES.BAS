' FILES: sequential and random-access files
PRINT "Writing NOTES.TXT..."
OPEN "NOTES.TXT" FOR OUTPUT AS #1
PRINT #1, "Shopping list"
WRITE #1, "Apples", 6, "Milk, semi-skimmed", 2
CLOSE #1
OPEN "NOTES.TXT" FOR APPEND AS #1
WRITE #1, "Bread", 1, "Tea", 80
CLOSE #1
PRINT "Reading it back:"
OPEN "NOTES.TXT" FOR INPUT AS #1
LINE INPUT #1, T$: PRINT "Title: "; T$
WHILE NOT EOF(1)
  INPUT #1, ITEM$, N
  PRINT "  "; ITEM$; TAB(26); N
WEND
CLOSE #1
PRINT
PRINT "Random access: PEOPLE.DAT, 22-byte records"
OPEN "PEOPLE.DAT" FOR RANDOM AS #2 LEN = 22
FIELD #2, 20 AS NM$, 2 AS AGE$
FOR I = 1 TO 3
  READ A$, B
  LSET NM$ = A$: LSET AGE$ = MKI$(B)
  PUT #2, I
NEXT I
DATA "Ada", 36, "Grace", 85, "Alan", 41
GET #2, 2
PRINT "Record 2: ["; NM$; "] age"; CVI(AGE$)
PRINT "Records in the file:"; LOF(2) / 22
CLOSE #2
KILL "PEOPLE.DAT"
PRINT "Done. NOTES.TXT is left on the disk: KILL it to remove it."
