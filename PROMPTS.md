# Deliverable 4: AI Prompt Log (`PROMPTS.md`)

Chronological log of prompts sent to AI tools during the hackathon by **Encrypted Squad**.

---

### Entry 1
* **Approximate Time:** Hour 0:30 (2026-11-06)
* **Tool:** Google Gemini
* **Prompt:**
> [Uploaded Problem Statement PDF: "Kill It and Prove It — 24-Hour Engineering Hackathon: Problem Statement and Team Guide"]

---

### Entry 2
* **Approximate Time:** Hour 2:15 (2026-11-06)
* **Tool:** Google Gemini
* **Prompt:**
> [Provided `ARCHITECTURE.md` specification and repository tree]
> give kvdb.cmd and torture.cmd code

---

### Entry 3
* **Approximate Time:** Hour 2:30 (2026-11-06)
* **Tool:** Google Gemini
* **Prompt:**
> give build.bat and CMakeLists.txt code

---

### Entry 4
* **Approximate Time:** Hour 3:00 (2026-11-06)
* **Tool:** Google Gemini
* **Prompt:**
> give include/kvbd/├── status.hpp
> │   ├── platform.hpp
> │   ├── crc32.hpp
> │   ├── record.hpp
> │   ├── index.hpp
> │   ├── engine.hpp
> │   ├── recovery.hpp
> │   └── compaction.hpp

---

### Entry 5
* **Approximate Time:** Hour 3:45 (2026-11-06)
* **Tool:** Google Gemini
* **Prompt:**
> give src/ ├── platform_win.cpp
> │   ├── platform_posix.cpp
> │   ├── record.cpp
> │   ├── engine.cpp
> │   ├── recovery.cpp
> │   ├── compaction.cpp
> │   └── main_kvdb.cpp

---

### Entry 6
* **Approximate Time:** Hour 4:30 (2026-11-06)
* **Tool:** Google Gemini
* **Prompt:**
> give torture/
> │   ├── main_torture.cpp
> │   ├── oracle.hpp
> │   ├── oracle.cpp
> │   ├── process_win.hpp
> │   ├── process_win.cpp
> │   ├── process_posix.hpp       # add if supporting POSIX torture runs
> │   ├── process_posix.cpp       # add if supporting POSIX torture runs
> │   ├── modes.hpp
> │   └── modes.cpp

---

### Entry 7
* **Approximate Time:** Hour 5:15 (2026-11-06)
* **Tool:** Google Gemini
* **Prompt:**
> give tests/
> │   ├── test_record.cpp
> │   ├── test_recovery.cpp
> │   ├── test_batch.cpp          # recommended
> │   ├── test_compaction.cpp     # recommended
> │   ├── test_concurrency.cpp    # recommended
> │   ├── test_disk_full.cpp      # required for disk-full acceptance
> │   └── benchmark_recovery.cpp # one-million-key open() benchmark

---

### Entry 8
* **Approximate Time:** Hour 5:45 (2026-11-06)
* **Tool:** Google Gemini
* **Prompt:**
> give diagrams/
> │   └── architecture.svg

---

### Entry 9
* **Approximate Time:** Hour 6:15 (2026-11-06)
* **Tool:** Google Gemini
* **Prompt:**
> give ├── README.md
> ├── BREAKING.md
> ├── PROMPTS.md
> └── ARCHITECTURE.md