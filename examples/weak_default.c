/* A weak definition: used only if no other file defines greeting(). */
__attribute__((weak)) const char *greeting(void) {
  return "weak default greeting\r\n";
}
