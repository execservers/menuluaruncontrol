// Optional diagnostic console for injected builds (no file logging).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "stdout_console.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
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

std::atomic<bool> g_active{false};
std::atomic<uint64_t> g_dropped{0};
struct Queue {
    std::mutex mutex;
    std::condition_variable wakeup;
    std::deque<Entry> entries;
    size_t bytes = 0;
};

Queue& SharedQueue() {
    // The DLL is pinned and the detached writer has process lifetime. Avoid
    // destroying a condition variable while that writer may still wait on it.
    static Queue* const queue = new Queue();
    return *queue;
}

void WriteBytes(HANDLE output, const char* bytes, size_t size) noexcept {
    while (size) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min(size, static_cast<size_t>(kWriteChunk)));
        if (!WriteFile(output, bytes, chunk, &written, nullptr) || written == 0) return;
        bytes += written;
        size -= written;
    }
}

void WriteText(HANDLE output, const char* text) noexcept {
    WriteBytes(output, text, std::char_traits<char>::length(text));
}

void PrintEntry(HANDLE output, const Entry& entry) noexcept {
    WriteText(output, "\r\n========== luaruncontrol ==========" "\r\nFILE:\r\n");
    WriteBytes(output, entry.file.data(), entry.file.size());
    WriteText(output, "\r\nCODE:\r\n");
    WriteBytes(output, entry.code.data(), entry.code.size());
    WriteText(output, "\r\n");
    if (entry.replacement) {
        WriteText(output, "REPLACEMENT:\r\n");
        WriteBytes(output, entry.replacement->data(), entry.replacement->size());
        WriteText(output, "\r\n");
    }
    WriteText(output, "===================================\r\n");
}

void Worker(HANDLE output) {
    for (;;) {
        Entry entry;
        {
            Queue& queue = SharedQueue();
            std::unique_lock<std::mutex> lock(queue.mutex);
            queue.wakeup.wait(lock, [&queue] { return !queue.entries.empty(); });
            entry = std::move(queue.entries.front());
            queue.bytes -= entry.Size();
            queue.entries.pop_front();
        }
        const uint64_t dropped = g_dropped.exchange(0, std::memory_order_relaxed);
        if (dropped) {
            WriteText(output, "\r\n[stdout] Dropped entries due to a full/busy queue: ");
            const std::string count = std::to_string(dropped);
            WriteBytes(output, count.data(), count.size());
            WriteText(output, "\r\n");
        }
        PrintEntry(output, entry);
    }
}
}  // namespace

void StdoutConsole::Start() noexcept {
    // AllocConsole creates a separate console window for normal GUI processes.
    // Do not detach/replace an existing console owned by the host process.
    if (!AllocConsole()) return;
    SetConsoleTitleW(L"menuluaruncontrol _stdout_");
    SetConsoleOutputCP(CP_UTF8);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) {
        FreeConsole();
        return;
    }
    try {
        SharedQueue(); // Allocate once, before enabling the publisher.
        std::thread(Worker, output).detach();
        g_active.store(true, std::memory_order_release);
        WriteText(output, "menuluaruncontrol _stdout_ active (MENU -> CLIENT)\r\n");
    } catch (...) {
        FreeConsole();
    }
}

void StdoutConsole::Publish(const std::string& file, const char* code,
                            const std::optional<std::string>& replacement) noexcept {
    if (!g_active.load(std::memory_order_acquire) || !code) return;
    try {
        Entry entry{file, code, replacement};
        const size_t size = entry.Size();
        if (size > kMaxQueuedBytes) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Never wait for the writer on the game's Lua thread.
        Queue& queue = SharedQueue();
        std::unique_lock<std::mutex> lock(queue.mutex, std::try_to_lock);
        if (!lock.owns_lock() || queue.entries.size() >= kMaxQueuedEntries ||
            size > kMaxQueuedBytes - queue.bytes) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue.entries.push_back(std::move(entry));
        queue.bytes += size;
        lock.unlock();
        queue.wakeup.notify_one();
    } catch (...) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}
