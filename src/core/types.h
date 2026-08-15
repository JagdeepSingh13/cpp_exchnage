#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

// memory assignment and padding while adding fields in struct in order

namespace exchange::core {

    using Price = std::int64_t;

    using Quantity = std::uint32_t;

    using OrderId = std::uint64_t;

    using SequenceNumber = std::uint64_t;

    using Timestamp = std::uint64_t;

    using ParticipantId = std::uint32_t;

    using MatchId = std::uint64_t;

    // we used int in Price so *10000
    inline constexpr Price PRICE_SCALE = 10'000;

    // by default enum has 4 bytes assigned but only 1 with uint8_t
    enum class Side : std::uint8_t {
        BUY,
        SELL,
    };

}