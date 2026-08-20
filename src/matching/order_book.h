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

        std::span<const Event>
            add_order(
                core::OrderId order_id, core::Side side, core::Price price,
                core::Quantity qty, core::OrderType order_type,
                core::Timestamp timestamp, core::ParticipantId participant_id,
                core::Price trigger_price = 0, core::Quantity display_qty = 0
            )
        {
            reset_events();

            // if we get a reason to reject the order
            if (const auto reason =
                validate_new_order(order_id, side, price, qty, order_type,
                    trigger_price, display_qty))
            {
                emit_rejected(timestamp, order_id, *reason);
                return events();
            }


        }

    private:

        void reset_events() noexcept { event_count_ = 0; }

        // span ->  lightweight, non-owning view of cont. seq.
        [[nodiscard]] std::span<const Event> events() const {
            return std::span<const Event>(event_storage_.get(), event_count_);
        }

        [[nodiscard]] const Level*
            opposite_best_level(core::Side side) const noexcept
        {
            return side == core::Side::BUY ? best_ask_ : best_bid_;
        }

        [[nodiscard]] bool
            would_cross(core::Side side, core::Price price) const noexcept
        {
            const Level* best_opposite = opposite_best_level(side);
            if (best_opposite == nullptr)
                return false;

            if (side == core::Side::BUY) {
                return best_opposite->price <= price;
            }
            return best_opposite->price >= price;
        }

        [[nodiscard]] core::Quantity
            available_liquidity(core::Side side, core::Price limit_price) const noexcept {
            std::uint64_t available = 0;

            if (side == core::Side::BUY) {
                // traverse the asks map
                for (const auto& [price, level] : asks_) {
                    // no qty at that level
                    if (level == nullptr)
                        continue;
                    if (price > limit_price)
                        break;

                    available += level->total_qty;
                }
            }
            else {
                for (const auto& [price, level] : bids_) {
                    if (level == nullptr)
                        continue;
                    if (price < limit_price)
                        break;
                    available += level->total_qty;
                }
            }

            return static_cast<core::Quantity>(std::min<std::uint64_t>(
                available, std::numeric_limits<core::Quantity>::max()));
        }

        [[nodiscard]] std::optional<ReasonCode>
            validate_new_order(core::OrderId order_id, core::Side side, core::Price price,
                core::Quantity qty, core::OrderType order_type,
                core::Price trigger_price,
                core::Quantity display_qty) const noexcept {
            if (qty == 0) {
                return ReasonCode::ZERO_QUANTITY;
            }

            if (orders_.contains(order_id)) {
                return ReasonCode::DUPLICATE_ORDER_ID;
            }

            const bool requires_limit_price =
                order_type == core::OrderType::LIMIT ||
                order_type == core::OrderType::IOC ||
                order_type == core::OrderType::FOK ||
                order_type == core::OrderType::GTC ||
                order_type == core::OrderType::STOP_LIMIT ||
                order_type == core::OrderType::ICEBERG ||
                order_type == core::OrderType::POST_ONLY;

            if (requires_limit_price && price <= 0) {
                return ReasonCode::NEGATIVE_PRICE;
            }

            if ((order_type == core::OrderType::STOP ||
                order_type == core::OrderType::STOP_LIMIT) &&
                trigger_price <= 0) {
                return ReasonCode::NEGATIVE_PRICE;
            }

            if (order_type == core::OrderType::ICEBERG &&
                (display_qty == 0 || display_qty > qty)) {
                return ReasonCode::INVALID_ICEBERG_DISPLAY;
            }

            if (order_type == core::OrderType::MARKET &&
                opposite_best_level(side) == nullptr) {
                return ReasonCode::BOOK_EMPTY;
            }

            if (order_type == core::OrderType::POST_ONLY && would_cross(side, price)) {
                return ReasonCode::POST_ONLY_WOULD_CROSS;
            }

            if (order_type == core::OrderType::FOK &&
                available_liquidity(side, price) < qty) {
                return ReasonCode::FOK_INSUFFICIENT_LIQUIDITY;
            }

            return std::nullopt;
        }

        void push_event(const Event& event) noexcept {
            if (event_count_ >= EventCapacity) [[unlikely]] {
                std::abort();
            }

            event_storage_[event_count_++] = event;
        }

        void emit_rejected(core::Timestamp timestamp, core::OrderId order_id,
            ReasonCode reason)
        {
            push_event(Event::make(OrderRejected{
                .sequence_number = next_sequence(),  // todo the seq no.
                .timestamp = timestamp,
                .order_id = order_id,
                .reason_code = reason,
                }));
        }

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

        Level* best_bid_{ nullptr };
        Level* best_ask_{ nullptr };

        core::MemoryPool<Order, OrderCapacity> order_pool_{};

        std::unique_ptr<Event[]> event_storage_;
        std::size_t event_count_{ 0 };
    };
}