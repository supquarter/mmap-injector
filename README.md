# Manual Map Injector For Roblox

A high-performance x64 DLL injector for Roblox designed to bypass **Hyperion** (Roblox's anti-exploit / bypass system). Uses **manual mapping** to load modules into `RobloxPlayerBeta.exe` without touching `LoadLibrary`, leaving zero trace in the PEB module list.

---

## Hyperion Bypass

| Hyperion Detection Vector | Bypass |
|---------------------------|--------|
| **PEB Module Walking** — Hyperion scans `InMemoryOrderModuleList` for unknown modules | Manual mapping skips `LdrLoadDll` entirely — the injected module never appears in the PEB |
| **`LoadLibrary` Hooking** — Hyperion hooks `LdrLoadDll` / `LoadLibraryA/W` to detect injection | All imports are resolved manually via offset math (`target_base + (local_func - local_base)`) — no call to `LoadLibrary` in the target |
| **RWX Memory Scan** — Hyperion flags memory pages with RWX permissions | Each section gets proper per-section protection (`PAGE_EXECUTE_READ`, `PAGE_READWRITE`, etc.) — never all-RWX |
| **Timing Analysis** — Hyperion detects abnormal execution time from injection techniques | Random jitter (`Sleep(100–2000ms)`) at multiple stages to bypass timing heuristics |
| **Memory Forensics** — Hyperion dumps and scans process memory for known DLL signatures | `SecureZeroMemory` wipes DLL data and local PE buffers after injection — no signature left in memory |
| **Disk Artifacts** — Hyperion monitors `CreateFile` / IRP for DLL writes | Module is injected directly from memory — never touches disk in the target process |

---

## Features

- **Manual Map Injection** — Fully resolves relocations, imports, and TLS callbacks. No `LoadLibrary` call in the target.
- **Win32 GUI** — Dark-themed interface with script editor, Inject/Execute/Clear buttons, and live status bar.
- **Remote Script Execution** — Sends Lua scripts to the injected DLL via named pipe (`\\.\pipe\YourExecutor_<PID>`). Configurable — change `PIPE_NAME` in `ManualMap.cpp`.
- **Anti-Detection** — Random jitter, `SecureZeroMemory`, section-based page protection, no PEB artifacts.
- **Self-Contained** — Pure Win32 API. No external dependencies.

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

Run `injector.exe` — no arguments needed:

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
| **Execute** | Sends the script contents to the injected DLL |
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
    bool Inject(DWORD processId, const std::string& dllPath);
    bool InjectFromMemory(DWORD processId, const std::vector<uint8_t>& dllData);
    DWORD FindProcess(const std::string& name);
    bool SendScript(DWORD processId, const std::string& script);
}
```

---

## Dependencies

- **Windows SDK** (10.0.x)
- **Visual Studio 2022** (v143/v145 toolset)
- **C++20**

Libraries linked: `Psapi.lib`, `comctl32.lib`
