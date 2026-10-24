// Async IPC writer for the _stdout_ build. The helper, not the game, owns the
// console: CTRL_CLOSE_EVENT then terminates only the helper process.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "stdout_console.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {
struct Entry {
    std::string file;
    std::string code;
    std::optional<std::string> replacement;
    size_t Size() const noexcept {
        return file.size() + code.size() + (replacement ? replacement->size() : 0);
    }
};

constexpr size_t kMaxQueuedBytes = 16 * 1024 * 1024;
constexpr size_t kMaxQueuedEntries = 256;
constexpr DWORD kWriteChunk = 16 * 1024;

struct State {
    std::atomic<bool> active{false};
    std::atomic<uint64_t> dropped{0};
    std::mutex mutex;
    std::deque<Entry> entries;
    size_t queuedBytes = 0;
    HANDLE wakeup = nullptr;
    HANDLE pipe = nullptr;
    HANDLE viewerProcess = nullptr;
    StdoutConsole::ClosedCallback onClosed = nullptr;
};

// Pinning the DLL ensures these resources cannot be destroyed while the worker
// executes. In particular, the mutex is intentionally process-lifetime.
State& Shared() {
    static State* const state = new State();
    return *state;
}

bool WriteBytes(HANDLE handle, const char* bytes, size_t size) noexcept {
    while (size) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min(size, static_cast<size_t>(kWriteChunk)));
        if (!WriteFile(handle, bytes, chunk, &written, nullptr) || written == 0)
            return false;
        bytes += written;
        size -= written;
    }
    return true;
}

bool WriteText(HANDLE handle, const char* text) noexcept {
    return WriteBytes(handle, text, std::char_traits<char>::length(text));
}

bool PrintEntry(HANDLE output, const Entry& entry) noexcept {
    if (!WriteText(output, "\r\n========== luaruncontrol ==========\r\nFILE:\r\n")) return false;
    if (!WriteBytes(output, entry.file.data(), entry.file.size())) return false;
    if (!WriteText(output, "\r\nCODE:\r\n")) return false;
    if (!WriteBytes(output, entry.code.data(), entry.code.size())) return false;
    if (!WriteText(output, "\r\n")) return false;
    if (entry.replacement) {
        if (!WriteText(output, "REPLACEMENT:\r\n")) return false;
        if (!WriteBytes(output, entry.replacement->data(), entry.replacement->size())) return false;
        if (!WriteText(output, "\r\n")) return false;
    }
    return WriteText(output, "===================================\r\n");
}

// Called off the game thread. Callback only sets an atomic flag; restoration
// of the game's vtable is performed later on its own Lua/message thread.
void Worker() noexcept {
    State& state = Shared();
    const HANDLE waits[] = {state.viewerProcess, state.wakeup};
    bool running = WriteText(state.pipe,
        "menuluaruncontrol _stdout_ active (MENU -> CLIENT)\r\n"
        "Closing this window disables interception; the game stays open.\r\n");

    while (running) {
        const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) break; // console child exited
        if (result != WAIT_OBJECT_0 + 1) break;

        for (;;) {
            // Closing can happen while a large batch is being drained.
            if (WaitForSingleObject(state.viewerProcess, 0) == WAIT_OBJECT_0) {
                running = false;
                break;
            }
            Entry entry;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (state.entries.empty()) break;
                entry = std::move(state.entries.front());
                state.queuedBytes -= entry.Size();
                state.entries.pop_front();
            }
            const uint64_t dropped = state.dropped.exchange(0, std::memory_order_relaxed);
            if (dropped) {
                const std::string notice = "\r\n[stdout] Dropped entries: " +
                    std::to_string(dropped) + "\r\n";
                if (!WriteBytes(state.pipe, notice.data(), notice.size())) {
                    running = false;
                    break;
                }
            }
            if (!PrintEntry(state.pipe, entry)) {
                running = false; // broken IPC pipe
                break;
            }
        }
    }

    state.active.store(false, std::memory_order_release);
    // Close the write end so the reader can exit if it is still present.
    CloseHandle(state.pipe);
    if (state.onClosed) state.onClosed();
    CloseHandle(state.viewerProcess);
    // Keep wakeup alive to avoid SetEvent racing handle destruction with an
    // in-flight Publish on the game thread. DLL resources are process-lifetime.
}

// The handle is passed to the console child, and *only* that handle is
// inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST). Avoid leaking game handles.
bool LaunchViewer(HMODULE dll, HANDLE& pipe, HANDLE& process) {
    wchar_t dllPath[32768] = {};
    const DWORD count = GetModuleFileNameW(dll, dllPath, 32768);
    if (!count || count >= 32768) return false;
    std::wstring exePath(dllPath, count);
    const size_t slash = exePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    exePath.resize(slash + 1);
#if defined(_M_IX86)
    exePath += L"menuluaruncontrol_console_win32.exe";
#else
    exePath += L"menuluaruncontrol_console_win64.exe";
#endif

    SECURITY_ATTRIBUTES attr{};
    attr.nLength = sizeof(attr);
    attr.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &attr, 0)) return false;
    // The game only retains the non-inheritable write end.
    if (!SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return false;
    }

    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    if (!attributeSize) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return false;
    }
    auto attributes = std::make_unique<unsigned char[]>(attributeSize);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.get());
    if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeSize)) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return false;
    }
    const bool configured = UpdateProcThreadAttribute(attributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &readEnd, sizeof(readEnd), nullptr, nullptr) != 0;
    bool launched = false;
    if (configured) {
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributeList;
        PROCESS_INFORMATION info{};
        std::wstring command = L"\"" + exePath + L"\" " +
            std::to_wstring(reinterpret_cast<uintptr_t>(readEnd));
        if (CreateProcessW(exePath.c_str(), command.data(), nullptr, nullptr,
            TRUE, CREATE_NEW_CONSOLE | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, nullptr, &startup.StartupInfo, &info)) {
            CloseHandle(info.hThread);
            process = info.hProcess;
            pipe = writeEnd;
            launched = true;
        }
    }
    DeleteProcThreadAttributeList(attributeList);
    CloseHandle(readEnd);
    if (!launched) CloseHandle(writeEnd);
    return launched;
}
} // namespace

bool StdoutConsole::Start(HMODULE dll, ClosedCallback onClosed) noexcept {
    try {
        State& state = Shared();
        if (!onClosed) return false;
        state.wakeup = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!state.wakeup) return false;
        if (!LaunchViewer(dll, state.pipe, state.viewerProcess)) {
            CloseHandle(state.wakeup);
            state.wakeup = nullptr;
            return false;
        }
        state.onClosed = onClosed;
        state.active.store(true, std::memory_order_release);
        try {
            std::thread(Worker).detach();
        } catch (...) {
            state.active.store(false, std::memory_order_release);
            // Only our own helper is terminated. The host game stays running.
            TerminateProcess(state.viewerProcess, 1);
            CloseHandle(state.viewerProcess);
            CloseHandle(state.pipe);
            CloseHandle(state.wakeup);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void StdoutConsole::Publish(const std::string& file, const char* code,
                            const std::optional<std::string>& replacement) noexcept {
    State& state = Shared();
    if (!state.active.load(std::memory_order_acquire) || !code) return;
    try {
        Entry entry{file, code, replacement};
        const size_t size = entry.Size();
        if (size > kMaxQueuedBytes) {
            state.dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // The game thread never waits for the queue lock or console I/O.
        std::unique_lock<std::mutex> lock(state.mutex, std::try_to_lock);
        if (!lock.owns_lock() || !state.active.load(std::memory_order_relaxed) ||
            state.entries.size() >= kMaxQueuedEntries ||
            size > kMaxQueuedBytes - state.queuedBytes) {
            state.dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        state.entries.push_back(std::move(entry));
        state.queuedBytes += size;
        lock.unlock();
        SetEvent(state.wakeup);
    } catch (...) {
        state.dropped.fetch_add(1, std::memory_order_relaxed);
    }
}
