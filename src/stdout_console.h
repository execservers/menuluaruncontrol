#pragma once

#include <optional>
#include <string>

// Compiled only in the _stdout_ DLL. The Lua thread never writes to the
// console; a dedicated worker prints the queued entries.
namespace StdoutConsole {
void Start() noexcept;
void Publish(const std::string& file, const char* code,
             const std::optional<std::string>& replacement) noexcept;
}
