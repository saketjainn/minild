void print(const char *s);
void print_int(int v);

extern int total;
extern int *total_ptr;
extern const char banner[];
extern int zeroed[4];
int bump(int by);

static int hidden = 1;   /* same name as in data_lib.c, but STATIC: no clash */

int main(void) {
  int ok = 1;
  *total_ptr += 2;                       /* writes total through the pointer */
  ok &= total == 42;
  ok &= zeroed[0] == 0 && zeroed[3] == 0;
  zeroed[3] = 7;
  ok &= zeroed[3] == 7;
  ok &= bump(5) == 1005;                 /* the other file's 'hidden'  */
  ok &= hidden == 1;                     /* our own 'hidden' untouched */
  print(banner);
  print(ok ? "all checks passed, total = " : "FAILED, total = ");
  print_int(total);
  print("\r\n");
  return ok ? 0 : 1;
}
