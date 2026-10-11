/* Globals of every kind, used by data_main.c. */
int total = 40;                           /* .data                         */
int *total_ptr = &total;                  /* .data + ADDR64 relocation     */
const char banner[] = "data test: ";      /* .rdata (read-only)            */
int zeroed[4];                            /* .bss or COMMON (zero-filled)  */
static int hidden = 1000;                 /* STATIC: private to this file  */

int bump(int by) {
  hidden += by;
  return hidden;
}
