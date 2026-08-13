# Binance Futures Arbitrage - C++ Live Engine

Low-latency C++17 implementation of a Binance spot versus quarterly-futures convergence arbitrage engine. This repo is the performance-oriented counterpart to the Python research version, with native networking, JSON parsing, HMAC signing, and a dry-run default.

## What it does

- Loads strategy and exchange settings from `config.json`.
- Connects to Binance public and signed endpoints with CURL/OpenSSL.
- Uses Boost/Asio/Beast support for WebSocket-style market-data plumbing.
- Monitors spot/futures spread, APR, DTE, and fee-adjusted entry/exit thresholds.
- Supports a dry-run mode so order-generation paths can be validated before real execution.

## Project structure

```text
.
├── quarterly_arb_live.cpp   # C++ trading engine
├── CMakeLists.txt           # CMake build definition
├── config.json              # local runtime config with placeholder credentials
├── run_bot.bat              # Windows launcher
├── recompile_bot.bat        # Windows rebuild helper
└── README.txt               # legacy quick-start notes
```

The current repo includes generated build output. For a cleaner open-source layout, keep source, scripts, and config examples in Git, while excluding future `build/` artifacts from commits.

## Agentic workflow

The engine is organized as a deterministic trading automation loop:

- Config agent loads symbol, sizing, spread thresholds, leverage, and dry-run mode.
- Market agent collects spot and futures prices.
- Signal agent checks spread/APR/DTE conditions.
- Risk agent enforces one-position limits, expiry guards, and fee safety.
- Execution agent signs and submits orders only when dry-run is disabled.

This keeps the portfolio story credible: the system is autonomous in monitoring and decision flow, but every decision is traceable to explicit rules.

## Build

Install MSYS2, then from the MSYS2 MinGW 64-bit terminal:

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-curl mingw-w64-x86_64-openssl mingw-w64-x86_64-boost mingw-w64-x86_64-nlohmann-json
```

Build with CMake:

```bash
cmake -S . -B build
cmake --build build
```

Or on Windows, after dependencies are installed:

```text
recompile_bot.bat
```

## Run

Review `config.json`, keep `"dry_run": true` while validating, then run:

```text
run_bot.bat
```

Credentials should be local placeholders or injected securely. Do not commit real API keys.

## Safety notes

- `dry_run` is enabled by default in `config.json`.
- Use restricted Binance keys with withdrawal permissions disabled.
- Validate quantity, leverage, symbol permissions, contract selection, and fee assumptions before live use.
- Keep future build artifacts and local configs out of commits.

