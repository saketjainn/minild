/* GetTickCount is declared WITHOUT __declspec(dllimport), so gcc emits a plain
 * `call GetTickCount`. minild satisfies it with a small jump stub that jumps
 * through the Import Address Table slot. */
unsigned long __stdcall GetTickCount(void);
void print(const char *s);

int main(void) {
  unsigned long t1 = GetTickCount();
  unsigned long t2 = GetTickCount();
  print(t2 >= t1 ? "stub call through the IAT works\r\n" : "clock went backwards?\r\n");
  return 0;
}
