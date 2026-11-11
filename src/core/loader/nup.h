// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>

#include "core/file_sys/control_metadata.h"

#include "core/file_sys/nup.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/loader.h"

namespace Loader {

// A .nup container (core/file_sys/nup.h). It is shown as an extracted dump and run by the
// extracted-dump loader, so updates in the container shadow the base as in a dump's
// updates/<version>/ folders; title, display version and icon come from its meta block.
class AppLoader_Nup final : public AppLoader {
public:
    explicit AppLoader_Nup(FileSys::VirtualFile file_);

    static FileType IdentifyType(const FileSys::VirtualFile& nup_file);

    FileType GetFileType() const override {
        return IdentifyType(file);
    }

    LoadResult Load(Kernel::KProcess& process, Core::System& system) override;

    ResultStatus ReadRomFS(FileSys::VirtualFile& out_file) override;
    ResultStatus ReadIcon(std::vector<u8>& buffer) override;
    ResultStatus ReadProgramId(u64& out_program_id) override;
    ResultStatus ReadTitle(std::string& title) override;
    ResultStatus ReadControlData(FileSys::NACP& control) override;
    ResultStatus ReadDisplayVersion(std::string& version) override;
    bool IsRomFSUpdatable() const override;
    ResultStatus ReadNSOModules(Modules& modules) override;

private:
    std::shared_ptr<FileSys::Nup::Archive> archive;
    std::unique_ptr<AppLoader_DeconstructedRomDirectory> inner;
    std::string title;
    std::string display_version;
    // Control data made from the meta block when the container has none (most have none):
    // title, localized titles, publisher, display version, as upstream Eglantine's NUP loader.
    std::optional<FileSys::NACP> control;
};

} // namespace Loader
