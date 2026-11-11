// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <span>
#include <string_view>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/nup.h"

namespace Loader {
namespace {

using FileSys::Nup::MetaType;

void CopyText(std::span<char> field, std::string_view text) {
    const std::size_t length = std::min(text.size(), field.size() - 1);
    std::memcpy(field.data(), text.data(), length);
    field[length] = '\0';
}

// NACP language entry of a BCP-47 tag (SPEC.md appendix B), or -1.
int LanguageIndex(std::string tag) {
    for (char& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static constexpr std::array<std::string_view, 16> Tags{"en-us", "en-gb", "ja",      "fr",      "de",   "es-419",
                                                           "es",    "it",    "nl",      "fr-ca",   "pt",   "ru",
                                                           "ko",    "zh-hant", "zh-hans", "pt-br"};
    for (std::size_t i = 0; i < Tags.size(); ++i)
        if (Tags[i] == tag) return static_cast<int>(i);
    return -1;
}

// Control data from a package's meta block (titles, publisher, display version).
std::optional<FileSys::NACP> MakeControl(const FileSys::Nup::Archive& archive, const std::string& version) {
    const auto& package = *archive.MetadataPackage();
    auto meta = archive.Meta(package);
    const auto text = [&](MetaType type) {
        const auto found = meta.find(static_cast<u16>(type));
        return found != meta.end() ? std::string(found->second.begin(), found->second.end()) : std::string{};
    };
    std::string title = text(MetaType::TitleName);
    if (title.empty()) title = text(MetaType::SourceDumpName);
    const std::string publisher = text(MetaType::Publisher);
    FileSys::RawNACP raw{};
    raw.supported_languages = static_cast<FileSys::SupportedLanguage>(0x3FFFF);
    raw.save_data_owner_id = archive.ProgramTitleID();
    auto& entries = raw.language_entries.language_entries;
    if (const auto localized = meta.find(static_cast<u16>(MetaType::LocalizedTitles)); localized != meta.end()) {
        const auto& bytes = localized->second;
        for (std::size_t at = 0; at < bytes.size();) {
            const std::size_t tag_length = bytes[at];
            if (at + 1 + tag_length + 4 > bytes.size()) break;
            const std::string tag(bytes.begin() + at + 1, bytes.begin() + at + 1 + tag_length);
            u32 length{};
            std::memcpy(&length, &bytes[at + 1 + tag_length], sizeof(length));
            const std::size_t data = at + 1 + tag_length + 4;
            if (length > bytes.size() - data) break;
            if (const int index = LanguageIndex(tag); index >= 0)
                CopyText(entries[index].application_name, std::string_view(
                    reinterpret_cast<const char*>(bytes.data() + data), length));
            at = data + length;
        }
    }
    if (title.empty() && publisher.empty() && version.empty() && entries[0].application_name[0] == '\0')
        return std::nullopt;
    for (auto& entry : entries) {
        if (entry.application_name[0] == '\0' && !title.empty()) CopyText(entry.application_name, title);
        if (!publisher.empty()) CopyText(entry.developer_name, publisher);
    }
    CopyText(raw.version_string, version);
    std::vector<u8> bytes(sizeof(raw));
    std::memcpy(bytes.data(), &raw, sizeof(raw));
    return FileSys::NACP(std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "control.nacp"));
}

std::string DecodeVersion(u32 version) {
    return version ? fmt::format("{}.{}.{}", version >> 16, (version >> 8) & 0xFF, version & 0xFF) : std::string{};
}

} // namespace

AppLoader_Nup::AppLoader_Nup(FileSys::VirtualFile file_) : AppLoader(std::move(file_)) {
    std::string error;
    archive = FileSys::Nup::Archive::Open(file, &error);
    if (!archive) {
        LOG_ERROR(Loader, "{} is not a usable NUP: {}", file ? file->GetName() : "", error);
        return;
    }
    std::string name = file->GetName();
    if (const auto dot = name.find_last_of('.'); dot != std::string::npos) name.resize(dot);
    const auto tree = archive->Tree(name);
    const auto main = archive->MainFile(tree);
    if (!main) {
        LOG_ERROR(Loader, "NUP {} has no exefs/main in its base or updates", file->GetName());
        return;
    }
    inner = std::make_unique<AppLoader_DeconstructedRomDirectory>(main);
    const auto& meta_package = *archive->MetadataPackage();
    title = archive->MetaString(meta_package, FileSys::Nup::MetaType::TitleName);
    display_version = archive->MetaString(meta_package, FileSys::Nup::MetaType::DisplayVersion);
    if (display_version.empty()) {
        // The newest update's version, else the base's (NCM encoding, SPEC §3.1.1).
        u32 newest = 0;
        for (const auto& package : archive->Packages())
            if (package.kind == FileSys::Nup::PackageKind::Update || package.kind == FileSys::Nup::PackageKind::Base)
                newest = std::max(newest, package.version);
        display_version = DecodeVersion(newest);
    }
    FileSys::NACP sidecar;
    if (inner->ReadControlData(sidecar) != ResultStatus::Success)
        control = MakeControl(*archive, display_version);
}

FileType AppLoader_Nup::IdentifyType(const FileSys::VirtualFile& nup_file) {
    return FileSys::Nup::Archive::HasMagic(nup_file) ? FileType::NUP : FileType::Error;
}

AppLoader_Nup::LoadResult AppLoader_Nup::Load(Kernel::KProcess& process, Core::System& system) {
    if (!inner) return {ResultStatus::ErrorBadNupHeader, {}};
    return inner->Load(process, system);
}

ResultStatus AppLoader_Nup::ReadRomFS(FileSys::VirtualFile& out_file) {
    return inner ? inner->ReadRomFS(out_file) : ResultStatus::ErrorBadNupHeader;
}

ResultStatus AppLoader_Nup::ReadIcon(std::vector<u8>& buffer) {
    return inner ? inner->ReadIcon(buffer) : ResultStatus::ErrorBadNupHeader;
}

ResultStatus AppLoader_Nup::ReadProgramId(u64& out_program_id) {
    if (!archive) return ResultStatus::ErrorBadNupHeader;
    if (inner && inner->ReadProgramId(out_program_id) == ResultStatus::Success && out_program_id != 0)
        return ResultStatus::Success;
    out_program_id = archive->ProgramTitleID();
    return ResultStatus::Success;
}

ResultStatus AppLoader_Nup::ReadTitle(std::string& out_title) {
    // The meta block's title wins over control data (SPEC §5: the TLV wins for metadata).
    if (!title.empty()) {
        out_title = title;
        return ResultStatus::Success;
    }
    return inner ? inner->ReadTitle(out_title) : ResultStatus::ErrorBadNupHeader;
}

ResultStatus AppLoader_Nup::ReadControlData(FileSys::NACP& out_control) {
    if (!inner) return ResultStatus::ErrorBadNupHeader;
    if (control) {
        out_control = *control;
        return ResultStatus::Success;
    }
    return inner->ReadControlData(out_control);
}

ResultStatus AppLoader_Nup::ReadDisplayVersion(std::string& version) {
    if (!display_version.empty()) {
        version = display_version;
        return ResultStatus::Success;
    }
    return inner ? inner->ReadDisplayVersion(version) : ResultStatus::ErrorBadNupHeader;
}

bool AppLoader_Nup::IsRomFSUpdatable() const {
    return inner && inner->IsRomFSUpdatable();
}

ResultStatus AppLoader_Nup::ReadNSOModules(Modules& modules) {
    return inner ? inner->ReadNSOModules(modules) : ResultStatus::ErrorNotInitialized;
}

} // namespace Loader
