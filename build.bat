@echo off
rem Build minild.exe with MinGW-w64 g++ (for example from winlibs.com).
rem Run this from the project folder:   build.bat
cd /d "%~dp0"
g++ -std=c++17 -O2 -Wall -Wextra -static -o minild.exe ^
  src\main.cpp src\coff_reader.cpp src\inspector.cpp src\resolver.cpp ^
  src\layout.cpp src\imports.cpp src\relocator.cpp src\pe_writer.cpp
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)
echo Built minild.exe
