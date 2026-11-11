// SPDX-License-Identifier: GPL-3.0-or-later

// NUP ("NX Package", .nup): one file holding the unencrypted ExeFS and RomFS of a game's base,
// its updates and its DLC, each file stored raw or as one zstd frame. Format: SPEC.md of
// https://github.com/a55uka/nup (v1).
//
// The archive is shown as the layout of an extracted dump (see deconstructed_rom_directory.cpp):
//   <name>/base/exefs, <name>/base/romfs
//   <name>/updates/<decimal version>/exefs, .../romfs
//   <name>/dlc/<16-hex title ID>/<index>/romfs
// so the extracted-dump loader runs a NUP game as it runs a folder: updates shadow the base.

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys::Nup {

constexpr u32 NoDirectory = 0xFFFFFFFF;

enum class PackageKind : u8 { Base = 0, Update = 1, AddOn = 2, AddOnUpdate = 3, System = 4 };

enum class MetaType : u16 {
    Npdm = 0x0001,
    Nacp = 0x0002,
    Icon = 0x0003,
    DisplayVersion = 0x0004,
    TitleName = 0x0005,
    Publisher = 0x0006,
    BuildId = 0x0007,
    SourceDumpName = 0x0008,
    LocalizedTitles = 0x0009,
    LocalizedIcons = 0x000A,
};

struct Package {
    u64 title_id;
    u32 root_dir;
    u32 exefs_dir;
    u32 romfs_dir;
    u32 version;
    PackageKind kind;
    u8 flags;
    u64 meta_offset;
    u32 meta_size;
};

struct Directory {
    u32 name_off;
    u32 name_len;
    u32 first_child;
    u32 child_count;
};

struct File {
    u32 name_off;
    u32 name_len;
    u64 size;
    u64 extent_offset;
    u64 extent_size;
    u8 codec; // 0 raw, 1 zstd
};

class Archive : public std::enable_shared_from_this<Archive> {
public:
    // Whether the file starts with the NUP magic (cheap; nothing else is checked).
    static bool HasMagic(const VirtualFile& file);

    // Reads and checks the superblock and the TOC (SPEC §6.1). nullptr, with the reason in
    // *error, when the file is not a valid v1 NUP.
    static std::shared_ptr<Archive> Open(VirtualFile file, std::string* error = nullptr);
    // Only the superblock, the package records and their meta blocks, for a game list: a large
    // game's TOC is tens of MiB (372k files: 37 MiB), read and hashed for nothing there. Such an
    // archive has no tree (Tree() is nullptr).
    static std::shared_ptr<Archive> OpenMetadata(VirtualFile file, std::string* error = nullptr);
    ~Archive();

    const std::vector<Package>& Packages() const { return packages; }
    // The base package, else nullptr (an update-only container).
    const Package* Base() const;
    // The package that holds the application's metadata: the base, else the first one.
    const Package* MetadataPackage() const;
    // The program title ID: the base's, else the update's with its 0x800 cleared.
    u64 ProgramTitleID() const;

    // The meta TLVs of a package (type -> payload, SPEC numbering). The first record of each type
    // wins.
    std::map<u16, std::vector<u8>> Meta(const Package& package) const;
    // Whether the container numbers its meta types from 0 (NPDM 0, NACP 1, ...): the reference
    // packer does since its commit 1d904e1 (TLV types became an IntEnum starting at 0), while
    // SPEC.md numbers them from 1. Type 0 is not a SPEC type, so a block with one is such a file.
    bool ZeroBasedMeta() const { return zero_based_meta; }
    std::optional<std::vector<u8>> MetaRecord(const Package& package, MetaType type) const;
    std::string MetaString(const Package& package, MetaType type) const;

    // The archive as an extracted dump (see the header comment), named `name`.
    VirtualDir Tree(const std::string& name) const;
    // The "main" an extracted-dump loader starts from: base/exefs/main, else the newest
    // update's exefs/main. nullptr when there is none.
    VirtualFile MainFile(const VirtualDir& tree) const;

    // Internal: used by the directory and file views.
    std::string Name(u32 offset, u32 length) const;
    u32 DirCount() const { return static_cast<u32>(dirs.size()); }
    const Directory& Dir(u32 index) const { return dirs[index]; }
    const File& FileRecord(u32 index) const { return files[index]; }
    u32 Child(u32 slot) const { return children[slot]; }
    // The node named `name` among a directory's children, or NoDirectory.
    u32 FindChild(u32 dir, std::string_view name) const;
    std::size_t ReadFile(u32 index, u8* data, std::size_t length, std::size_t offset) const;

private:
    Archive() = default;

    struct Decompressed {
        std::shared_ptr<const std::vector<u8>> data;
        u64 last_use;
    };
    std::shared_ptr<const std::vector<u8>> MemoryCopy(u32 index) const;
    // Large zstd files are decompressed once into a cache file named after their SHA-256.
    int CacheFile(u32 index) const;

    std::map<u16, std::vector<u8>> RawMeta(const Package& package) const;
    static std::shared_ptr<Archive> Read(VirtualFile file, std::string* error, bool metadata_only);
    std::string_view NameView(u32 offset, u32 length) const;

    VirtualFile backing;
    u64 toc_offset{};
    u64 files_at{}; // TOC offset of files[]: each record's SHA-256 is read from the file when needed
    bool metadata_only{};
    u64 blob_offset{};
    bool zero_based_meta{};
    std::vector<Package> packages;
    std::vector<Directory> dirs;
    std::vector<File> files;
    std::vector<u32> children;
    std::vector<u8> string_pool;

    mutable std::mutex cache_mutex;
    mutable std::map<u32, Decompressed> memory_cache;
    mutable std::size_t memory_cache_bytes{};
    mutable u64 use_counter{};
    mutable std::map<u32, int> cache_files;
};

} // namespace FileSys::Nup
