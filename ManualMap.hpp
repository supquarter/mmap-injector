#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace ManualMap
{
    struct ModuleInfo
    {
        void* base;
        size_t size;
    };

    bool Inject(DWORD processId, const std::string& dllPath);
    bool InjectFromMemory(DWORD processId, const std::vector<uint8_t>& dllData);
    DWORD FindProcess(const std::string& name);

    bool SendScript(DWORD processId, const std::string& script);
    bool SendScriptToPipe(const std::string& pipeName, const std::string& script);
    std::string GetPipeName(DWORD processId);
}
