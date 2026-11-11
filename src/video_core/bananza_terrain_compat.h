// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace Tegra {

class BananzaTerrainDrawCompat {
public:
    using Address = std::uint64_t;
    using Count = std::uint32_t;

    bool RecordDepthDraw(Address address_space, Address counter, Address indices,
                         Count index_capacity, Count instances) {
        if (!counter || !indices || index_capacity < 3 || instances != 1) {
            return false;
        }
        if (!depth_pass) {
            draws.clear();
            depth_pass = true;
        }
        const Key key{address_space, counter};
        if (draws.size() < 65536 || draws.contains(key)) {
            const auto found = draws.find(key);
            const bool initialize = found == draws.end() || found->second.indices != indices;
            draws.insert_or_assign(key, Draw{indices, index_capacity, instances});
            return initialize;
        }
        return false;
    }

    void BeginColorPass() noexcept {
        depth_pass = false;
    }

    std::optional<Count> FindInstances(Address address_space, Address counter, Address indices,
                                       Count index_count) const {
        if (depth_pass || !index_count || index_count % 3 != 0) {
            return std::nullopt;
        }
        const auto found = draws.find(Key{address_space, counter});
        if (found == draws.end() || found->second.indices != indices ||
            index_count > found->second.index_capacity) {
            return std::nullopt;
        }
        return found->second.instances;
    }

private:
    using Key = std::pair<Address, Address>;
    struct Draw {
        Address indices;
        Count index_capacity;
        Count instances;
    };
    std::map<Key, Draw> draws;
    bool depth_pass{};
};

} // namespace Tegra
