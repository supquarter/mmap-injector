#include "ManualMap.hpp"
#include <psapi.h>
#include <tlhelp32.h>
#include <vector>
#include <algorithm>
#include <random>
#include <cstring>

#pragma comment(lib, "psapi.lib")

#define PIPE_NAME "YourExecutor"

namespace
{
    struct REMOTE_MODULE_INFO
    {
        uintptr_t base;
        size_t size;
    };

    static void RandomSleep()
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(100, 2000);
        Sleep(dist(gen));
    }

    static bool GetRemoteModuleInfo(HANDLE hProcess, const char* moduleName, REMOTE_MODULE_INFO& info)
    {
        info = { 0, 0 };

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetProcessId(hProcess));
        if (snapshot != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32 me = { sizeof(me) };
            if (Module32First(snapshot, &me))
            {
                do
                {
                    if (_stricmp(me.szModule, moduleName) == 0)
                    {
                        info.base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                        info.size = me.modBaseSize;
                        CloseHandle(snapshot);
                        return true;
                    }
                } while (Module32Next(snapshot, &me));
            }
            CloseHandle(snapshot);
        }

        DWORD needed = 0;
        HMODULE modules[1024];
        if (EnumProcessModules(hProcess, modules, sizeof(modules), &needed))
        {
            DWORD count = needed / sizeof(HMODULE);
            for (DWORD i = 0; i < count; i++)
            {
                char name[MAX_PATH];
                if (GetModuleFileNameExA(hProcess, modules[i], name, MAX_PATH))
                {
                    const char* baseName = strrchr(name, '\\');
                    baseName = baseName ? baseName + 1 : name;
                    if (_stricmp(baseName, moduleName) == 0)
                    {
                        MODULEINFO mi;
                        if (GetModuleInformation(hProcess, modules[i], &mi, sizeof(mi)))
                        {
                            info.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
                            info.size = mi.SizeOfImage;
                            return true;
                        }
                    }
                }
            }
        }

        return false;
    }

    static bool IsValidPE(const std::vector<uint8_t>& data)
    {
        if (data.size() < sizeof(IMAGE_DOS_HEADER))
            return false;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        if (data.size() < static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64))
            return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(data.data() + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
            return false;

        if (!(nt->FileHeader.Characteristics & IMAGE_FILE_DLL))
            return false;

        return true;
    }

    static bool ApplyRelocations(void* localImage, void* remoteBase)
    {
        const auto* dos = static_cast<PIMAGE_DOS_HEADER>(localImage);
        const auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(
            static_cast<uint8_t*>(localImage) + dos->e_lfanew);

        if (nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED)
            return true;

        const auto& relocDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (!relocDir.VirtualAddress || !relocDir.Size)
            return true;

        uintptr_t preferredBase = nt->OptionalHeader.ImageBase;
        intptr_t adjustDelta = static_cast<intptr_t>(
            reinterpret_cast<uintptr_t>(remoteBase) - preferredBase);

        if (adjustDelta == 0)
            return true;

        auto* relocBase = static_cast<uint8_t*>(localImage) + relocDir.VirtualAddress;
        auto* relocEnd = relocBase + relocDir.Size;
        auto* block = reinterpret_cast<PIMAGE_BASE_RELOCATION>(relocBase);

        while (block->VirtualAddress > 0 && block->SizeOfBlock > 0 &&
            reinterpret_cast<uint8_t*>(block) < relocEnd)
        {
            DWORD count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            auto* entries = reinterpret_cast<WORD*>(block + 1);

            for (DWORD i = 0; i < count; i++)
            {
                if (entries[i] == 0)
                    continue;

                DWORD type = entries[i] >> 12;
                DWORD offset = entries[i] & 0xFFF;

                if (type == IMAGE_REL_BASED_ABSOLUTE)
                    continue;
                else if (type == IMAGE_REL_BASED_DIR64)
                {
                    auto* patchAddr = reinterpret_cast<uintptr_t*>(
                        static_cast<uint8_t*>(localImage) + block->VirtualAddress + offset);
                    *patchAddr += adjustDelta;
                }
                else if (type == IMAGE_REL_BASED_HIGHLOW)
                {
                    auto* patchAddr = reinterpret_cast<DWORD*>(
                        static_cast<uint8_t*>(localImage) + block->VirtualAddress + offset);
                    *patchAddr += static_cast<DWORD>(adjustDelta);
                }
            }

            block = reinterpret_cast<PIMAGE_BASE_RELOCATION>(
                reinterpret_cast<uint8_t*>(block) + block->SizeOfBlock);
        }

        return true;
    }

    static bool ResolveImports(HANDLE hProcess, void* localImage)
    {
        const auto* dos = static_cast<PIMAGE_DOS_HEADER>(localImage);
        const auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(
            static_cast<uint8_t*>(localImage) + dos->e_lfanew);

        const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!importDir.VirtualAddress || !importDir.Size)
            return true;

        auto* importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
            static_cast<uint8_t*>(localImage) + importDir.VirtualAddress);

        while (importDesc->Name)
        {
            const char* moduleName = reinterpret_cast<const char*>(
                static_cast<uint8_t*>(localImage) + importDesc->Name);

            char lowerName[MAX_PATH];
            strcpy_s(lowerName, moduleName);
            _strlwr_s(lowerName);

            REMOTE_MODULE_INFO remoteInfo;
            if (!GetRemoteModuleInfo(hProcess, lowerName, remoteInfo))
            {
                if (!GetRemoteModuleInfo(hProcess, moduleName, remoteInfo))
                    return false;
            }

            HMODULE localModule = LoadLibraryExA(moduleName, nullptr, DONT_RESOLVE_DLL_REFERENCES);
            if (!localModule)
            {
                localModule = LoadLibraryA(moduleName);
                if (!localModule)
                    return false;
            }

            uintptr_t localModuleBase = reinterpret_cast<uintptr_t>(localModule);

            auto* thunkIAT = reinterpret_cast<uintptr_t*>(
                static_cast<uint8_t*>(localImage) + importDesc->FirstThunk);

            uintptr_t* thunkINT;
            if (importDesc->OriginalFirstThunk)
                thunkINT = reinterpret_cast<uintptr_t*>(
                    static_cast<uint8_t*>(localImage) + importDesc->OriginalFirstThunk);
            else
                thunkINT = thunkIAT;

            while (*thunkINT)
            {
                uintptr_t funcAddr = 0;

                if (IMAGE_SNAP_BY_ORDINAL(*thunkINT))
                {
                    funcAddr = reinterpret_cast<uintptr_t>(
                        GetProcAddress(localModule,
                            reinterpret_cast<const char*>(IMAGE_ORDINAL(*thunkINT))));
                }
                else
                {
                    auto* importByName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
                        static_cast<uint8_t*>(localImage) + *thunkINT);
                    funcAddr = reinterpret_cast<uintptr_t>(
                        GetProcAddress(localModule, importByName->Name));
                }

                if (!funcAddr)
                    return false;

                uintptr_t offset = funcAddr - localModuleBase;
                *thunkIAT = remoteInfo.base + offset;

                thunkINT++;
                thunkIAT++;
            }

            FreeLibrary(localModule);
            importDesc++;
        }

        return true;
    }

    static bool WriteMemorySafe(HANDLE hProcess, void* dest, const void* src, size_t size)
    {
        SIZE_T written = 0;
        return WriteProcessMemory(hProcess, dest, src, size, &written) && written == size;
    }
}

namespace ManualMap
{
    DWORD FindProcess(const std::string& name)
    {
        DWORD pid = 0;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return 0;

        PROCESSENTRY32 pe = { sizeof(pe) };
        if (Process32First(snapshot, &pe))
        {
            do
            {
                if (lstrcmpiA(pe.szExeFile, name.c_str()) == 0)
                {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32Next(snapshot, &pe));
        }

        CloseHandle(snapshot);
        return pid;
    }

    std::string GetPipeName(DWORD processId)
    {
        std::string path;
        path += '\\';
        path += '\\';
        path += '.';
        path += '\\';
        path += "pipe";
        path += '\\';
        path += PIPE_NAME;
        path += '_';
        path += std::to_string(processId);
        return path;
    }

    bool SendScriptToPipe(const std::string& pipeName, const std::string& script)
    {
        HANDLE hPipe = CreateFileA(
            pipeName.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (hPipe == INVALID_HANDLE_VALUE)
            return false;

        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

        DWORD bytesWritten = 0;
        BOOL result = WriteFile(hPipe, script.c_str(),
            static_cast<DWORD>(script.size()), &bytesWritten, nullptr);

        FlushFileBuffers(hPipe);
        CloseHandle(hPipe);

        return result && bytesWritten == static_cast<DWORD>(script.size());
    }

    bool SendScript(DWORD processId, const std::string& script)
    {
        return SendScriptToPipe(GetPipeName(processId), script);
    }

    ModuleInfo MapModule(HANDLE hProcess, const std::vector<uint8_t>& dllData)
    {
        ModuleInfo info = { nullptr, 0 };

        if (!IsValidPE(dllData))
            return info;

        auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(const_cast<uint8_t*>(dllData.data()));
        auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
            const_cast<uint8_t*>(dllData.data()) + dos->e_lfanew);

        size_t imageSize = nt->OptionalHeader.SizeOfImage;

        void* remoteBase = VirtualAllocEx(
            hProcess, nullptr, imageSize,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_EXECUTE_READWRITE);

        if (!remoteBase)
            return info;

        std::vector<uint8_t> localCopy(imageSize, 0);
        memcpy(localCopy.data(), dllData.data(),
            (std::min)(static_cast<size_t>(nt->OptionalHeader.SizeOfHeaders), dllData.size()));

        auto* sections = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
        {
            if (sections[i].VirtualAddress && sections[i].SizeOfRawData && sections[i].PointerToRawData)
            {
                size_t copySize = (std::min)(
                    static_cast<size_t>(sections[i].SizeOfRawData),
                    static_cast<size_t>(sections[i].Misc.VirtualSize));
                size_t srcOffset = sections[i].PointerToRawData;
                size_t dstOffset = sections[i].VirtualAddress;

                if (srcOffset < dllData.size() && dstOffset < localCopy.size() &&
                    srcOffset + copySize <= dllData.size() && dstOffset + copySize <= localCopy.size())
                {
                    memcpy(localCopy.data() + dstOffset, dllData.data() + srcOffset, copySize);
                }
            }
        }

        if (!ApplyRelocations(localCopy.data(), remoteBase))
        {
            VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
            return info;
        }

        if (!ResolveImports(hProcess, localCopy.data()))
        {
            VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
            return info;
        }

        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
        {
            DWORD protect = PAGE_EXECUTE_READWRITE;
            if (sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            {
                protect = (sections[i].Characteristics & IMAGE_SCN_MEM_WRITE)
                    ? PAGE_EXECUTE_READWRITE
                    : PAGE_EXECUTE_READ;
            }
            else
            {
                protect = (sections[i].Characteristics & IMAGE_SCN_MEM_WRITE)
                    ? PAGE_READWRITE
                    : PAGE_READONLY;
            }

            void* sectionRemote = static_cast<uint8_t*>(remoteBase) + sections[i].VirtualAddress;
            void* sectionLocal = localCopy.data() + sections[i].VirtualAddress;
            size_t sectionSize = sections[i].Misc.VirtualSize;

            if (!WriteProcessMemory(hProcess, sectionRemote, sectionLocal, sectionSize, nullptr))
            {
                VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                return info;
            }

            DWORD oldProtect;
            VirtualProtectEx(hProcess, sectionRemote, sectionSize, protect, &oldProtect);
        }

        SecureZeroMemory(localCopy.data(), localCopy.size());

        info.base = remoteBase;
        info.size = imageSize;
        return info;
    }

    bool InjectFromMemory(DWORD processId, const std::vector<uint8_t>& dllData)
    {
        RandomSleep();

        HANDLE hProcess = OpenProcess(
            PROCESS_CREATE_THREAD |
            PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE |
            PROCESS_VM_READ,
            FALSE, processId);

        if (!hProcess)
            return false;

        RandomSleep();

        std::vector<uint8_t> workData = dllData;

        ModuleInfo module = MapModule(hProcess, workData);
        if (!module.base)
        {
            CloseHandle(hProcess);
            return false;
        }

        auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(workData.data());
        auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
            workData.data() + dos->e_lfanew);

        void* entryPoint = nt->OptionalHeader.AddressOfEntryPoint
            ? static_cast<uint8_t*>(module.base) + nt->OptionalHeader.AddressOfEntryPoint
            : nullptr;

        if (!entryPoint)
        {
            VirtualFreeEx(hProcess, module.base, 0, MEM_RELEASE);
            CloseHandle(hProcess);
            return false;
        }

        RandomSleep();

        HANDLE hThread = CreateRemoteThread(
            hProcess, nullptr, 0,
            static_cast<LPTHREAD_START_ROUTINE>(entryPoint),
            module.base, 0, nullptr);

        if (!hThread)
        {
            VirtualFreeEx(hProcess, module.base, 0, MEM_RELEASE);
            CloseHandle(hProcess);
            return false;
        }

        if (WaitForSingleObject(hThread, 8000) == WAIT_OBJECT_0)
        {
            DWORD exitCode = 0;
            GetExitCodeThread(hThread, &exitCode);
        }
        else
        {
            TerminateThread(hThread, 0);
        }

        CloseHandle(hThread);
        CloseHandle(hProcess);

        return true;
    }

    bool Inject(DWORD processId, const std::string& dllPath)
    {
        HANDLE hFile = CreateFileA(
            dllPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (hFile == INVALID_HANDLE_VALUE)
            return false;

        DWORD fileSize = GetFileSize(hFile, nullptr);
        if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
        {
            CloseHandle(hFile);
            return false;
        }

        std::vector<uint8_t> dllData(fileSize);
        DWORD bytesRead = 0;

        if (!ReadFile(hFile, dllData.data(), fileSize, &bytesRead, nullptr) || bytesRead != fileSize)
        {
            CloseHandle(hFile);
            return false;
        }

        CloseHandle(hFile);

        bool result = InjectFromMemory(processId, dllData);

        SecureZeroMemory(dllData.data(), dllData.size());

        return result;
    }
}
