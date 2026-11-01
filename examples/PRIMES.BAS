' Sieve of Eratosthenes: the primes below 1000
N = 1000
DIM S(N)
FOR I = 2 TO SQR(N)
  IF S(I) = 0 THEN FOR J = I * I TO N STEP I: S(J) = 1: NEXT J
NEXT I
C = 0
FOR I = 2 TO N
  IF S(I) = 0 THEN PRINT RIGHT$("     " + STR$(I), 5);: C = C + 1
NEXT I
PRINT
PRINT C; "primes below"; N
