// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>

#include <fmt/format.h>
#include <openssl/evp.h>
#include <zstd.h>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/zstd_compression.h"
#include "core/file_sys/nup.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys::Nup {
namespace {

constexpr std::size_t SuperblockSize = 0x80;
constexpr std::size_t TocHeaderSize = 0x30;
constexpr std::size_t PackageSize = 0x40;
constexpr std::size_t DirSize = 0x18;
constexpr std::size_t FileSize = 0x50;
constexpr u32 TocMagic = 0x01434F54; // "TOC\1"
constexpr u64 MaxTocSize = 1ULL << 30;

// zstd files up to this size are decompressed into memory, kept while the cache stays under
// MemoryBudget; larger ones are decompressed once to a file in the cache folder.
constexpr std::size_t MemoryFileLimit = 8ULL << 20;
constexpr std::size_t MemoryBudget = 64ULL << 20;

template <typename T>
T Load(const u8* data) {
    T value;
    std::memcpy(&value, data, sizeof(T));
    return value;
}

bool Fail(std::string* error, std::string reason) {
    if (error) *error = std::move(reason);
    return false;
}

std::string Hex(const std::array<u8, 32>& bytes) {
    std::string text;
    text.reserve(64);
    for (const u8 byte : bytes) text += fmt::format("{:02x}", byte);
    return text;
}

// A synthetic level of the tree: base/, updates/, updates/<version>/, ...
struct Node {
    std::string name;
    u32 dir = NoDirectory;                 // a TOC directory, or synthetic when NoDirectory
    std::vector<std::shared_ptr<const Node>> children;
    std::vector<std::pair<std::string, std::vector<u8>>> files; // sidecars (control.nacp, icon)
};

class NupFile final : public VfsFile {
public:
    NupFile(std::shared_ptr<const Archive> archive_, u32 index_, VirtualDir parent_)
        : archive(std::move(archive_)), index(index_), parent(std::move(parent_)) {}

    std::string GetName() const override {
        const auto& record = archive->FileRecord(index);
        return archive->Name(record.name_off, record.name_len);
    }
    std::size_t GetSize() const override { return archive->FileRecord(index).size; }
    bool Resize(std::size_t) override { return false; }
    VirtualDir GetContainingDirectory() const override { return parent; }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        return archive->ReadFile(index, data, length, offset);
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override { return 0; }
    bool Rename(std::string_view) override { return false; }

private:
    std::shared_ptr<const Archive> archive;
    u32 index;
    VirtualDir parent;
};

class NupDirectory final : public VfsDirectory {
public:
    static VirtualDir Make(std::shared_ptr<const Archive> archive, std::shared_ptr<const Node> node,
                           u32 dir, std::string name, VirtualDir parent) {
        auto made = std::shared_ptr<NupDirectory>(new NupDirectory(
            std::move(archive), std::move(node), dir, std::move(name), std::move(parent)));
        made->self = made;
        return made;
    }

    std::vector<VirtualFile> GetFiles() const override {
        std::vector<VirtualFile> out;
        const VirtualDir me = self.lock();
        if (node) {
            for (const auto& [file_name, bytes] : node->files)
                out.push_back(std::make_shared<VectorVfsFile>(bytes, file_name, me));
            return out;
        }
        const auto& record = archive->Dir(dir);
        for (u32 slot = record.first_child; slot < record.first_child + record.child_count; ++slot) {
            const u32 id = archive->Child(slot);
            if (id >= archive->DirCount())
                out.push_back(std::make_shared<NupFile>(archive, id - archive->DirCount(), me));
        }
        return out;
    }

    std::vector<VirtualDir> GetSubdirectories() const override {
        std::vector<VirtualDir> out;
        const VirtualDir me = self.lock();
        if (node) {
            for (const auto& child : node->children) {
                if (child->dir != NoDirectory)
                    out.push_back(Make(archive, nullptr, child->dir, child->name, me));
                else
                    out.push_back(Make(archive, child, NoDirectory, child->name, me));
            }
            return out;
        }
        const auto& record = archive->Dir(dir);
        for (u32 slot = record.first_child; slot < record.first_child + record.child_count; ++slot) {
            const u32 id = archive->Child(slot);
            if (id < archive->DirCount()) {
                const auto& child = archive->Dir(id);
                out.push_back(Make(archive, nullptr, id, archive->Name(child.name_off, child.name_len), me));
            }
        }
        return out;
    }

    // A game's folders can hold thousands of files: look names up without making the others.
    VirtualFile GetFile(std::string_view file_name) const override {
        if (node) {
            for (auto& file : GetFiles())
                if (file->GetName() == file_name) return file;
            return nullptr;
        }
        const u32 id = archive->FindChild(dir, file_name);
        if (id == NoDirectory || id < archive->DirCount()) return nullptr;
        return std::make_shared<NupFile>(archive, id - archive->DirCount(), self.lock());
    }
    VirtualDir GetSubdirectory(std::string_view dir_name) const override {
        if (node) {
            for (auto& sub : GetSubdirectories())
                if (sub->GetName() == dir_name) return sub;
            return nullptr;
        }
        const u32 id = archive->FindChild(dir, dir_name);
        if (id >= archive->DirCount()) return nullptr;
        return Make(archive, nullptr, id, std::string(dir_name), self.lock());
    }

    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }
    std::string GetName() const override { return name; }
    VirtualDir GetParentDirectory() const override { return parent; }
    VirtualDir CreateSubdirectory(std::string_view) override { return nullptr; }
    VirtualFile CreateFile(std::string_view) override { return nullptr; }
    bool DeleteSubdirectory(std::string_view) override { return false; }
    bool DeleteFile(std::string_view) override { return false; }
    bool Rename(std::string_view) override { return false; }

private:
    NupDirectory(std::shared_ptr<const Archive> archive_, std::shared_ptr<const Node> node_, u32 dir_,
                 std::string name_, VirtualDir parent_)
        : archive(std::move(archive_)), node(std::move(node_)), dir(dir_), name(std::move(name_)),
          parent(std::move(parent_)) {}

    std::shared_ptr<const Archive> archive;
    std::shared_ptr<const Node> node; // synthetic, or nullptr for a TOC directory
    u32 dir;
    std::string name;
    VirtualDir parent;
    std::weak_ptr<VfsDirectory> self;
};

} // namespace

bool Archive::HasMagic(const VirtualFile& file) {
    if (!file || file->GetSize() < SuperblockSize) return false;
    std::array<u8, 4> magic{};
    return file->Read(magic.data(), magic.size(), 0) == magic.size() &&
           magic == std::array<u8, 4>{'N', 'U', 'P', 1};
}

std::shared_ptr<Archive> Archive::Open(VirtualFile file, std::string* error) {
    return Read(std::move(file), error, false);
}

std::shared_ptr<Archive> Archive::OpenMetadata(VirtualFile file, std::string* error) {
    return Read(std::move(file), error, true);
}

std::shared_ptr<Archive> Archive::Read(VirtualFile file, std::string* error, bool metadata_only) {
    if (!HasMagic(file)) {
        Fail(error, "not a NUP v1 file");
        return nullptr;
    }
    std::array<u8, SuperblockSize> super{};
    if (file->Read(super.data(), super.size(), 0) != super.size()) {
        Fail(error, "short superblock");
        return nullptr;
    }
    const u16 header_size = Load<u16>(&super[0x04]);
    const u64 toc_offset = Load<u64>(&super[0x08]);
    const u64 toc_size = Load<u64>(&super[0x10]);
    const u64 blob_offset = Load<u64>(&super[0x38]);
    const u64 file_size = Load<u64>(&super[0x40]);
    const u32 package_count = Load<u32>(&super[0x48]);
    const u32 flags = Load<u32>(&super[0x4C]);
    const u64 actual_size = file->GetSize();
    if (header_size < SuperblockSize || toc_offset != header_size || blob_offset % 16 != 0 ||
        toc_size < TocHeaderSize || toc_size > MaxTocSize || blob_offset < toc_offset + toc_size ||
        file_size != actual_size || blob_offset > file_size || package_count == 0 || flags != 0) {
        Fail(error, fmt::format("bad superblock (header {:#x}, toc {:#x}+{:#x}, blob {:#x}, size {} of {}, "
                                "{} packages, flags {:#x})",
                                header_size, toc_offset, toc_size, blob_offset, file_size, actual_size,
                                package_count, flags));
        return nullptr;
    }

    // A game list reads the package records only; their meta blocks are bounds-checked below.
    const u64 read_size =
        metadata_only ? std::min<u64>(toc_size, TocHeaderSize + u64{package_count} * PackageSize) : toc_size;
    std::vector<u8> toc(read_size);
    if (file->Read(toc.data(), toc.size(), toc_offset) != toc.size()) {
        Fail(error, "short TOC");
        return nullptr;
    }
    if (!metadata_only) {
        std::array<u8, 32> digest{};
        unsigned int digest_size = 0;
        if (!EVP_Digest(toc.data(), toc.size(), digest.data(), &digest_size, EVP_sha256(), nullptr) ||
            std::memcmp(digest.data(), &super[0x18], digest.size()) != 0) {
            Fail(error, "TOC SHA-256 mismatch");
            return nullptr;
        }
    }

    const u8* t = toc.data();
    const u64 dir_count = Load<u64>(t + 0x08);
    const u64 file_count = Load<u64>(t + 0x10);
    const u64 pool_off = Load<u64>(t + 0x18);
    const u64 pool_size = Load<u64>(t + 0x20);
    if (Load<u32>(t) != TocMagic || Load<u32>(t + 0x04) != package_count || dir_count == 0 ||
        dir_count >= NoDirectory || file_count >= NoDirectory - dir_count) {
        Fail(error, "bad TOC header");
        return nullptr;
    }
    const u64 packages_at = TocHeaderSize;
    const u64 dirs_at = packages_at + u64{package_count} * PackageSize;
    const u64 files_at = dirs_at + dir_count * DirSize;
    const u64 children_at = files_at + file_count * FileSize;
    if (children_at > toc_size || pool_off > toc_size || pool_size > toc_size - pool_off ||
        dirs_at > read_size) {
        Fail(error, "TOC tables out of bounds");
        return nullptr;
    }

    auto archive = std::shared_ptr<Archive>(new Archive());
    archive->backing = file;
    archive->toc_offset = toc_offset;
    archive->files_at = files_at;
    archive->metadata_only = metadata_only;
    archive->blob_offset = blob_offset;
    const auto read_packages = [&]() -> bool {
        for (u32 i = 0; i < package_count; ++i) {
            const u8* p = t + packages_at + u64{i} * PackageSize;
            Package package{Load<u64>(p),        Load<u32>(p + 0x08), Load<u32>(p + 0x0C),
                            Load<u32>(p + 0x10), Load<u32>(p + 0x14), static_cast<PackageKind>(p[0x18]),
                            p[0x19],             Load<u64>(p + 0x20), Load<u32>(p + 0x28)};
            const auto dir_ok = [&](u32 index) { return index == NoDirectory || index < dir_count; };
            if (p[0x18] >= 5 || package.root_dir >= dir_count || !dir_ok(package.exefs_dir) ||
                !dir_ok(package.romfs_dir) ||
                (package.meta_size && (package.meta_offset > file_size ||
                                       package.meta_size > file_size - package.meta_offset))) {
                Fail(error, fmt::format("package {} record is invalid (kind {})", i, p[0x18]));
                return false;
            }
            archive->packages.push_back(package);
        }
        for (const auto& package : archive->packages)
            if (archive->RawMeta(package).contains(0)) archive->zero_based_meta = true;
        return true;
    };
    if (metadata_only) return read_packages() ? archive : nullptr;
    archive->string_pool.assign(t + pool_off, t + pool_off + pool_size);
    const auto name_ok = [&](u32 offset, u32 length) {
        return u64{offset} + length <= pool_size;
    };

    archive->dirs.reserve(dir_count);
    u64 child_slots = 0;
    for (u64 i = 0; i < dir_count; ++i) {
        const u8* d = t + dirs_at + i * DirSize;
        Directory dir{Load<u32>(d), Load<u32>(d + 4), Load<u32>(d + 8), Load<u32>(d + 12)};
        if (!name_ok(dir.name_off, dir.name_len)) {
            Fail(error, fmt::format("directory {} name out of bounds", i));
            return nullptr;
        }
        child_slots = std::max<u64>(child_slots, u64{dir.first_child} + dir.child_count);
        archive->dirs.push_back(dir);
    }
    if (children_at + child_slots * 4 > toc_size) {
        Fail(error, "children array out of bounds");
        return nullptr;
    }
    archive->children.resize(child_slots);
    std::memcpy(archive->children.data(), t + children_at, child_slots * 4);
    for (const u32 id : archive->children) {
        if (id >= dir_count + file_count) {
            Fail(error, "child node out of range");
            return nullptr;
        }
    }

    archive->files.reserve(file_count);
    for (u64 i = 0; i < file_count; ++i) {
        const u8* f = t + files_at + i * FileSize;
        File record{Load<u32>(f), Load<u32>(f + 4), Load<u64>(f + 8), Load<u64>(f + 0x10),
                    Load<u64>(f + 0x18), f[0x40]};
        const bool empty = record.extent_size == 0;
        const bool extent_ok =
            empty || (record.extent_offset >= blob_offset && record.extent_offset % 16 == 0 &&
                      record.extent_offset <= file_size && record.extent_size <= file_size - record.extent_offset);
        if (!name_ok(record.name_off, record.name_len) || record.codec > 1 || !extent_ok ||
            (record.codec == 0 && record.extent_size != record.size) ||
            (record.codec == 1 && empty && record.size != 0)) {
            Fail(error, fmt::format("file {} record is invalid (codec {})", i, record.codec));
            return nullptr;
        }
        archive->files.push_back(record);
    }

    if (!read_packages()) return nullptr;
    LOG_INFO(Loader, "NUP {}: {} packages, {} directories, {} files{}", file->GetName(), package_count,
             dir_count, file_count, archive->zero_based_meta ? " (meta types numbered from 0)" : "");
    return archive;
}

Archive::~Archive() {
    for (const auto& [index, fd] : cache_files) ::close(fd);
}

std::string_view Archive::NameView(u32 offset, u32 length) const {
    if (u64{offset} + length > string_pool.size()) return {};
    return std::string_view(reinterpret_cast<const char*>(string_pool.data()) + offset, length);
}

std::string Archive::Name(u32 offset, u32 length) const {
    return std::string(NameView(offset, length));
}

u32 Archive::FindChild(u32 dir, std::string_view name) const {
    const auto& record = dirs[dir];
    for (u32 slot = record.first_child; slot < record.first_child + record.child_count; ++slot) {
        const u32 id = children[slot];
        const bool is_dir = id < dirs.size();
        const u32 off = is_dir ? dirs[id].name_off : files[id - dirs.size()].name_off;
        const u32 len = is_dir ? dirs[id].name_len : files[id - dirs.size()].name_len;
        if (NameView(off, len) == name) return id;
    }
    return NoDirectory;
}

const Package* Archive::Base() const {
    for (const auto& package : packages)
        if (package.kind == PackageKind::Base) return &package;
    return nullptr;
}

const Package* Archive::MetadataPackage() const {
    if (const auto* base = Base()) return base;
    return &packages.front();
}

u64 Archive::ProgramTitleID() const {
    if (const auto* base = Base()) return base->title_id;
    for (const auto& package : packages)
        if (package.kind == PackageKind::Update) return package.title_id & ~u64{0x800};
    return 0;
}

std::map<u16, std::vector<u8>> Archive::Meta(const Package& package) const {
    auto raw = RawMeta(package);
    if (!zero_based_meta) return raw;
    std::map<u16, std::vector<u8>> out;
    for (auto& [type, payload] : raw) out.emplace(static_cast<u16>(type + 1), std::move(payload));
    return out;
}

std::map<u16, std::vector<u8>> Archive::RawMeta(const Package& package) const {
    std::map<u16, std::vector<u8>> out;
    if (!package.meta_size) return out;
    std::vector<u8> block(package.meta_size);
    if (backing->Read(block.data(), block.size(), package.meta_offset) != block.size()) return out;
    for (std::size_t at = 0; at + 8 <= block.size();) {
        const u16 type = Load<u16>(&block[at]);
        const u32 size = Load<u32>(&block[at + 4]);
        if (size > block.size() - at - 8) break;
        out.try_emplace(type, block.begin() + at + 8, block.begin() + at + 8 + size);
        at += 8 + std::size_t{size};
    }
    return out;
}

std::optional<std::vector<u8>> Archive::MetaRecord(const Package& package, MetaType type) const {
    auto meta = Meta(package);
    const auto found = meta.find(static_cast<u16>(type));
    if (found == meta.end()) return std::nullopt;
    return std::move(found->second);
}

std::string Archive::MetaString(const Package& package, MetaType type) const {
    const auto record = MetaRecord(package, type);
    return record ? std::string(record->begin(), record->end()) : std::string{};
}

VirtualDir Archive::Tree(const std::string& name) const {
    if (metadata_only) return nullptr;
    auto root = std::make_shared<Node>();
    root->name = name;
    const auto package_node = [](const Package& package, std::string node_name) {
        auto node = std::make_shared<Node>();
        node->name = std::move(node_name);
        for (const auto& [sub, index] : {std::pair{"exefs", package.exefs_dir}, std::pair{"romfs", package.romfs_dir}}) {
            if (index == NoDirectory) continue;
            auto child = std::make_shared<Node>();
            child->name = sub;
            child->dir = index;
            node->children.push_back(std::move(child));
        }
        return node;
    };
    auto updates = std::make_shared<Node>();
    updates->name = "updates";
    auto dlc = std::make_shared<Node>();
    dlc->name = "dlc";
    std::map<u64, std::shared_ptr<Node>> dlc_titles;
    bool have_base = false;
    for (const auto& package : packages) {
        switch (package.kind) {
        case PackageKind::Base:
            if (!have_base) root->children.push_back(package_node(package, "base"));
            have_base = true;
            break;
        case PackageKind::Update: {
            const std::string version = std::to_string(package.version);
            if (std::none_of(updates->children.begin(), updates->children.end(),
                             [&](const auto& child) { return child->name == version; }))
                updates->children.push_back(package_node(package, version));
            break;
        }
        case PackageKind::AddOn: {
            auto& title = dlc_titles[package.title_id];
            if (!title) {
                title = std::make_shared<Node>();
                title->name = fmt::format("{:016x}", package.title_id);
            }
            title->children.push_back(package_node(package, std::to_string(title->children.size())));
            break;
        }
        default:
            break;
        }
    }
    if (!updates->children.empty()) root->children.push_back(updates);
    for (auto& [title_id, title] : dlc_titles) dlc->children.push_back(title);
    if (!dlc->children.empty()) root->children.push_back(dlc);

    // Control data and icon where the extracted-dump loader looks for them (FindSidecarDirs).
    const Package& meta_package = *MetadataPackage();
    auto meta = Meta(meta_package);
    if (const auto nacp = meta.find(static_cast<u16>(MetaType::Nacp)); nacp != meta.end())
        root->files.emplace_back("control.nacp", std::move(nacp->second));
    if (const auto icon = meta.find(static_cast<u16>(MetaType::Icon)); icon != meta.end())
        root->files.emplace_back("icon_AmericanEnglish.dat", std::move(icon->second));

    return NupDirectory::Make(shared_from_this(), root, NoDirectory, name, nullptr);
}

VirtualFile Archive::MainFile(const VirtualDir& tree) const {
    if (!tree) return nullptr;
    if (const auto base = tree->GetSubdirectory("base"))
        if (const auto exefs = base->GetSubdirectory("exefs"))
            if (auto main = exefs->GetFile("main")) return main;
    const auto updates = tree->GetSubdirectory("updates");
    if (!updates) return nullptr;
    VirtualFile newest;
    u64 newest_version = 0;
    for (const auto& update : updates->GetSubdirectories()) {
        const u64 version = std::stoull(update->GetName());
        if (const auto exefs = update->GetSubdirectory("exefs"))
            if (auto main = exefs->GetFile("main"); main && (!newest || version > newest_version)) {
                newest = std::move(main);
                newest_version = version;
            }
    }
    return newest;
}

std::size_t Archive::ReadFile(u32 index, u8* data, std::size_t length, std::size_t offset) const {
    const File& record = files[index];
    if (offset >= record.size) return 0;
    length = std::min<u64>(length, record.size - offset);
    if (record.codec == 0) return backing->Read(data, length, record.extent_offset + offset);
    if (record.size <= MemoryFileLimit) {
        const auto copy = MemoryCopy(index);
        if (!copy) return 0;
        std::memcpy(data, copy->data() + offset, length);
        return length;
    }
    const int fd = CacheFile(index);
    if (fd < 0) return 0;
    std::size_t done = 0;
    while (done < length) {
        const ssize_t got = ::pread(fd, data + done, length - done, static_cast<off_t>(offset + done));
        if (got <= 0) break;
        done += static_cast<std::size_t>(got);
    }
    return done;
}

std::shared_ptr<const std::vector<u8>> Archive::MemoryCopy(u32 index) const {
    {
        const std::lock_guard lock(cache_mutex);
        if (const auto found = memory_cache.find(index); found != memory_cache.end()) {
            found->second.last_use = ++use_counter;
            return found->second.data;
        }
    }
    const File& record = files[index];
    std::vector<u8> compressed(record.extent_size);
    if (backing->Read(compressed.data(), compressed.size(), record.extent_offset) != compressed.size())
        return nullptr;
    auto plain = std::make_shared<std::vector<u8>>(record.size);
    if (Common::Compression::DecompressDataZSTD(compressed, *plain) != record.size) {
        LOG_ERROR(Loader, "NUP: cannot decompress {}", Name(record.name_off, record.name_len));
        return nullptr;
    }
    const std::lock_guard lock(cache_mutex);
    while (memory_cache_bytes + record.size > MemoryBudget && !memory_cache.empty()) {
        const auto oldest = std::min_element(memory_cache.begin(), memory_cache.end(), [](const auto& a, const auto& b) {
            return a.second.last_use < b.second.last_use;
        });
        memory_cache_bytes -= oldest->second.data->size();
        memory_cache.erase(oldest);
    }
    const auto [entry, inserted] = memory_cache.try_emplace(index, Decompressed{plain, ++use_counter});
    if (inserted) memory_cache_bytes += record.size;
    return entry->second.data;
}

int Archive::CacheFile(u32 index) const {
    const std::lock_guard lock(cache_mutex);
    if (const auto found = cache_files.find(index); found != cache_files.end()) return found->second;
    const File& record = files[index];
    std::array<u8, 32> sha256{};
    if (backing->Read(sha256.data(), sha256.size(), toc_offset + files_at + u64{index} * FileSize + 0x20) !=
        sha256.size())
        return -1;
    const std::filesystem::path folder = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "nup";
    const std::filesystem::path path = folder / (Hex(sha256) + ".bin");
    const std::string label = Name(record.name_off, record.name_len);
    std::error_code size_error;
    if (std::filesystem::file_size(path, size_error) != record.size || size_error) {
        (void)Common::FS::CreateDirs(folder);
        const std::filesystem::path part = folder / (Hex(sha256) + ".part");
        const int out = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) {
            LOG_ERROR(Loader, "NUP: cannot write {}", part.string());
            return -1;
        }
        LOG_INFO(Loader, "NUP: decompressing {} ({} MiB) to {}", label, record.size >> 20, path.string());
        ZSTD_DStream* stream = ZSTD_createDStream();
        EVP_MD_CTX* hash = EVP_MD_CTX_new();
        EVP_DigestInit_ex(hash, EVP_sha256(), nullptr);
        std::vector<u8> in(ZSTD_DStreamInSize() * 8);
        std::vector<u8> plain(ZSTD_DStreamOutSize() * 8);
        u64 read = 0, written = 0;
        bool ok = stream != nullptr && !ZSTD_isError(ZSTD_initDStream(stream));
        size_t pending = 1;
        while (ok && read < record.extent_size) {
            const std::size_t chunk = std::min<u64>(in.size(), record.extent_size - read);
            if (backing->Read(in.data(), chunk, record.extent_offset + read) != chunk) {
                ok = false;
                break;
            }
            read += chunk;
            ZSTD_inBuffer input{in.data(), chunk, 0};
            while (ok && input.pos < input.size) {
                ZSTD_outBuffer output{plain.data(), plain.size(), 0};
                pending = ZSTD_decompressStream(stream, &output, &input);
                if (ZSTD_isError(pending) || written + output.pos > record.size) {
                    ok = false;
                    break;
                }
                EVP_DigestUpdate(hash, plain.data(), output.pos);
                for (std::size_t at = 0; at < output.pos;) {
                    const ssize_t put = ::write(out, plain.data() + at, output.pos - at);
                    if (put <= 0) {
                        ok = false;
                        break;
                    }
                    at += static_cast<std::size_t>(put);
                }
                written += output.pos;
            }
        }
        std::array<u8, 32> digest{};
        unsigned int digest_size = 0;
        EVP_DigestFinal_ex(hash, digest.data(), &digest_size);
        EVP_MD_CTX_free(hash);
        ZSTD_freeDStream(stream);
        const bool closed = ::close(out) == 0;
        ok = ok && closed && pending == 0 && written == record.size && digest == sha256;
        if (!ok) {
            LOG_ERROR(Loader, "NUP: cannot decompress {} (wrote {} of {} bytes)", label, written, record.size);
            std::error_code ignored;
            std::filesystem::remove(part, ignored);
            return -1;
        }
        std::error_code rename_error;
        std::filesystem::rename(part, path, rename_error);
        if (rename_error) return -1;
    }
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) cache_files.emplace(index, fd);
    return fd;
}

} // namespace FileSys::Nup
