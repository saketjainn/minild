# minild

**minild** ("mini ld") is a small static linker and object-file analyzer for **Windows x64**, written from scratch in C++17 with no dependencies.

It reads the COFF object files (`.o`) that MinGW `gcc -c` produces, shows what is inside them, resolves symbols across files, and links them into a Windows `.exe` (PE32+) that runs. It can also explain every relocation it applies, step by step.

```
> gcc -c -O1 -ffreestanding start.c main.c math.c
> minild link start.o main.o math.o -o calc.exe
minild: wrote calc.exe
> calc.exe
> echo %ERRORLEVEL%
7
```

## Why

`gcc main.c math.c` hides a whole step: the linker. I wanted to understand what happens between `gcc -c` and a working program, including how a `call add` with four blank bytes becomes a real jump, why the order of libraries matters, and how a program finds `ExitProcess` inside `kernel32.dll`. So I wrote a linker small enough to read in an afternoon, with output that really runs.

## Features

- **Inspect** an object file: sections, symbols (with auxiliary records), relocations and their implicit addends. Relocation counts are checked against `objdump` in the test suite.
- **Resolve** symbols across files: defined/undefined, external vs static (file-private), weak externals, COMMON symbols and COMDAT sections. Errors for **undefined** and **duplicate** symbols name the files involved.
- **Link** into a PE32+ console executable:
  - groups sections by name (`.text$mn` joins `.text`) and lays out `.text`, `.rdata`, `.data`, `.pdata`, `.idata`, `.bss`
  - applies x64 relocations: `ADDR64`, `ADDR32`, `ADDR32NB`, `REL32`, `REL32_1` to `REL32_5`, `SECREL`, `SECTION`
  - builds the **import table** for DLL functions; a direct `call ExitProcess` gets a 6-byte jump stub through the Import Address Table
  - registers exception tables (`.pdata`), so objects built with gcc's default flags link and run
- `--explain` prints the calculation for every relocation, and `--map` prints a link map.

## Quick start (Windows)

1. Install **MinGW-w64** from [winlibs.com](https://winlibs.com): download the *UCRT runtime, Win64* zip, extract it to `C:\mingw64`, and add `C:\mingw64\bin` to your PATH. No installer or admin rights are needed. Check with `gcc --version`.
2. Build minild:
   ```
   build.bat
   ```
3. Run the test suite. It compiles every example with gcc, links it with minild, runs the result, and checks the output:
   ```
   tests\run_tests.bat
   ```
   Expected: `13 passed, 0 failed`.

On Linux or macOS, `./build.sh` builds minild, and `tests/run_tests.sh` runs the same suite with the MinGW cross-compiler and Wine.

## Usage

```
minild inspect FILE.o...                  show sections, symbols and relocations
minild resolve FILE.o... [options]        show how every global symbol is resolved
minild link FILE.o... -o OUT.exe [options]

  -o FILE            output executable (default a.exe)
  -e NAME            entry symbol (default start)
  --explain          print the calculation for every relocation
  --map              print the link map
  --import DLL:FUNC  treat FUNC as imported from DLL (e.g. kernel32.dll:Beep)
```

Programs are linked without the C runtime, so they start at `start` (see `examples/start.c`) and talk to Windows through `kernel32.dll` (`examples/print.c`). Compile them with `gcc -c -O1 -ffreestanding`.

## How it works

```
 .o files ──▶ COFF reader ──▶ symbol resolver ──▶ layout ──▶ relocator ──▶ PE writer ──▶ prog.exe
                  │
                  └──▶ inspector
```

| Stage | File | What it does |
|---|---|---|
| Read | `src/coff_reader.cpp` | Parses the COFF header, section table (long names via the string table), symbol table (18-byte records plus auxiliary records) and relocations |
| Inspect | `src/inspector.cpp` | Prints all of it, including each relocation's implicit addend |
| Resolve | `src/resolver.cpp` | Builds one global symbol table; applies the rules for duplicates, COMMON, weak externals and COMDAT; turns `__imp_X` and known Windows functions into imports |
| Layout | `src/layout.cpp` | Groups and orders sections, assigns addresses (image base `0x140000000`, 0x1000 section and 0x200 file alignment), creates jump stubs and COMMON storage |
| Imports | `src/imports.cpp` | Builds `.idata`: import directory, lookup tables, Import Address Table, hint/name entries |
| Relocate | `src/relocator.cpp` | Patches every relocation (below) |
| Write | `src/pe_writer.cpp` | Writes the DOS header and stub, PE headers, data directories, section table and data; prints the map |

### Relocations, with a real example

`main.c` calls `add()`, which lives in `math.c`. gcc does not know where `add` will end up, so it emits `call` with four zero bytes and a relocation:

```
> minild inspect main.o
Relocations in .text
  Offset    Type                        Addend      Symbol
  0x18      IMAGE_REL_AMD64_REL32       0           add
```

`REL32` means: write the distance from the **end** of the 4-byte field to the symbol, so the CPU's RIP-relative `call` lands on it. COFF has no addend field; the addend is whatever number is already in those bytes.

```
> minild link start.o main.o math.o -o calc.exe --explain
main.o .text+0x18  IMAGE_REL_AMD64_REL32  add
  S (address of add)               = 0x140001050
  A (addend stored in the field)   = 0x0
  P (address being patched)        = 0x140001020 (main.o .text) + 0x18 = 0x140001038
  value = S - (P + 4) + A          = 0x140001050 - 0x14000103c + 0x0 = 0x14
  write 4 bytes: 14 00 00 00
```

| Type | Value written |
|---|---|
| `ADDR64` | S + A (8 bytes) |
| `ADDR32` | S + A (4 bytes, must fit) |
| `ADDR32NB` | S − ImageBase + A, an RVA (used by `.pdata`) |
| `REL32_n` | S − (P + 4 + n) + A (n = bytes of immediate after the field) |

### Calling Windows: the import table

`start.c` calls `ExitProcess`, which lives in `kernel32.dll`. With `__declspec(dllimport)`, gcc emits `call [__imp_ExitProcess]`. minild sees that `__imp_ExitProcess` is not defined anywhere, recognises `ExitProcess` as a `kernel32.dll` function, and builds an import table whose Import Address Table slot is `__imp_ExitProcess`. When Windows loads the program, its loader writes the real address of `ExitProcess` into that slot.

Without `dllimport` (`examples/stub.c`), gcc emits a plain `call GetTickCount`, so minild adds a stub `jmp [IAT slot]` and points the call at it.

### Windows (COFF/PE) vs Linux (ELF)

| | Windows | Linux |
|---|---|---|
| Formats | COFF `.o` → PE `.exe` | ELF `.o` → ELF executable |
| Relocation addend | implicit, stored in the patched bytes | explicit `r_addend` field |
| `call` relocation | `REL32`: S − (P + 4) + A | `R_X86_64_PLT32`: S + A − P, with A = −4 |
| Calling the OS | import table → DLL functions | `syscall` instruction, or libc |
| Dynamic linking | IAT filled by the Windows loader | GOT/PLT filled by `ld.so` |
| File-private symbols | storage class `STATIC` | binding `STB_LOCAL` |

## Tests

`tests/run_tests.bat` (Windows) and `tests/run_tests.sh` (Linux + Wine) check:

| Test | Checks |
|---|---|
| hello, calc, data, stub, weak, weak2 | links, runs, correct output and exit code: cross-file calls and data, pointers in data (`ADDR64`), read-only data, `.bss`, file-private statics, DLL imports, jump stubs, weak vs strong definitions |
| undefined, duplicate, noentry | the link fails with the right message |
| inspect vs objdump | same number of relocations as `objdump -r` |
| ld reference (Windows script) | the same objects linked by MinGW `ld` give the same exit code |

## Limitations

- Static linking of object files only: no `.a` archives yet, no DLL output, no base relocations (`.reloc`), so the image loads at a fixed address.
- Not linked with the C runtime; programs use `start` and `kernel32` directly.
- Debug sections are dropped.
- x86-64 only.

## Project layout

```
src/        the linker (one file per stage, shared types in minild.h)
examples/   small C programs used by the tests
tests/      run_tests.bat (Windows), run_tests.sh (Linux + Wine)
build.bat   build with MinGW g++ on Windows
build.sh    build on Linux/macOS
```

## References

- Microsoft, *PE Format* specification (learn.microsoft.com)
- Ian Lance Taylor, *Linkers* blog series
- John R. Levine, *Linkers and Loaders*

## License

MIT
