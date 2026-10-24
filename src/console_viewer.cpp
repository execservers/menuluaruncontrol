// Companion console program for the _stdout_ injected DLL. Windows attaches
// only this process to the new console, so closing its window cannot kill GMod.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <cwchar>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || !argv[1] || !*argv[1]) return 2;
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(argv[1], &end, 10);
    if (errno || !end || *end != L'\0' || !value) return 2;
    const HANDLE input = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));

    SetConsoleTitleW(L"menuluaruncontrol _stdout_");
    SetConsoleOutputCP(CP_UTF8);
    // Acquire our OWN console buffer; none is shared with the game process.
    const HANDLE output = CreateFileW(L"CONOUT$", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (output == INVALID_HANDLE_VALUE) return 3;

    char buffer[16 * 1024];
    DWORD length = 0;
    while (ReadFile(input, buffer, sizeof(buffer), &length, nullptr) && length) {
        DWORD offset = 0;
        while (offset < length) {
            DWORD written = 0;
            if (!WriteFile(output, buffer + offset, length - offset, &written, nullptr) ||
                !written) {
                CloseHandle(output);
                CloseHandle(input);
                return 4;
            }
            offset += written;
        }
    }
    CloseHandle(output);
    CloseHandle(input);
    return 0;
}
