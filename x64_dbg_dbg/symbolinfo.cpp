/**
 @file symbolinfo.cpp

 @brief Implements the symbolinfo class.
 */

#include "symbolinfo.h"
#include "debugger.h"
#include "addrinfo.h"
#include "console.h"
#include "module.h"
#include "label.h"

struct SYMBOLCBDATA
{
    CBSYMBOLENUM cbSymbolEnum;
    void* user;
};

static BOOL CALLBACK EnumSymbols(PSYMBOL_INFO SymInfo, ULONG SymbolSize, PVOID UserContext)
{
    SYMBOLINFO curSymbol;
    memset(&curSymbol, 0, sizeof(SYMBOLINFO));

    curSymbol.addr = (duint)SymInfo->Address;
    curSymbol.decoratedSymbol = (char*)BridgeAlloc(strlen(SymInfo->Name) + 1);
    curSymbol.undecoratedSymbol = (char*)BridgeAlloc(MAX_SYM_NAME);
    strcpy_s(curSymbol.decoratedSymbol, strlen(SymInfo->Name) + 1, SymInfo->Name);

    // Skip bad ordinals
    if(strstr(SymInfo->Name, "Ordinal"))
    {
        // Does the symbol point to the module base?
        if(SymInfo->Address == SymInfo->ModBase)
            return TRUE;
    }

    // Convert a mangled/decorated C++ name to a readable format
    if(!SafeUnDecorateSymbolName(SymInfo->Name, curSymbol.undecoratedSymbol, MAX_SYM_NAME, UNDNAME_COMPLETE))
    {
        BridgeFree(curSymbol.undecoratedSymbol);
        curSymbol.undecoratedSymbol = nullptr;
    }
    else if(!strcmp(curSymbol.decoratedSymbol, curSymbol.undecoratedSymbol))
    {
        BridgeFree(curSymbol.undecoratedSymbol);
        curSymbol.undecoratedSymbol = nullptr;
    }

    SYMBOLCBDATA* cbData = (SYMBOLCBDATA*)UserContext;
    cbData->cbSymbolEnum(&curSymbol, cbData->user);
    return TRUE;
}

void SymEnum(uint Base, CBSYMBOLENUM EnumCallback, void* UserData)
{
    SYMBOLCBDATA symbolCbData;
    symbolCbData.cbSymbolEnum = EnumCallback;
    symbolCbData.user = UserData;

    // Enumerate every single symbol for the module in 'base'
    if(!SafeSymEnumSymbols(fdProcessInfo->hProcess, Base, "*", EnumSymbols, &symbolCbData))
        dputs("SymEnumSymbols failed!");
}

bool SymGetModuleList(std::vector<SYMBOLMODULEINFO>* List)
{
    //
    // Inline lambda enum
    //
    auto EnumModules = [](LPCTSTR ModuleName, DWORD64 BaseOfDll, PVOID UserContext) -> BOOL
    {
        SYMBOLMODULEINFO curModule;
        curModule.base = (duint)BaseOfDll;

        // Terminate module name if one isn't found
        if(!ModNameFromAddr(curModule.base, curModule.name, true))
            curModule.name[0] = '\0';

        ((std::vector<SYMBOLMODULEINFO>*)UserContext)->push_back(curModule);
        return TRUE;
    };

    // Execute the symbol enumerator (Force cast to STDCALL)
    if(!SafeSymEnumerateModules64(fdProcessInfo->hProcess, EnumModules, List))
    {
        dputs("SymEnumerateModules64 failed!");
        return false;
    }

    return true;
}

void SymUpdateModuleList()
{
    // Build the vector of modules
    std::vector<SYMBOLMODULEINFO> modList;

    if(!SymGetModuleList(&modList))
    {
        GuiSymbolUpdateModuleList(0, nullptr);
        return;
    }

    // Create a new array to be sent to the GUI thread
    size_t moduleCount = modList.size();
    SYMBOLMODULEINFO* data = (SYMBOLMODULEINFO*)BridgeAlloc(moduleCount * sizeof(SYMBOLMODULEINFO));

    // Direct copy from std::vector data
    memcpy(data, modList.data(), moduleCount * sizeof(SYMBOLMODULEINFO));

    // Send the module data to the GUI for updating
    GuiSymbolUpdateModuleList((int)moduleCount, data);
}

void SymDownloadAllSymbols(const char* SymbolStore)
{
    // Default to Microsoft's symbol server
    if(!SymbolStore)
        SymbolStore = "http://msdl.microsoft.com/download/symbols";

    // Build the vector of modules
    std::vector<SYMBOLMODULEINFO> modList;

    if(!SymGetModuleList(&modList))
        return;

    // Skip loading if there aren't any found modules
    if(modList.size() <= 0)
        return;

    // Backup the current symbol search path
    char oldSearchPath[MAX_PATH];

    if(!SafeSymGetSearchPath(fdProcessInfo->hProcess, oldSearchPath, MAX_PATH))
    {
        dputs("SymGetSearchPath failed!");
        return;
    }

    // Use the custom server path and directory
    char customSearchPath[MAX_PATH * 2];
    sprintf_s(customSearchPath, "SRV*%s*%s", szSymbolCachePath, SymbolStore);

    if(!SafeSymSetSearchPath(fdProcessInfo->hProcess, customSearchPath))
    {
        dputs("SymSetSearchPath (1) failed!");
        return;
    }

    // Reload
    for(auto & module : modList)
    {
        dprintf("Downloading symbols for %s...\n", module.name);

        wchar_t modulePath[MAX_PATH];
        if(!GetModuleFileNameExW(fdProcessInfo->hProcess, (HMODULE)module.base, modulePath, MAX_PATH))
        {
            dprintf("GetModuleFileNameExW("fhex") failed!\n", module.base);
            continue;
        }

        if(!SafeSymUnloadModule64(fdProcessInfo->hProcess, (DWORD64)module.base))
        {
            dprintf("SymUnloadModule64("fhex") failed!\n", module.base);
            continue;
        }

        if(!SafeSymLoadModuleEx(fdProcessInfo->hProcess, 0, StringUtils::Utf16ToUtf8(modulePath).c_str(), 0, (DWORD64)module.base, 0, 0, 0))
        {
            dprintf("SymLoadModuleEx("fhex") failed!\n", module.base);
            continue;
        }
    }

    // Restore the old search path
    if(!SafeSymSetSearchPath(fdProcessInfo->hProcess, oldSearchPath))
        dputs("SymSetSearchPath (2) failed!");
}

bool SymAddrFromName(const char* Name, uint* Address)
{
    if(!Name || Name[0] == '\0')
        return false;

    if(!Address)
        return false;

    // Skip 'OrdinalXXX'
    if(!_strnicmp(Name, "Ordinal", 7))
        return false;

    // According to MSDN:
    // Note that the total size of the data is the SizeOfStruct + (MaxNameLen - 1) * sizeof(TCHAR)
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)];

    PSYMBOL_INFO symbol = (PSYMBOL_INFO)&buffer;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_LABEL_SIZE;

    if(!SafeSymFromName(fdProcessInfo->hProcess, Name, symbol))
        return false;

    *Address = (uint)symbol->Address;
    return true;
}

const char* SymGetSymbolicName(uint Address)
{
    //
    // This resolves an address to a module and symbol:
    // [modname.]symbolname
    //
    char label[MAX_SYM_NAME];

    // User labels have priority, but if one wasn't found,
    // default to a symbol lookup
    if(!LabelGet(Address, label))
    {
        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)];

        PSYMBOL_INFO symbol = (PSYMBOL_INFO)buffer;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_LABEL_SIZE;

        // Perform a symbol lookup
        DWORD64 displacement = 0;

        if(!SafeSymFromAddr(fdProcessInfo->hProcess, (DWORD64)Address, &displacement, symbol))
            return nullptr;

        // If the symbol wasn't at offset 0 (start from the beginning) ignore it
        if(displacement != 0)
            return nullptr;

        // Terminate the string for sanity
        symbol->Name[symbol->MaxNameLen - 1] = '\0';

        if(!bUndecorateSymbolNames || !SafeUnDecorateSymbolName(symbol->Name, label, MAX_SYM_NAME, UNDNAME_COMPLETE))
            strcpy_s(label, symbol->Name);
    }

    // TODO: FIXME: STATIC VARIABLE
    static char symbolicname[MAX_MODULE_SIZE + MAX_SYM_NAME];
    char modname[MAX_MODULE_SIZE];

    if(ModNameFromAddr(Address, modname, false))
        sprintf_s(symbolicname, "%s.%s", modname, label);
    else
        sprintf_s(symbolicname, "<%s>", label);

    return symbolicname;
}

bool SymGetSourceLine(uint Cip, char* FileName, int* Line)
{
    IMAGEHLP_LINEW64 lineInfo;
    memset(&lineInfo, 0, sizeof(IMAGEHLP_LINE64));

    lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    // Perform a symbol lookup from a specific address
    DWORD displacement;

    if(!SymGetLineFromAddrW64(fdProcessInfo->hProcess, Cip, &displacement, &lineInfo))
        return false;

    String NewFile = StringUtils::Utf16ToUtf8(lineInfo.FileName);

    // Copy line number if requested
    if(Line)
        *Line = lineInfo.LineNumber;

    // Copy file name if requested
    if(FileName)
    {
        // Check if it was a full path
        if(NewFile[1] == ':' && NewFile[2] == '\\')
        {
            // Success: no more parsing
            strcpy_s(FileName, MAX_STRING_SIZE, NewFile.c_str());
            return true;
        }

        // Construct full path from pdb path
        IMAGEHLP_MODULE64 modInfo;
        memset(&modInfo, 0, sizeof(IMAGEHLP_MODULE64));
        modInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);

        if(!SafeSymGetModuleInfo64(fdProcessInfo->hProcess, Cip, &modInfo))
            return false;

        // Strip the name, leaving only the file directory
        char* pdbFileName = strrchr(modInfo.LoadedPdbName, '\\');

        if(pdbFileName)
            pdbFileName[1] = '\0';

        // Copy back to the caller's buffer
        strcpy_s(FileName, MAX_STRING_SIZE, modInfo.LoadedPdbName);
        strcat_s(FileName, MAX_STRING_SIZE, NewFile.c_str());
    }

    return true;
}