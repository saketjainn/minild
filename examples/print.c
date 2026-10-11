/* Tiny output helpers built directly on kernel32 (no C library). */
typedef void *HANDLE;
__declspec(dllimport) HANDLE __stdcall GetStdHandle(unsigned long which);
__declspec(dllimport) int __stdcall WriteFile(HANDLE file, const void *buf, unsigned long len,
                                             unsigned long *written, void *overlapped);

#define STD_OUTPUT_HANDLE ((unsigned long)-11)

void print(const char *s) {
  unsigned long len = 0, written;
  while (s[len]) len++;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, len, &written, 0);
}

void print_int(int v) {
  char buf[16];
  int i = 15;
  int neg = v < 0;
  unsigned int u = neg ? 0u - (unsigned int)v : (unsigned int)v;
  buf[i] = 0;
  do { buf[--i] = (char)('0' + u % 10); u /= 10; } while (u);
  if (neg) buf[--i] = '-';
  print(buf + i);
}
