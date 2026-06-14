# Manual Map Injector

A high-performance x64 Windows DLL injector that uses **manual mapping** (mmap) to load modules into target processes without touching `LoadLibrary`, leaving no trace in the PEB module list. Designed for stealth, reliability, and clean integration.

---

## Features

- **Manual Map Injection** — Fully resolves relocations, imports, and TLS callbacks. No `LoadLibrary` call in the target process.
- **Win32 GUI** — Dark-themed interface with a script editor, Inject/Execute/Clear buttons, and a live status bar.
- **Remote Script Execution** — After injection, sends Lua scripts to the target via a named pipe (`\\.\pipe\YourExecutor_<PID>`). Configurable — change `PIPE_NAME` in `ManualMap.cpp`.
- **Anti-Detection** — Random jitter, memory cleanup with `SecureZeroMemory`, section-based page protection (no all-RWX), and no PEB artifacts.
- **Self-Contained** — No external dependencies. Pure Win32 API. One executable, one DLL.

---

## Architecture

```
injector.exe (GUI)
    │
    │  Manual Map (no LoadLibrary)
    │  │  ─ Parse PE headers
    │  │  ─ Map sections to target
    │  │  ─ Resolve relocations (delta = remote_base − preferred_base)
    │  │  ─ Resolve imports (function offset → target module base)
    │  │  ─ Apply section protection (per IMAGE_SCN_MEM_* flags)
    │  │
    ▼
Target Process
    │
    └─ DllMain(DLL_PROCESS_ATTACH)
         │
         └─ CreateNamedPipe(\\.\pipe\YourExecutor_<PID>)
              │
              └─ ReadFile() → script → TaskScheduler::ScheduleScript()

 User types script, clicks [Execute]
    │
    └─ CreateFile(\\.\pipe\YourExecutor_<PID>)
         │
         └─ WriteFile(script) → DLL receives and runs it
```

### Injection Pipeline

```
Read DLL from disk
    │
    ▼
Validate PE (DOS signature, NT signature, x64, DLL flag)
    │
    ▼
VirtualAllocEx(SizeOfImage, PAGE_EXECUTE_READWRITE)
    │
    ▼
Copy headers + sections into local buffer
    │
    ▼
Apply relocations (IMAGE_REL_BASED_DIR64 / HIGHLOW)
    │
    ▼
Resolve imports:
  └─ GetRemoteModuleBase(moduleName) → target's base address
  └─ LoadLibraryEx(DONT_RESOLVE_REF) → injector's base + offset
  └─ IAT entry = remote_base + (local_func − local_base)
    │
    ▼
WriteProcessMemory for each section
    │
    ▼
VirtualProtectEx (set proper per-section permissions)
    │
    ▼
CreateRemoteThread(entryPoint, imageBase, DLL_PROCESS_ATTACH)
```

---

## Usage

### GUI Mode (default)

Run `injector.exe` — no arguments needed. A dark-theme window appears:

```
┌────────────────────────────────────────────────┐
│  [Inject]  [Execute]  [Clear]                   │
│  ┌──────────────────────────────────────────┐   │
│  │  -- Script editor                        │   │
│  │  print("Hello!")                          │   │
│  │                                          │   │
│  │                                          │   │
│  └──────────────────────────────────────────┘   │
│  Status: Ready — Click Inject to begin          │
└────────────────────────────────────────────────┘
```

| Button | Action |
|--------|--------|
| **Inject** | Manual maps the DLL into `RobloxPlayerBeta.exe` |
| **Execute** | Sends the script contents to the injected DLL via named pipe |
| **Clear** | Clears the script editor |

### CLI Mode

```
injector.exe <dll_path> <process_name>
injector.exe -pid <pid> <dll_path>
```

---

## API Reference (ManualMap.hpp)

```cpp
namespace ManualMap {
    // Inject a DLL from disk by path
    bool Inject(DWORD processId, const std::string& dllPath);

    // Inject a DLL already loaded in memory
    bool InjectFromMemory(DWORD processId, const std::vector<uint8_t>& dllData);

    // Find a process by executable name (case-insensitive)
    DWORD FindProcess(const std::string& name);

    // Send a script to the target process's named pipe
    bool SendScript(DWORD processId, const std::string& script);
}
```

---

## Stealth & Anti-Detection

| Technique | Implementation |
|-----------|---------------|
| **No PEB entry** | Manual mapping skips `LdrLoadDll` — module never appears in `InMemoryOrderModuleList` |
| **No LoadLibrary** | Imports resolved via offset math: `target_base + (local_func − local_base)` |
| **Random jitter** | `Sleep(100–2000ms)` at multiple stages to break timing analysis |
| **Memory cleanup** | `SecureZeroMemory` on DLL data and local PE copy after injection |
| **Section permissions** | Each section gets `PAGE_EXECUTE_READ`, `PAGE_READWRITE`, etc. — never all-RWX |
| **No disk artifacts** | DLL never written to disk in the target process |

---

## Dependencies

- **Windows SDK** (10.0.x)
- **Visual Studio 2022** (v143/v145 toolset)
- **C++20** language standard

Libraries linked: `Psapi.lib`, `comctl32.lib`


