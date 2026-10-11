@echo off
rem minild test suite for Windows. Needs MinGW gcc on PATH.
rem Run from the project folder:   tests\run_tests.bat
setlocal enabledelayedexpansion
cd /d "%~dp0\.."

if not exist minild.exe call build.bat
if not exist minild.exe exit /b 1

set OUT=build\tests
if not exist build mkdir build
if not exist %OUT% mkdir %OUT%
set PASS=0
set FAIL=0

echo Compiling examples with gcc ...
for %%f in (examples\*.c) do (
  gcc -c -O1 -ffreestanding %%f -o %OUT%\%%~nf.o
  if errorlevel 1 (
    echo   could not compile %%f
    exit /b 1
  )
)
echo.

rem ---- programs that must link, run, exit with a code and print a text
rem      call :run NAME EXPECTED_EXIT_CODE "EXPECTED TEXT" objects...
call :run hello   0 "Hello from a program linked by minild" start hello print
call :run calc    7 ""                                      start main math
call :run data    0 "all checks passed, total = 42"         start data_main data_lib print
call :run stub    0 "stub call through the IAT works"       start stub print
call :run weak    0 "strong greeting"                       start weak_main weak_default weak_strong print
call :run weak2   0 "weak default greeting"                 start weak_main weak_default print

rem ---- links that must fail with a clear message
rem      call :fails NAME "EXPECTED MESSAGE" objects...
call :fails undefined "undefined symbol 'add'"              start main
call :fails duplicate "duplicate symbol 'add'"              start main math dup
call :fails noentry   "entry symbol 'start' is not defined" main math

rem ---- inspector agrees with objdump on the number of relocations
call :inspect_vs_objdump main
call :inspect_vs_objdump data_main
call :inspect_vs_objdump start

rem ---- same objects linked by MinGW's own ld give the same exit code
gcc -nostdlib -e start %OUT%\start.o %OUT%\main.o %OUT%\math.o -lkernel32 -o %OUT%\ref.exe >nul 2>&1
if errorlevel 1 (
  call :bad "reference link with ld failed"
) else (
  %OUT%\ref.exe
  if "!errorlevel!"=="7" (call :good "same result as MinGW ld (exit code 7)") else (call :bad "ld reference gave !errorlevel!")
)

echo.
echo !PASS! passed, !FAIL! failed
if !FAIL! gtr 0 exit /b 1
exit /b 0

rem =====================================================================
:run
set NAME=%~1
set WANT_CODE=%~2
set WANT_TEXT=%~3
set OBJS=
shift & shift & shift
:run_collect
if "%~1"=="" goto run_link
set OBJS=!OBJS! %OUT%\%~1.o
shift
goto run_collect
:run_link
minild.exe link !OBJS! -o %OUT%\!NAME!.exe >nul 2>%OUT%\!NAME!.err
if errorlevel 1 (
  call :bad "!NAME!: link failed"
  type %OUT%\!NAME!.err
  goto :eof
)
%OUT%\!NAME!.exe > %OUT%\!NAME!.txt
set CODE=!errorlevel!
if not "!CODE!"=="!WANT_CODE!" (
  call :bad "!NAME!: exit code !CODE!, expected !WANT_CODE!"
  goto :eof
)
if not "!WANT_TEXT!"=="" (
  findstr /c:"!WANT_TEXT!" %OUT%\!NAME!.txt >nul
  if errorlevel 1 (
    call :bad "!NAME!: output did not contain the expected text"
    goto :eof
  )
)
call :good "!NAME!: links, runs, exit code !CODE!"
goto :eof

rem =====================================================================
:fails
set NAME=%~1
set WANT_MSG=%~2
set OBJS=
shift & shift
:fails_collect
if "%~1"=="" goto fails_link
if exist %OUT%\%~1.o set OBJS=!OBJS! %OUT%\%~1.o
shift
goto fails_collect
:fails_link
minild.exe link !OBJS! -o %OUT%\!NAME!.exe >nul 2>%OUT%\!NAME!.err
if not errorlevel 1 (
  call :bad "!NAME!: link should have failed"
  goto :eof
)
findstr /c:"!WANT_MSG!" %OUT%\!NAME!.err >nul
if errorlevel 1 (
  call :bad "!NAME!: wrong error message"
  type %OUT%\!NAME!.err
  goto :eof
)
call :good "!NAME!: rejected with: !WANT_MSG!"
goto :eof

rem =====================================================================
:inspect_vs_objdump
minild.exe inspect %OUT%\%~1.o > %OUT%\%~1.inspect.txt
objdump -r %OUT%\%~1.o > %OUT%\%~1.objdump.txt
set MINE=0
set THEIRS=0
for /f "delims=" %%l in ('findstr /c:"IMAGE_REL_AMD64" %OUT%\%~1.inspect.txt') do set /a MINE+=1
for /f "delims=" %%l in ('findstr /c:"IMAGE_REL_AMD64" %OUT%\%~1.objdump.txt') do set /a THEIRS+=1
if "!MINE!"=="0" (
  call :bad "inspect %~1.o: no relocations found"
  goto :eof
)
if "!MINE!"=="!THEIRS!" (
  call :good "inspect %~1.o: !MINE! relocations, same as objdump"
) else (
  call :bad "inspect %~1.o: minild found !MINE! relocations, objdump found !THEIRS!"
)
goto :eof

:good
echo   PASS  %~1
set /a PASS+=1
goto :eof

:bad
echo   FAIL  %~1
set /a FAIL+=1
goto :eof
