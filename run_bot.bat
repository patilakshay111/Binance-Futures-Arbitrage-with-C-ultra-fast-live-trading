@echo off
echo Setting MSYS2 Library Paths...
set PATH=C:\msys64\mingw64\bin;%PATH%

echo Starting C++ Trading Bot...
cd build
quarterly_arb_live.exe

pause
