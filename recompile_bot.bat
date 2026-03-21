@echo off
echo Setting MSYS2 Library Paths...
set PATH=C:\msys64\mingw64\bin;%PATH%

echo Recompiling C++ Trading Bot...
cd build
mingw32-make

if %errorlevel% neq 0 (
    echo.
    echo COMPILATION FAILED! Check the errors above.
    pause
    exit /b %errorlevel%
)

echo.
echo Compilation Successful!
pause
