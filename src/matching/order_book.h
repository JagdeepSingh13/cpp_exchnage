#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <unordered_map>

#include "core/memory_pool.h"
#include "core/types.h"
#include "matching/events.h"
#include "matching/level.h"

namespace exchange::matching {

    struct OrderRecord {
        Order* order{ nullptr };
        core::OrderStatus last_status{ core::OrderStatus::NEW };
    };

    static_assert(std::is_trivially_copyable_v<OrderRecord>);

    struct PriceLevelView {
        core::Price price{ 0 };
        core::Quantity qty{ 0 };
    };

    static_assert(std::is_trivially_copyable_v<PriceLevelView>);

    class MonotonicBuffer {
    public:
        // : is used to initialize the members
        explicit MonotonicBuffer(std::size_t bytes)
            : storage_(std::make_unique<std::byte[]>(bytes)),
            resource_(storage_.get(), bytes, std::pmr::null_memory_resource())
        {
        }

        MonotonicBuffer(const MonotonicBuffer&) = delete;
        MonotonicBuffer& operator=(const MonotonicBuffer&) = delete;
        MonotonicBuffer(MonotonicBuffer&&) = delete;
        MonotonicBuffer& operator=(MonotonicBuffer&&) = delete;

        [[nodiscard]] std::pmr::memory_resource* resource() noexcept {
            return &resource_;
        }

    private:
        std::unique_ptr<std::byte[]> storage_;
        std::pmr::monotonic_buffer_resource resource_;
    };

    template <
        std::size_t OrderCapacity = 1'000'000, std::size_t LevelCapacity = 10'000,
        std::size_t EventCapacity = 262'144, std::size_t StopCapacity = 65'536>
    class OrderBook {
        using BidMap = std::pmr::map<core::Price, Level*, std::greater<core::Price>>;
        using AskMap = std::pmr::map<core::Price, Level*, std::less<core::Price>>;
        // multi-map -> since one price can have multiple orders
        using StopMap = std::pmr::multimap<core::Price, Order*>;
        using OrderMap = std::pmr::unordered_map<core::OrderId, OrderRecord>;

        static constexpr std::size_t ORDER_INDEX_BYTES =
            (OrderCapacity * 128U) + (OrderCapacity * 16U);
        static constexpr std::size_t LEVEL_INDEX_BYTES = LevelCapacity * 128U;
        static constexpr std::size_t STOP_INDEX_BYTES = StopCapacity * 128U;

    public:
        explicit OrderBook(core::Symbol symbol)
            : symbol_(symbol), order_index_buffer_(ORDER_INDEX_BYTES),
            bid_index_buffer_(LEVEL_INDEX_BYTES), ask_index_buffer_(LEVEL_INDEX_BYTES),
            stop_buy_index_buffer_(STOP_INDEX_BYTES), stop_sell_index_buffer_(STOP_INDEX_BYTES),
            bids_(std::greater<core::Price>{}, bid_index_buffer_.resource()),
            asks_(std::less<core::Price>{}, ask_index_buffer_.resource()),
            stop_buys_(stop_buy_index_buffer_.resource()),
            stop_sells_(stop_sell_index_buffer_.resource()),
            orders_(order_index_buffer_.resource()),
            event_storage_(std::make_unique<Event[]>(EventCapacity))
        {
            orders_.reserve(OrderCapacity);
        }

        OrderBook(const OrderBook&) = delete;
        OrderBook& operator=(const OrderBook&) = delete;
        OrderBook(OrderBook&&) = delete;
        OrderBook& operator=(OrderBook&&) = delete;

        core::Symbol symbol_{};

        MonotonicBuffer order_index_buffer_;
        MonotonicBuffer bid_index_buffer_;
        MonotonicBuffer ask_index_buffer_;
        MonotonicBuffer stop_buy_index_buffer_;
        MonotonicBuffer stop_sell_index_buffer_;

        BidMap bids_;
        AskMap asks_;
        // triggers at that price
        StopMap stop_buys_;
        StopMap stop_sells_;
        OrderMap orders_;

        std::unique_ptr<Event[]> event_storage_;
    };
}