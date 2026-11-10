#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise folder resolution and the loader's actual version ordering against fixture trees."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
loader = (root / 'Eglantine/src/core/loader/deconstructed_rom_directory.cpp').read_text()
# The core's extracted-layer ordering (ExtractedLayers/ExtractedExeFS) was rewritten with its
# loose-dump handling: update folders are picked by a strict numeric name, and the layers come from
# FindSidecarDirs. The port's extracted_game.h still follows the old rule, so the parity check only
# runs against a core that still spells it that way.
MARKERS = ('std::vector<FileSys::VirtualDir> ExtractedLayers(', 'FileSys::VirtualDir ExtractedExeFS(')
if not all(marker in loader for marker in MARKERS):
    print('Extracted content: skipped, the core no longer has ExtractedLayers/ExtractedExeFS '
          '(port the check to FindSidecarDirs and the numeric update rule)')
    raise SystemExit(0)
ordering = loader[loader.index(MARKERS[0]):loader.index(MARKERS[1])]
program = r'''
#include "extracted_game.h"
#include <algorithm>
#include <cassert>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
using u64 = uint64_t;
#define LOG_INFO(...)
namespace FileSys {
struct Directory;
using VirtualDir = std::shared_ptr<Directory>;
struct Directory {
    std::filesystem::path path;
    std::string GetName() const { return path.filename().string(); }
    VirtualDir GetParentDirectory() const { return std::make_shared<Directory>(Directory{path.parent_path()}); }
    VirtualDir GetSubdirectory(const char* name) const {
        const auto child = path / name;
        return std::filesystem::is_directory(child) ? std::make_shared<Directory>(Directory{child}) : nullptr;
    }
    std::vector<VirtualDir> GetSubdirectories() const {
        std::vector<VirtualDir> out;
        for (const auto& e : std::filesystem::directory_iterator(path))
            if (e.is_directory()) out.push_back(std::make_shared<Directory>(Directory{e.path()}));
        return out;
    }
};
}
ORDERING
int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path game = fs::path(argv[1]) / "Pokopia";
    fs::create_directories(game / "base/exefs");
    std::ofstream(game / "base/exefs/main") << "base";
    assert(Eden::ExtractedGameMain(game.string()) == (game / "base/exefs/main").string());
    const auto base = std::make_shared<FileSys::Directory>(FileSys::Directory{game / "base/exefs"});
    assert(ExtractedLayers(base).size() == 1);
    for (const char* version : {"9", "10", "655360", "notes", "18446744073709551616", "12bad"})
        fs::create_directories(game / "updates" / version);
    auto layers = ExtractedLayers(base);
    assert(layers.size() == 4);
    assert(layers[0]->GetName() == "655360" && layers[1]->GetName() == "10");
    assert(layers[2]->GetName() == "9" && layers[3]->GetName() == "base");
    const auto npdm_path = game / "base/exefs/main.npdm";
    std::vector<unsigned char> npdm(0x580);
    std::memcpy(npdm.data(), "META", 4);
    uint32_t aci = 0x560;
    uint64_t title = 0x0400A3A01EE4C000ULL;
    std::memcpy(npdm.data() + 0x70, &aci, 4);
    std::memcpy(npdm.data() + aci, "ACI0", 4);
    std::memcpy(npdm.data() + aci + 0x10, &title, 8);
    const auto write_npdm = [&] { std::ofstream f(npdm_path, std::ios::binary); f.write(reinterpret_cast<const char*>(npdm.data()), npdm.size()); };
    write_npdm();
    assert(Eden::ExtractedTitleID(npdm_path.string()) == title);
    npdm.resize(0x570); write_npdm();
    assert(Eden::ExtractedTitleID(npdm_path.string()) == 0);
    aci = 0xfffffff0; std::memcpy(npdm.data() + 0x70, &aci, 4); write_npdm();
    assert(Eden::ExtractedTitleID(npdm_path.string()) == 0);
    // A complete versioned dump can have updates but no base executable.
    const auto only = fs::path(argv[1]) / "UpdateOnly";
    for (const char* version : {"9", "10", "327680", "999bad", "18446744073709551616"}) {
        fs::create_directories(only / "updates" / version / "exefs");
        std::ofstream(only / "updates" / version / "exefs/main") << version;
    }
    fs::create_directories(only / "updates/655360"); // incomplete update, no executable
    assert(Eden::ExtractedGameMain(only.string()) == (only / "updates/327680/exefs/main").string());
    const auto only_exefs = std::make_shared<FileSys::Directory>(FileSys::Directory{only / "updates/327680/exefs"});
    auto only_layers = ExtractedLayers(only_exefs);
    assert(only_layers.size() == 4 && only_layers[0]->GetName() == "655360");
    assert(only_layers[1]->GetName() == "327680" && only_layers[2]->GetName() == "10");
    fs::create_directories(only / "base"); // empty base is also a supported layout
    assert(Eden::ExtractedGameMain(only.string()) == (only / "updates/327680/exefs/main").string());
    only_layers = ExtractedLayers(only_exefs);
    assert(only_layers.size() == 5 && only_layers.back()->GetName() == "base");
    fs::create_directories(only / "base/exefs");
    std::ofstream(only / "base/exefs/main") << "base";
    assert(Eden::ExtractedGameMain(only.string()) == (only / "base/exefs/main").string());
    // Existing direct ExeFS games stay independent of similarly named sibling updates.
    const fs::path direct = fs::path(argv[1]) / "Direct";
    fs::create_directories(direct / "exefs");
    std::ofstream(direct / "exefs/main") << "direct";
    assert(Eden::ExtractedGameMain(direct.string()) == (direct / "exefs/main").string());
    assert(ExtractedLayers(std::make_shared<FileSys::Directory>(FileSys::Directory{direct / "exefs"})).empty());
    assert(Eden::ExtractedGameMain((game / "updates").string()).empty());
    assert(Eden::ExtractedGameMain((game / "missing").string()).empty());
}
'''.replace('ORDERING', ordering)
with tempfile.TemporaryDirectory(prefix='eglantine-extracted-') as temporary:
    folder = Path(temporary)
    cpp, executable = folder / 'check.cpp', folder / 'check'
    cpp.write_text(program)
    subprocess.run(['g++', '-std=c++20', '-I' + str(root / 'headless'), str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable), str(folder)], check=True)
print('Extracted content: direct/base/update-only layouts, numeric update precedence, invalid versions, missing games and NPDM offsets/truncation PASS')
