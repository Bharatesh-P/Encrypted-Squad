# Encrypted Squad — Crash-Safe Embedded Key-Value Database (`kvdb`)

A single-process, single-file, crash-safe embedded key-value storage engine and randomized torture-test oracle written from scratch in standard C++17 for the **"Kill It and Prove It"** 24-Hour Engineering Hackathon.

![KVDB Architecture and On-Disk Byte Layout](diagrams/architecture.svg)

---

## 1. One-Command Offline Build

The project requires **zero external libraries** and **zero network access** (only a standard C++17 compiler and CMake).

### Windows (Command Prompt or PowerShell)
```cmd
build.bat