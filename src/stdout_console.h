#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <optional>
#include <string>

// Available only in the _stdout_ build. No console is attached to the game.
// The companion console EXE owns its window; closing it stops the interception.
namespace StdoutConsole {
using ClosedCallback = void (*)();

// The companion EXE must be next to the injected DLL.
// Calls onClosed from the background writer thread if the viewer disappears.
bool Start(HMODULE dll, ClosedCallback onClosed) noexcept;
void Publish(const std::string& file, const char* code,
             const std::optional<std::string>& replacement) noexcept;
}
