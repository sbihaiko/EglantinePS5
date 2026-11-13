#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual development selection branch, including a missing title."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'headless/main.cpp').read_text()
start = source.index('#if defined(EDEN_DEV_PROFILE) || defined(EDEN_DEV_ROM_ID)\n'
                     '        if (std::exchange(autoboot_pending, false)) {')
end = source.index('        Eden::BootTrace::Line("launcher closed: %s"', start)
branch = source[start:end]
helper_start = source.index('// Unattended sessions on the console: EDEN_LAUNCHER_AUTOPICK names a game')
helper = source[helper_start:source.index('\n}\n', helper_start) + 3]
wait_start = source.index('#elif defined(EDEN_DEV_ROM_ID)\n                        // Library and forwarded launches')
wait_start = source.index('\n', wait_start) + 1
wait_end = source.index('#else\n                        completion->wake.wait(lock, completed);', wait_start)
observation = source[wait_start:wait_end]
wait_program = r'''
#include <cassert>
#include <chrono>
struct Lock { void lock() {} void unlock() {} };
struct Wake {
    unsigned waits{}, timed{};
    template <class L, class P> void wait(L&, P) { ++waits; }
    template <class L, class D, class P> bool wait_for(L&, D, P) { ++timed; return false; }
};
struct Completion { Wake wake; bool return_to_menu{}; };
struct Gpu { int Renderer() { return 0; } };
struct System { Gpu gpu; Gpu& GPU() { return gpu; } };
struct Window {
    unsigned captures{};
    template <class F> void CaptureNextFrame(int, F) { ++captures; }
};
struct Result { unsigned waits, timed, captures; bool returned; };
Result Observe(bool compatibility_probe) {
    Lock lock;
    Completion state;
    auto* completion = &state;
    auto completed = [] { return false; };
    System system;
    Window window;
OBSERVATION
    return {state.wake.waits, state.wake.timed, window.captures, state.return_to_menu};
}
int main() {
    const auto library = Observe(false);
    assert(library.waits == 1 && library.timed == 0 && library.captures == 0 && !library.returned);
    const auto automatic = Observe(true);
    assert(automatic.waits == 0 && automatic.timed == 120 && automatic.captures == 119 && automatic.returned);
}
'''.replace('OBSERVATION', observation)
program = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
struct Entry {
    std::filesystem::path name;
    const std::filesystem::path& path() const { return name; }
};
std::vector<Entry> entries;
std::uint64_t metadata;
int launcher_calls;
std::string launcher_choice;
namespace Eden {
std::string AssetsPath(const char*) { return "/roms"; }
std::vector<Entry> ReadNativeDirectory(const std::string& folder, std::error_code&) {
    return folder == "/roms" ? entries : std::vector<Entry>{};
}
std::string ResolveForwardedRom(std::string_view rom, std::string_view roms_dir) {
    if (!rom.empty() && rom.front() == '/') return std::string{rom};
    return std::string{roms_dir} + "/" + std::string{rom};
}
std::string ExtractedGameMain(const std::string&) { return {}; }
bool FileExists(const std::string& path) { return std::filesystem::exists(path); }
namespace BootTrace { template <typename... T> void Line(const char*, T...) {} }
}
std::uint64_t eden_game_title_id(const char*) { return metadata; }
std::string SelectEglantinePS5Game(int, const std::string&) {
    ++launcher_calls;
    return launcher_choice;
}
HELPER
struct Result { std::string selected, error; bool pending, probe; };
Result Select(bool autoboot_pending) {
    int rooms = 0;
    bool compatibility_probe = false;
    std::string development_id = "040074A01BF12000", selected_game, launch_error;
BRANCH
    return {selected_game, launch_error, autoboot_pending, compatibility_probe};
}
int main() {
    launcher_choice = "/roms/chosen.xci";
    metadata = 0;
    for (auto candidates : {std::vector<Entry>{}, std::vector<Entry>{{"LooseGame"}, {"other.xci"}}}) {
        entries = candidates;
        launcher_calls = 0;
        auto result = Select(true);
        assert(result.selected == launcher_choice && !result.error.empty());
        assert(launcher_calls == 1 && !result.pending && !result.probe);
    }
    entries = {{"040074A01BF12000.nsp"}};
    launcher_calls = 0;
    auto named = Select(true);
    assert(named.selected == "/roms/040074A01BF12000.nsp" && named.error.empty());
    assert(launcher_calls == 0 && !named.pending && named.probe);
    entries = {{"named.xci"}};
    metadata = 0x040074A01BF12000ULL;
    auto matched = Select(true);
    assert(matched.selected == "/roms/named.xci" && matched.error.empty());
    assert(launcher_calls == 0 && matched.probe);
    auto first = Select(false);
    assert(first.selected == launcher_choice && first.error.empty() && launcher_calls == 1 && !first.probe);
    // EDEN_LAUNCHER_AUTOPICK: a named game starts without the launcher; a game that is not there
    // says so, and the launcher still opens.
    {
        const auto picked = std::filesystem::temp_directory_path() / "eden-autopick.nsp";
        std::ofstream(picked).put('x');
        setenv("EDEN_LAUNCHER_AUTOPICK", picked.c_str(), 1);
        launcher_calls = 0;
        auto automatic = Select(false);
        assert(automatic.selected == picked.string() && automatic.error.empty() && launcher_calls == 0);
        setenv("EDEN_LAUNCHER_AUTOPICK", "/roms/absent.nup", 1);
        launcher_calls = 0;
        auto absent = Select(false);
        assert(absent.selected == launcher_choice && launcher_calls == 1);
        assert(absent.error.find("Autopicked game not found: /roms/absent.nup") != std::string::npos);
        unsetenv("EDEN_LAUNCHER_AUTOPICK");
    }
    launcher_choice.clear();
    auto quit = Select(false);
    assert(quit.selected.empty() && quit.error.empty() && launcher_calls == 2);
}
'''.replace('HELPER', helper).replace('BRANCH', branch)
with tempfile.TemporaryDirectory(prefix='eden-launcher-autoboot-') as temporary:
    folder = Path(temporary)
    cpp, executable = folder / 'test.cpp', folder / 'test'
    cpp.write_text(program)
    subprocess.run(['g++', '-std=c++20', '-DEDEN_DEV_ROM_ID=1', str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    wait_cpp, wait_binary = folder / 'wait.cpp', folder / 'wait'
    wait_cpp.write_text(wait_program)
    subprocess.run(['g++', '-std=c++20', str(wait_cpp), '-o', str(wait_binary)], check=True)
    subprocess.run([str(wait_binary)], check=True)
print('Launcher startup: missing title fallback, filename/metadata autoboot, launcher-first/manual selection stays outside timed probes, unattended autopick and quit PASS')

print('Actual observation branch: Library gameplay waits without a timer; automatic probe samples120x5seconds PASS')
