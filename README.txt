=================================================
 SPOT-QUARTERLY FUTURES SPREAD ARBITRAGE (C++)
=================================================

This folder contains the complete, ultra-low latency C++ trading engine, capable of natively executing Spot/Futures convergence arbitrage in milliseconds.

-----------------------------
 1. QUICK START (Same PC)
-----------------------------
If you are moving this folder around on your current PC where it was already compiled:
1. Open `config.json` in Notepad or VSCode.
   - Insert your actual `api_key` and `api_secret`.
   - Update `quantity` and `symbol` (e.g. BTCUSDT).
   - Set `"dry_run": false` when you are ready to trade live money.
2. Double-click `run_bot.bat`.
   - The bot will instantly negotiate API keys, fetch 0-ms Websocket streams, and print the high-speed UI matrix to the console!

-----------------------------
 2. RECOMPILING
-----------------------------
If you edit `quarterly_arb_live.cpp` to add new custom math or change the C++ architecture:
1. Double-click `recompile_bot.bat`.
   - This automatically runs `mingw32-make` inside the `build` directory.
   - You only need to recompile if you edit the `.cpp` file. Changing `config.json` DOES NOT require recompiling!

-----------------------------
 3. DEPLOYING ON A NEW WINDOWS PC
-----------------------------
If you zip this folder and move it to a brand new Windows computer, you must install the C++ compiler and networking libraries first:
1. Download and install **MSYS2** from https://www.msys2.org/
2. Open the "MSYS2 MinGW 64-bit" terminal and run this exact command to install GCC, CMake, WebSockets, OpenSSL, and Boost:
   `pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-curl mingw-w64-x86_64-openssl mingw-w64-x86_64-boost mingw-w64-x86_64-nlohmann-json`
3. Hit `Y` to confirm.
4. Once installed, copy this release folder to the new PC's desktop.
5. Double-click `run_bot.bat`! (It automatically detects your MSYS2 library paths based on your installation).
