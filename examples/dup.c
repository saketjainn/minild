/* Defines add() a second time: linking it with math.c must fail. */
int add(int a, int b) {
  return a - b;
}
