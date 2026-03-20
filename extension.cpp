/**
 * csgo-appid-kickmsg - SourceMod Extension
 *
 * Patches the CS:GO dedicated server engine to show a custom kick message
 * when clients with the wrong Steam app ID try to connect.
 *
 * The kick message is read from: addons/sourcemod/configs/csgo_appid_kickmsg.txt
 * Use "sm appid" in server console to view status or "sm appid reload" to reload.
 */

#include "extension.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
  #include <windows.h>
  #include <psapi.h>
  #pragma comment(lib, "psapi.lib")
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #include <dlfcn.h>
  #include <stdio.h>
#endif

// #notob

CCSGOAppIDKickMsg g_AppIDKickMsg;
SMEXT_LINK(&g_AppIDKickMsg);

// Signatures for the appid switch jump table

// Linux:   FF 24 85 ?? ?? ?? ?? 8D B4 26 ?? ?? ?? ?? 31 F6
static const unsigned char g_sigLinux[]  = { 0xFF, 0x24, 0x85, 0x00, 0x00, 0x00, 0x00, 0x8D, 0xB4, 0x26, 0x00, 0x00, 0x00, 0x00, 0x31, 0xF6 };
static const unsigned char g_maskLinux[] = { 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF };
static const size_t g_sigLinuxLen = sizeof(g_sigLinux);

// Windows: FF 24 85 ?? ?? ?? ?? FF 75 ?? 68
static const unsigned char g_sigWin[]    = { 0xFF, 0x24, 0x85, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x75, 0x00, 0x68 };
static const unsigned char g_maskWin[]   = { 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0xFF };
static const size_t g_sigWinLen = sizeof(g_sigWin);

// Patch state 

static bool      g_patched         = false;
static uint32_t  g_origMsgPtr      = 0;       // original appid-reject string pointer
static uint32_t *g_msgPatchAddr    = NULL;     // appid-reject code patch location

static bool      g_steamValPatched    = false;
static uint32_t  g_origSteamValPtr    = 0;       // original "STEAM validation rejected" pointer
static uint32_t *g_steamValPatchAddr  = NULL;     // steam-val code patch location

// Kick message 

#define KICK_MSG_MAX 512
static char g_kickMsgBuffer[KICK_MSG_MAX];

static const char *DEFAULT_KICK_MSG =
    "This server does not support your client version.\n"
    " \n"
    "Use the standalone CSGO client.\n"
    "https://store.steampowered.com/app/4465480/CounterStrikeGlobal_Offensive";

#define CONFIG_FILE "configs/csgo_appid_kickmsg.txt"

//  Helpers 

static bool SetMemoryWritable(void *addr, size_t len)
{
#ifdef _WIN32
    DWORD oldProt;
    return VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProt) != 0;
#else
    long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t base = (uintptr_t)addr & ~((uintptr_t)pageSize - 1);
    size_t totalLen = ((uintptr_t)addr + len) - base;
    return mprotect((void *)base, totalLen, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
}

static void *SigScan(void *base, size_t len, const unsigned char *sig, const unsigned char *mask, size_t sigLen)
{
    unsigned char *start = (unsigned char *)base;
    unsigned char *end   = start + len - sigLen;

    for (unsigned char *p = start; p <= end; ++p)
    {
        bool found = true;
        for (size_t i = 0; i < sigLen; ++i)
        {
            if (mask[i] != 0x00 && (p[i] != sig[i]))
            {
                found = false;
                break;
            }
        }
        if (found)
            return (void *)p;
    }
    return NULL;
}

struct ModuleInfo_t
{
    void  *base;
    size_t size;
};

#ifdef _WIN32

static ModuleInfo_t GetEngineModule()
{
    ModuleInfo_t info;
    info.base = NULL;
    info.size = 0;

    HMODULE hMod = GetModuleHandleA("engine.dll");
    if (!hMod)
        return info;

    MODULEINFO mi;
    memset(&mi, 0, sizeof(mi));
    if (GetModuleInformation(GetCurrentProcess(), hMod, &mi, sizeof(mi)))
    {
        info.base = mi.lpBaseOfDll;
        info.size = mi.SizeOfImage;
    }
    return info;
}

#else

static ModuleInfo_t GetEngineModuleFromMaps(const char *needle)
{
    ModuleInfo_t result;
    result.base = NULL;
    result.size = 0;

    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp)
        return result;

    uintptr_t lo = (uintptr_t)-1, hi = 0;
    char line[512];
    bool found = false;

    while (fgets(line, sizeof(line), fp))
    {
        if (!strstr(line, needle))
            continue;

        found = true;
        uintptr_t start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) == 2)
        {
            if (start < lo) lo = start;
            if (end   > hi) hi = end;
        }
    }
    fclose(fp);

    if (found && lo < hi)
    {
        result.base = (void *)lo;
        result.size = (size_t)(hi - lo);
    }
    return result;
}

static ModuleInfo_t GetEngineModule()
{
    static const char *names[] = {
        "/engine.so",
        "/engine_srv.so",
        NULL
    };

    for (int i = 0; names[i]; i++)
    {
        ModuleInfo_t info = GetEngineModuleFromMaps(names[i]);
        if (info.base && info.size)
            return info;
    }

    ModuleInfo_t empty;
    empty.base = NULL;
    empty.size = 0;
    return empty;
}

#endif

static void LogMsg(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    smutils->LogMessage(myself, "%s", buf);
}

// Check if a virtual address points to a printable ASCII string within the engine module
static bool IsPrintableString(uint32_t addr, void *modBase, size_t modSize, int minLen)
{
    uintptr_t modStart = (uintptr_t)modBase;
    uintptr_t modEnd = modStart + modSize;

    if (addr < (uint32_t)modStart || addr >= (uint32_t)modEnd)
        return false;

    const char *str = (const char *)(uintptr_t)addr;
    int len = 0;
    for (int i = 0; i < 256 && str[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char)str[i];
        if ((c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\r' || c == '\t')
            len++;
        else
            return false;
    }
    return len >= minLen;
}

// Scan code starting at 'start' for an instruction operand that references
// a printable ASCII string within the engine module. Handles common x86-32
// patterns: push imm32, mov reg imm32, mov [esp+N] imm32.
static uint32_t *FindStringRef(unsigned char *start, size_t scanLen, void *modBase, size_t modSize)
{
    // Follow unconditional jmp if the case target is a trampoline
    if (start[0] == 0xE9)
    {
        int32_t rel = *(int32_t *)(start + 1);
        start = start + 5 + rel;
    }
    else if (start[0] == 0xEB)
    {
        int8_t rel = *(int8_t *)(start + 1);
        start = start + 2 + rel;
    }

    for (size_t off = 0; off < scanLen; off++)
    {
        // push imm32: 68 XX XX XX XX
        if (start[off] == 0x68 && off + 5 <= scanLen)
        {
            uint32_t val = *(uint32_t *)(start + off + 1);
            if (IsPrintableString(val, modBase, modSize, 8))
                return (uint32_t *)(start + off + 1);
        }

        // mov reg, imm32: B8..BF XX XX XX XX
        if (start[off] >= 0xB8 && start[off] <= 0xBF && off + 5 <= scanLen)
        {
            uint32_t val = *(uint32_t *)(start + off + 1);
            if (IsPrintableString(val, modBase, modSize, 8))
                return (uint32_t *)(start + off + 1);
        }

        // mov dword ptr [esp+N], imm32: C7 44 24 NN XX XX XX XX
        if (start[off] == 0xC7 && off + 8 <= scanLen &&
            start[off+1] == 0x44 && start[off+2] == 0x24)
        {
            uint32_t val = *(uint32_t *)(start + off + 4);
            if (IsPrintableString(val, modBase, modSize, 8))
                return (uint32_t *)(start + off + 4);
        }
    }
    return NULL;
}

// Find a null-terminated string in the engine module, return its VA
static void *FindStringInModule(void *base, size_t len, const char *needle)
{
    size_t needleLen = strlen(needle);
    unsigned char *start = (unsigned char *)base;
    unsigned char *end   = start + len - needleLen;

    for (unsigned char *p = start; p <= end; ++p)
    {
        if (memcmp(p, needle, needleLen) == 0 && p[needleLen] == '\0')
            return (void *)p;
    }
    return NULL;
}

// Scan the engine module for a code instruction whose imm32 operand equals targetAddr.
// Returns pointer to the imm32 operand in code so we can overwrite it.
static uint32_t *FindAddrRef(void *base, size_t len, uint32_t targetAddr)
{
    unsigned char *start = (unsigned char *)base;
    unsigned char *end   = start + len - 5;

    for (unsigned char *p = start; p <= end; ++p)
    {
        // push imm32: 68 XX XX XX XX
        if (p[0] == 0x68)
        {
            uint32_t val = *(uint32_t *)(p + 1);
            if (val == targetAddr)
                return (uint32_t *)(p + 1);
        }

        // mov reg, imm32: B8..BF XX XX XX XX
        if (p[0] >= 0xB8 && p[0] <= 0xBF)
        {
            uint32_t val = *(uint32_t *)(p + 1);
            if (val == targetAddr)
                return (uint32_t *)(p + 1);
        }

        // mov dword ptr [esp+N], imm32: C7 44 24 NN XX XX XX XX
        if (p[0] == 0xC7 && p + 8 <= (unsigned char *)base + len &&
            p[1] == 0x44 && p[2] == 0x24)
        {
            uint32_t val = *(uint32_t *)(p + 4);
            if (val == targetAddr)
                return (uint32_t *)(p + 4);
        }

        // mov dword ptr [ebp+N], imm32: C7 45 NN XX XX XX XX
        if (p[0] == 0xC7 && p + 7 <= (unsigned char *)base + len &&
            p[1] == 0x45)
        {
            uint32_t val = *(uint32_t *)(p + 3);
            if (val == targetAddr)
                return (uint32_t *)(p + 3);
        }
    }
    return NULL;
}

//  Config file 

static void LoadKickMessage()
{
    char path[PLATFORM_MAX_PATH];
    smutils->BuildPath(Path_SM, path, sizeof(path), CONFIG_FILE);

    FILE *fp = fopen(path, "r");
    if (fp)
    {
        size_t n = fread(g_kickMsgBuffer, 1, KICK_MSG_MAX - 1, fp);
        g_kickMsgBuffer[n] = '\0';
        fclose(fp);

        // Trim trailing whitespace
        while (n > 0 && (g_kickMsgBuffer[n-1] == '\n' || g_kickMsgBuffer[n-1] == '\r' ||
               g_kickMsgBuffer[n-1] == ' '))
        {
            g_kickMsgBuffer[--n] = '\0';
        }

        LogMsg("[APPID] loaded kick message from config (%d chars)", (int)n);
    }
    else
    {
        // Create default config file
        strncpy(g_kickMsgBuffer, DEFAULT_KICK_MSG, KICK_MSG_MAX - 1);
        g_kickMsgBuffer[KICK_MSG_MAX - 1] = '\0';

        fp = fopen(path, "w");
        if (fp)
        {
            fprintf(fp, "%s\n", DEFAULT_KICK_MSG);
            fclose(fp);
            LogMsg("[APPID] created default config at %s", path);
        }
        else
        {
            LogMsg("[APPID] warning: could not write default config to %s", path);
        }
    }
}

//  Root console command: "sm appid" 

void CCSGOAppIDKickMsg::OnRootConsoleCommand(const char *cmdname, const ICommandArgs *args)
{
    if (args->ArgC() >= 3)
    {
        const char *subcmd = args->Arg(2);
        if (strcmp(subcmd, "reload") == 0)
        {
            LoadKickMessage();
            rootconsole->ConsolePrint("[APPID] Kick message reloaded.");
            return;
        }
    }

    rootconsole->ConsolePrint("[APPID] AppID reject patch: %s", g_patched ? "active" : "NOT patched");
    rootconsole->ConsolePrint("[APPID] STEAM validation patch: %s", g_steamValPatched ? "active" : "NOT patched");
    rootconsole->ConsolePrint("[APPID] Current kick message:");
    rootconsole->ConsolePrint("  %s", g_kickMsgBuffer);
    rootconsole->ConsolePrint("");
    rootconsole->ConsolePrint("[APPID] Commands:");
    rootconsole->DrawGenericOption("reload", "Reload kick message from config file");
}

//  SDK callbacks 

bool CCSGOAppIDKickMsg::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
    LoadKickMessage();

    ModuleInfo_t eng = GetEngineModule();
    if (!eng.base || !eng.size)
    {
        snprintf(error, maxlength, "[APPID] failed to locate engine module");
        return false;
    }

#ifdef _WIN32

    void *hit = SigScan(eng.base, eng.size, g_sigWin, g_maskWin, g_sigWinLen);
    if (!hit)
    {
        snprintf(error, maxlength, "[APPID] signature not found (Windows)");
        return false;
    }

    uint32_t jtAddr = *(uint32_t *)((unsigned char *)hit + 3);
    uint32_t *jt    = (uint32_t *)jtAddr;

    // jt[3] is the wrong-appid reject case on Windows
    unsigned char *rejectCode = (unsigned char *)(uintptr_t)jt[3];

    g_msgPatchAddr = FindStringRef(rejectCode, 256, eng.base, eng.size);
    if (!g_msgPatchAddr)
    {
        snprintf(error, maxlength, "[APPID] could not find kick message reference (Windows)");
        return false;
    }

#else

    void *hit = SigScan(eng.base, eng.size, g_sigLinux, g_maskLinux, g_sigLinuxLen);
    if (!hit)
    {
        snprintf(error, maxlength, "[APPID] signature not found (Linux)");
        return false;
    }

    uint32_t jtAddr = *(uint32_t *)((unsigned char *)hit + 3);
    uint32_t *jt    = (uint32_t *)(uintptr_t)jtAddr;

    // jt[4] is the wrong-appid reject case on Linux
    unsigned char *rejectCode = (unsigned char *)(uintptr_t)jt[4];

    g_msgPatchAddr = FindStringRef(rejectCode, 256, eng.base, eng.size);
    if (!g_msgPatchAddr)
    {
        snprintf(error, maxlength, "[APPID] could not find kick message reference (Linux)");
        return false;
    }

#endif

    // Save original and patch the string pointer to our buffer
    g_origMsgPtr = *g_msgPatchAddr;

    LogMsg("[APPID] original kick message: %s", (const char *)(uintptr_t)g_origMsgPtr);

    if (!SetMemoryWritable(g_msgPatchAddr, sizeof(uint32_t)))
    {
        snprintf(error, maxlength, "[APPID] failed to make memory writable");
        return false;
    }

    *g_msgPatchAddr = (uint32_t)(uintptr_t)g_kickMsgBuffer;
    g_patched = true;

    LogMsg("[APPID] appid-reject kick message patched");

    //  Patch 2: "STEAM validation rejected" message 
    void *steamValStr = FindStringInModule(eng.base, eng.size, "STEAM validation rejected\n");
    if (steamValStr)
    {
        uint32_t steamValAddr = (uint32_t)(uintptr_t)steamValStr;
        g_steamValPatchAddr = FindAddrRef(eng.base, eng.size, steamValAddr);

        if (g_steamValPatchAddr)
        {
            g_origSteamValPtr = *g_steamValPatchAddr;

            if (SetMemoryWritable(g_steamValPatchAddr, sizeof(uint32_t)))
            {
                *g_steamValPatchAddr = (uint32_t)(uintptr_t)g_kickMsgBuffer;
                g_steamValPatched = true;
                LogMsg("[APPID] STEAM validation rejected message patched");
            }
            else
            {
                LogMsg("[APPID] warning: could not make STEAM validation string ref writable");
            }
        }
        else
        {
            LogMsg("[APPID] warning: found STEAM validation string but no code reference to it");
        }
    }
    else
    {
        LogMsg("[APPID] warning: could not find STEAM validation rejected string");
    }

    return true;
}

void CCSGOAppIDKickMsg::SDK_OnAllLoaded()
{
    rootconsole->AddRootConsoleCommand3("appid", "CS:GO AppID kick message", this);
}

void CCSGOAppIDKickMsg::SDK_OnUnload()
{
    rootconsole->RemoveRootConsoleCommand("appid", this);

    if (g_steamValPatched && g_steamValPatchAddr)
    {
        SetMemoryWritable(g_steamValPatchAddr, sizeof(uint32_t));
        *g_steamValPatchAddr = g_origSteamValPtr;
        g_steamValPatched = false;
    }

    if (g_patched && g_msgPatchAddr)
    {
        SetMemoryWritable(g_msgPatchAddr, sizeof(uint32_t));
        *g_msgPatchAddr = g_origMsgPtr;
        g_patched = false;
    }

    LogMsg("[APPID] engine restored to original state");
}
