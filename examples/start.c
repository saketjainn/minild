/* Program entry point used by all examples.
 *
 * Without the C runtime there is no mainCRTStartup, so minild uses `start`
 * (change it with -e). It calls main() and hands its return value to
 * ExitProcess, which becomes the program's exit code.
 *
 * MinGW gcc inserts a hidden call to __main() at the top of every main();
 * normally the C runtime provides it to run global constructors. We have
 * none, so an empty one is enough.
 */
__declspec(dllimport) void __stdcall ExitProcess(unsigned int code);

int main(void);

void __main(void) {}

void start(void) {
  ExitProcess((unsigned int)main());
}
