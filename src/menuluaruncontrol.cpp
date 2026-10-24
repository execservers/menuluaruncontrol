// Windows Garry's Mod MENU-state interception of CLIENT RunString / RunStringEx.
// Injected with LoadLibraryW into the running process (no Lua require needed).
// MSVC x86 and x64; headers: danielga/garrysmod_common.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <GarrysMod/Lua/LuaInterface.h>
#include <GarrysMod/Lua/LuaShared.h>

#include <atomic>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace {
using GarrysMod::Lua::ILuaBase;
using GarrysMod::Lua::ILuaInterface;
using GarrysMod::Lua::ILuaShared;
namespace LuaType = GarrysMod::Lua::Type;

using Factory = void* (__cdecl*)(const char*, int*);
using RunStringFn = bool (__thiscall*)(ILuaInterface*, const char*, const char*, const char*, bool, bool);
using RunStringExFn = bool (__thiscall*)(ILuaInterface*, const char*, const char*, const char*, bool, bool, bool, bool);

// For the garrysmod_common LuaBase/LuaInterface Windows/MSVC ABI.
// LuaBase: 55 virtual functions; then LuaInterface::RunString is #37,
// LuaInterface::RunStringEx is #56 (indices from zero in that class).
// These slots MUST be reviewed if the upstream interface changes.
constexpr size_t kRunStringSlot = 92;
constexpr size_t kRunStringExSlot = 111;

std::atomic<ILuaShared*> g_shared{nullptr};
std::atomic<DWORD> g_luaThread{0};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_failed{false};
std::atomic<HHOOK> g_msgHook{nullptr};
RunStringFn g_originalRunString = nullptr;
RunStringExFn g_originalRunStringEx = nullptr;

// Lua is not thread safe. Guards apply only on the main Lua/UI thread.
thread_local bool g_inMenuCallback = false;
thread_local bool g_inRunString = false;
thread_local std::string g_outerFile;
thread_local std::string g_outerCode;

struct ScopedFlag {
    explicit ScopedFlag(bool& flag) : flag_(flag) { flag_ = true; }
    ~ScopedFlag() { flag_ = false; }
    ScopedFlag(const ScopedFlag&) = delete;
    ScopedFlag& operator=(const ScopedFlag&) = delete;
private:
    bool& flag_;
};

std::string NormalizeFile(const char* filename) {
    std::string file = filename ? filename : "";
    for (char& c : file) if (c == '\\') c = '/';
    while (file.compare(0, 2, "./") == 0) file.erase(0, 2);
    // GMod often passes the path inside the Lua search root.
    if (file.compare(0, 8, "autorun/") == 0) file.insert(0, "lua/");
    return file;
}

// Both names are supported. "bitch" is the exact event from the supplied
// example, while "luaruncontrol" preserves compatibility with the API title.
// Check "bitch" first. If it does not produce a replacement, check the second
// name; returning nil from both means run the original client code.
constexpr const char* kEvents[] = {"bitch", "luaruncontrol"};

// Runs exactly one hook.Run(event, file, code). Leave the stack in its original
// state on every path, including Lua errors.
std::optional<std::string> RunMenuHook(ILuaBase* lua, const char* event,
                                        const std::string& filename,
                                        const char* code, unsigned int codeLen) {
    const int top = lua->Top();
    lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua->GetField(-1, "hook");
    if (lua->GetType(-1) != LuaType::Table) {
        lua->Pop(lua->Top() - top);
        return std::nullopt;
    }
    lua->GetField(-1, "Run");
    if (lua->GetType(-1) != LuaType::Function) {
        lua->Pop(lua->Top() - top);
        return std::nullopt;
    }
    lua->PushString(event);
    lua->PushString(filename.c_str(), static_cast<unsigned int>(filename.size()));
    lua->PushString(code, codeLen);

    std::optional<std::string> replacement;
    if (lua->PCall(3, 1, 0) == 0 && lua->GetType(-1) == LuaType::String) {
        unsigned int len = 0;
        const char* value = lua->GetString(-1, &len);
        // The native RunString API requires a NUL-terminated string.
        if (value && !std::memchr(value, '\0', len)) {
            replacement.emplace(value, len);
        }
    }
    lua->Pop(lua->Top() - top);
    return replacement;
}

std::optional<std::string> MenuOverride(ILuaBase* lua,
                                         const std::string& filename,
                                         const char* code) {
    if (!lua || !code || g_inMenuCallback) return std::nullopt;
    const size_t codeLen = std::strlen(code);
    if (codeLen > std::numeric_limits<unsigned int>::max() ||
        filename.size() > std::numeric_limits<unsigned int>::max()) return std::nullopt;

    ScopedFlag guard(g_inMenuCallback);
    for (const char* event : kEvents) {
        auto replacement = RunMenuHook(lua, event, filename, code,
                                        static_cast<unsigned int>(codeLen));
        if (replacement) return replacement;
    }
    return std::nullopt;
}

std::optional<std::string> Intercept(ILuaInterface* self,
                                     const char* filename,
                                     const char* code) {
    if (!g_ready.load(std::memory_order_acquire) || !self || !code ||
        g_inMenuCallback || GetCurrentThreadId() != g_luaThread.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    try {
        ILuaShared* shared = g_shared.load(std::memory_order_acquire);
        if (!shared || shared->GetLuaInterface(GarrysMod::Lua::State::CLIENT) != self)
            return std::nullopt; // Never intercept SERVER or MENU.
        ILuaInterface* menu = shared->GetLuaInterface(GarrysMod::Lua::State::MENU);
        if (!menu) return std::nullopt;
        return MenuOverride(static_cast<ILuaBase*>(menu), NormalizeFile(filename), code);
    } catch (...) {
        return std::nullopt; // Never let C++ exceptions escape into GMod.
    }
}

// MSVC __thiscall on x86 has 'this' in ECX; a fastcall stub consumes EDX.
// On x64 there is only one Windows calling convention (no extra EDX arg).
#if defined(_M_IX86)
#define DETOUR_CALL __fastcall
#define DETOUR_THIS ILuaInterface* self, void*,
#else
#define DETOUR_CALL
#define DETOUR_THIS ILuaInterface* self,
#endif

bool DETOUR_CALL OnRunString(DETOUR_THIS const char* filename, const char* path,
                             const char* code, bool run, bool showErrors) {
    if (!g_originalRunString) return false;
    std::optional<std::string> replacement = Intercept(self, filename, code);
    const char* effective = replacement ? replacement->c_str() : code;

    // Most builds implement RunString via RunStringEx. Suppress only the
    // forwarded duplicate; separately executed nested scripts still dispatch.
    const bool previous = g_inRunString;
    std::string previousFile = std::move(g_outerFile);
    std::string previousCode = std::move(g_outerCode);
    try {
        g_outerFile = filename ? filename : "";
        g_outerCode = effective ? effective : "";
    } catch (...) {
        g_outerFile.clear();
        g_outerCode.clear();
    }
    g_inRunString = true;
    const bool result = g_originalRunString(self, filename, path, effective, run, showErrors);
    g_inRunString = previous;
    g_outerFile = std::move(previousFile);
    g_outerCode = std::move(previousCode);
    return result;
}

bool DETOUR_CALL OnRunStringEx(DETOUR_THIS const char* filename, const char* path,
                               const char* code, bool run, bool printErrors,
                               bool dontPushErrors, bool noReturns) {
    if (!g_originalRunStringEx) return false;
    bool fromRunString = false;
    if (g_inRunString && filename && code) {
        // Compare the content because GMod may copy strings between the calls.
        fromRunString = g_outerFile == filename && g_outerCode == code;
    }
    std::optional<std::string> replacement;
    if (!fromRunString) replacement = Intercept(self, filename, code);
    return g_originalRunStringEx(self, filename, path,
        replacement ? replacement->c_str() : code,
        run, printErrors, dontPushErrors, noReturns);
}

// Patches the vtable of the concrete CLuaInterface implementation. It is
// shared by MENU and CLIENT, but Intercept only acts on the CLIENT instance.
// We PIN the DLL and do not attempt hot-unload or detour removal.
bool PatchSlot(void** slot, void* replacement) {
    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection)) return false;
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), replacement);
    DWORD unused = 0;
    VirtualProtect(slot, sizeof(void*), oldProtection, &unused);
    return true;
}

bool Install() {
    HMODULE module = GetModuleHandleW(L"lua_shared.dll");
    if (!module) return false;
    auto* factory = reinterpret_cast<Factory>(GetProcAddress(module, "CreateInterface"));
    if (!factory) return false;
    auto* shared = static_cast<ILuaShared*>(factory(GMOD_LUASHARED_INTERFACE, nullptr));
    if (!shared) return false;
    ILuaInterface* menu = shared->GetLuaInterface(GarrysMod::Lua::State::MENU);
    if (!menu) return false;

    // Delay installation until MENU Lua and the standard hook library exist.
    auto* lua = static_cast<ILuaBase*>(menu);
    lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua->GetField(-1, "MENU_DLL");
    const bool menuReady = lua->GetType(-1) == LuaType::Bool && lua->GetBool(-1);
    lua->Pop();
    lua->GetField(-1, "hook");
    bool hookReady = false;
    if (lua->GetType(-1) == LuaType::Table) {
        lua->GetField(-1, "Run");
        hookReady = lua->GetType(-1) == LuaType::Function;
        lua->Pop();
    }
    lua->Pop(2); // hook + _G
    if (!menuReady || !hookReady) return false;

    auto** vtable = *reinterpret_cast<void***>(menu);
    if (!vtable || !vtable[kRunStringSlot] || !vtable[kRunStringExSlot]) return false;
    g_originalRunString = reinterpret_cast<RunStringFn>(vtable[kRunStringSlot]);
    g_originalRunStringEx = reinterpret_cast<RunStringExFn>(vtable[kRunStringExSlot]);
    g_shared.store(shared, std::memory_order_release);
    g_luaThread.store(GetCurrentThreadId(), std::memory_order_release);
    if (!PatchSlot(&vtable[kRunStringSlot], reinterpret_cast<void*>(&OnRunString))) {
        g_failed.store(true, std::memory_order_release);
        return false;
    }
    if (!PatchSlot(&vtable[kRunStringExSlot], reinterpret_cast<void*>(&OnRunStringEx))) {
        // Restore the first slot if the second patch failed.
        PatchSlot(&vtable[kRunStringSlot], reinterpret_cast<void*>(g_originalRunString));
        g_failed.store(true, std::memory_order_release);
        return false;
    }
    g_ready.store(true, std::memory_order_release);
    return true;
}

// Protect against access violations while probing startup-only interface APIs.
int WINAPI ProbeExceptionFilter(EXCEPTION_POINTERS*) {
    g_failed.store(true, std::memory_order_release);
    return EXCEPTION_EXECUTE_HANDLER;
}
bool GuardedInstall() {
    __try {
        return Install();
    } __except (ProbeExceptionFilter(GetExceptionInformation())) {
        return false;
    }
}

LRESULT CALLBACK OnWindowMessage(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !g_ready.load(std::memory_order_acquire) &&
        !g_failed.load(std::memory_order_acquire)) {
        static thread_local bool busy = false;
        static thread_local ULONGLONG nextTry = 0;
        const ULONGLONG now = GetTickCount64();
        if (!busy && now >= nextTry) {
            busy = true;
            nextTry = now + 750;
            GuardedInstall();
            busy = false;
        }
    }
    return CallNextHookEx(g_msgHook.load(std::memory_order_acquire), code, wp, lp);
}

struct WindowThread { DWORD id = 0; HWND hwnd = nullptr; };
BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM payload) {
    auto* out = reinterpret_cast<WindowThread*>(payload);
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !tid || !IsWindowVisible(hwnd) ||
        GetWindow(hwnd, GW_OWNER)) return TRUE;
    wchar_t klass[128] = {};
    GetClassNameW(hwnd, klass, 128);
    if (!out->hwnd || lstrcmpiW(klass, L"Valve001") == 0) {
        out->hwnd = hwnd;
        out->id = tid;
    }
    return lstrcmpiW(klass, L"Valve001") != 0;
}

DWORD WINAPI Bootstrap(void*) {
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&Bootstrap), &pinned)) {
        g_failed.store(true, std::memory_order_release);
        return 1;
    }
    while (!g_ready.load(std::memory_order_acquire) &&
           !g_failed.load(std::memory_order_acquire)) {
        WindowThread window{};
        EnumWindows(&FindGameWindow, reinterpret_cast<LPARAM>(&window));
        if (window.hwnd && window.id && !g_msgHook.load(std::memory_order_acquire)) {
            HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, &OnWindowMessage, nullptr, window.id);
            if (hook) g_msgHook.store(hook, std::memory_order_release);
        }
        if (window.hwnd && g_msgHook.load(std::memory_order_acquire))
            PostMessageW(window.hwnd, WM_NULL, 0, 0);
        Sleep(500);
    }
    HHOOK hook = g_msgHook.exchange(nullptr, std::memory_order_acq_rel);
    if (hook) UnhookWindowsHookEx(hook);
    return g_ready.load(std::memory_order_acquire) ? 0 : 1;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, &Bootstrap, nullptr, 0, nullptr);
        if (!thread) return FALSE;
        CloseHandle(thread);
    }
    return TRUE;
}

// Status: 0 waiting for MENU, 1 active, -1 installation failed.
extern "C" __declspec(dllexport) int __cdecl LuaRunControl_Status() {
    if (g_failed.load(std::memory_order_acquire)) return -1;
    return g_ready.load(std::memory_order_acquire) ? 1 : 0;
}
