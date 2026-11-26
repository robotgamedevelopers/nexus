// nova-config accepts AI names through 128 bytes. A registered sender can live on another host
// or harness, so the local Nexus ai/driver limit of 64 bytes does not apply to its source label.
#pragma once

#include <cstddef>
#include <string_view>

namespace nexus::sync {
inline constexpr std::size_t kMaxBusPeerBytes = 128;
inline constexpr std::string_view kBusPeerCharacters =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-";

inline bool busPeerName(std::string_view name) {
    return !name.empty() && name.size() <= kMaxBusPeerBytes &&
           name.find_first_not_of(kBusPeerCharacters) == name.npos;
}
} // namespace nexus::sync
