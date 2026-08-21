#pragma once

#include <cstddef>
#include <type_traits>

#include "core/types.h"

namespace exchange::matching {
    // forward declaration
    struct Level;

    struct alignas(64) Order {
        core::OrderId id{ 0 };
        core::Side side{ core::Side::BUY };

        // bound price
        core::Price price{ 0 };

        // remaining qty of the order
        core::Quantity qty{ 0 };
        core::Quantity original_qty{ 0 };
        core::Quantity display_qty{ 0 };
        // for iceberg orders
        core::Quantity hidden_qty{ 0 };

        core::Timestamp timestamp{ 0 };

        core::OrderType type{ core::OrderType::LIMIT };
        core::OrderStatus status{ core::OrderStatus::NEW };
        core::ParticipantId participant_id{ 0 };

        // for stop orders
        core::Price trigger_price{ 0 };
        // in icebery order
        core::Quantity peak_qty{ 0 };

        // LL according to time as they arrive in a price level
        Order* prev{ nullptr };
        Order* next{ nullptr };
        Level* parent_level{ nullptr };

        [[nodiscard]] bool is_stop_order() const noexcept {
            return type == core::OrderType::STOP ||
                type == core::OrderType::STOP_LIMIT;
        }
    };

    static_assert(sizeof(Order) <= 128,
        "Order should stay compact enough for cache-friendly access.");
    static_assert(std::is_standard_layout_v<Order>);
}
