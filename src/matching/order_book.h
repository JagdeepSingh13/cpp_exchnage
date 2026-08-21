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

            Order* order = order_pool_.allocate();
            if (order == nullptr) [[unlikely]] {
                emit_rejected(timestamp, order_id, ReasonCode::ORDER_POOL_EXHAUSTED);
                return events();
            }

            const core::Quantity peak_qty =
                order_type == core::OrderType::ICEBERG ? display_qty : 0U;

            order = std::construct_at(order, Order{
                .id = order_id,
                .side = side,
                .price = price,
                .qty = qty,
                .original_qty = qty,
                .display_qty = qty,
                .hidden_qty = 0,
                .timestamp = timestamp,
                .type = order_type,
                .status = core::OrderStatus::NEW,
                .participant_id = participant_id,
                .trigger_price = trigger_price,
                .peak_qty = peak_qty,
                });

            // to prepare qtys for iceberg orders
            prepare_visible_slice(*order);

            orders_.insert_or_assign(
                order_id,
                OrderRecord{ .order = order, .last_status = core::OrderStatus::NEW });

            // run the matching logic for the order
            // if not then add to the order book
            execute_inbound_order(*order, timestamp, true);
            return events();
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
                core::Quantity qty, core::OrderType order_type, core::Price trigger_price,
                core::Quantity display_qty) const noexcept
        {
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

        void prepare_visible_slice(Order& order) noexcept {
            if (order.type != core::OrderType::ICEBERG) {
                order.display_qty = order.qty;
                order.hidden_qty = 0;
                return;
            }

            order.display_qty = std::min(order.qty, order.peak_qty);
            order.hidden_qty = order.qty - order.display_qty;
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

        void emit_accepted(core::Timestamp timestamp, const Order& order) {
            push_event(Event::make(OrderAccepted{
                .sequence_number = next_sequence(), // TODO: ADD Sequence number
                .timestamp = timestamp,
                .order_id = order.id,
                .symbol = symbol_,
                .side = order.side,
                .price = order.type == core::OrderType::STOP ? order.trigger_price : order.price,
                .qty = order.qty,
                .order_type = order.type,
                }));
        }

        void emit_rested(core::Timestamp event_timestamp, const Order& order) {
            push_event(Event::make(OrderRested{
                .sequence_number = next_sequence(),
                .timestamp = timestamp,
                .order_id = order.id,
                .symbol = symbol_,
                .side = order.side,
                .price = order.price,
                .qty = order.display_qty,
                }));
        }

        void emit_trade(core::Timestamp timestamp, const Order& aggressor,
            const Order& passive, core::Price price, core::Quantity qty)
        {
            const core::OrderId buy_order_id =
                aggressor.side == core::Side::BUY ? aggressor.id : passive.id;
            const core::OrderId sell_order_id =
                aggressor.side == core::Side::SELL ? aggressor.id : passive.id;

            push_event(Event::make(Trade{
                .sequence_number = next_sequence(),
                .timestamp = timestamp,
                .match_id = next_match_id_++,
                .symbol = symbol_,
                .buy_order_id = buy_order_id,
                .sell_order_id = sell_order_id,
                .price = price,
                .qty = qty,
                }));
        }

        void emit_passive_fill_state(core::Timestamp timestamp, const Order& passive,
            core::Quantity filled_qty, core::Price price) {
            if (passive.qty == 0) {
                push_event(Event::make(OrderFilled{
                    .sequence_number = next_sequence(),
                    .timestamp = timestamp,
                    .order_id = passive.id,
                    .filled_qty = filled_qty,
                    .price = price,
                    }));
                return;
            }

            push_event(Event::make(OrderPartiallyFilled{
                .sequence_number = next_sequence(),
                .timestamp = timestamp,
                .order_id = passive.id,
                .filled_qty = filled_qty,
                .remaining_qty = passive.qty,
                .price = price,
                }));
        }

        void park_stop_order(Order& order) {
            if (order.side == core::Side::BUY) {
                stop_buys_.emplace(order.trigger_price, &order);
            }
            else {
                stop_sells_.emplace(order.trigger_price, &order);
            }
        }

        [[nodiscard]] static bool is_marketable(const Order& aggressor,
            core::Price resting_price) noexcept
        {
            if (aggressor.type == core::OrderType::MARKET)
                return true;

            if (aggressor.side == core::Side::BUY)
                return aggressor.price >= resting_price;

            return aggressor.price <= resting_price;
        }

        void replenish_iceberg(Order& order, core::Timestamp event_timestamp) {
            Level* level = order.parent_level;
            if (level == nullptr)
                return;

            level->remove_order(&order);
            order.timestamp = event_timestamp;

            const core::Quantity replenished =
                std::min(order.hidden_qty, order.peak_qty);

            order.hidden_qty -= replenished;
            order.display_qty = replenished;

            // to move order to the tail of queue
            level->add_order(&order);
            emit_rested(event_timestamp, order);
        }

        void refresh_best_level(core::Side side) noexcept {
            if (side == core::Side::BUY) {
                best_bid_ = first_active_level(bids_);
            }
            else {
                best_ask_ = first_active_level(asks_);
            }
        }

        template<typename LevelMap>
        [[nodiscard]] static Level* first_active_level(const LevelMap& levels) noexcept {
            for (const auto& [price, level] : levels) {
                // to tell compiler price is of no use in this
                (void)price;
                if (level != nullptr)
                    return level;
            }

            return nullptr;
        }

        void retire_order(Order& order, core::OrderStatus terminal_status,
            bool refresh_best = true)
        {
            bool removed_best_level = false;

            if (order.parent_level != nullptr) {
                Level* level = order.parent_level;
                level->remove_order(&order);

                if (level->is_empty()) {
                    removed_best_level = order.side == core::Side::BUY
                        ? best_bid_ == level
                        : best_ask_ == level;

                    deactivate_level(order.side, level->price, level);
                }
            }
            else if (order.is_stop_order()) {
                erase_stop_reference(order);
            }

            orders_[order.id] =
                OrderRecord{ .order = nullptr, .last_status = terminal_status };

            order_pool_.deallocate(&order);

            if (refresh_best && removed_best_level) {
                refresh_best_level(order.side);
            }
        }

        void erase_stop_reference(const Order& order) {
            StopMap& stops = order.side == core::Side::BUY ? stop_buys_ : stop_sells_;

            // to check for our order in that range only
            const auto [first, last] = stops.equal_range(order.trigger_price);
            for (auto it = first; it != last; ++it) {
                if (it->second == &order) {
                    stops.erase(it);
                    return;
                }
            }
        }

        void deactivate_level(core::Side side, core::Price price,
            Level* level) noexcept
        {
            if (side == core::Side::BUY) {
                auto level_it = bids_.find(price);
                if (level_it != bids_.end()) {
                    level_it->second = nullptr;
                }
            }
            else {
                auto level_it = asks_.find(price);
                if (level_it != asks_.end()) {
                    level_it->second = nullptr;
                }
            }

            level_pool_.deallocate(level);
        }

        void execute_trade_slice(Order& aggressor, Order& passive,
            core::Quantity trade_qty,
            core::Timestamp event_timestamp,
            core::Price& last_fill_price)
        {
            const core::Price trade_price = passive.price;
            last_fill_price = trade_price;
            last_trade_price_ = trade_price;

            aggressor.qty -= trade_qty;
            passive.qty -= trade_qty;
            passive.display_qty -= trade_qty;

            passive.parent_level->total_qty -= trade_qty;

            emit_trade(event_timestamp, aggressor, passive, trade_price, trade_qty);
            emit_passive_fill_state(event_timestamp, passive, trade_qty, trade_price);

            if (passive.qty == 0) {
                retire_order(passive, core::OrderStatus::FILLED, false);
            }
            else if (passive.display_qty == 0 && passive.hidden_qty > 0) {
                replenish_iceberg(passive, event_timestamp);
            }
            else {
                passive.status = core::OrderStatus::PARTIALLY_FILLED;
                orders_[passive.id].last_status = passive.status;
            }
        }

        void match_fifo_at_level(Order& aggressor, Level& level,
            core::Timestamp event_timestamp, core::Price& last_fill_price)
        {
            while (aggressor.qty > 0 && !level.is_empty()) {
                Order* passive = level.front();
                if (passive == nullptr)
                    break;

                // for iceberg order on other side of match
                // for aggressor iceberg goes to end of queue to run again
                if (passive->display_qty == 0) {
                    if (passive->hidden_qty > 0) {
                        replenish_iceberg(*passive, event_timestamp);
                        // to again check front() as icebergs can have different prices
                        continue;
                    }
                    // ??? means the level is empty, exit from it
                    break;
                }

                const core::Quantity trade_qty =
                    std::min(aggressor.qty, passive->display_qty);

                execute_trade_slice(aggressor, *passive, trade_qty, event_timestamp,
                    last_fill_price);
            }
        }

        void trigger_stop_orders(core::Timestamp event_timestamp) {
            bool triggered_any = false;

            // to check for chain reaction that trigger other stop orders at other levels
            do {
                triggered_any = false;

                while (!stop_buys_.empty()) {
                    auto stop_it = stop_buys_.begin();
                    if (stop_it->first > last_trade_price_)
                        break;

                    Order* order = stop_it->second;
                    stop_buys_.erase(stop_it);

                    activate_stop_order(*order, event_timestamp);

                    triggered_any = true;
                }

                // stop sells trigger when price falls below trigger_price
                while (true) {
                    auto stop_it = stop_sells_.lower_bound(last_trade_price_);
                    if (stop_it == stop_sells_.end())
                        break;

                    Order* order = stop_it->second;
                    stop_sells_.erase(stop_it);

                    activate_stop_order(*order, event_timestamp);
                    triggered_any = true;
                }
            } while (triggered_any);
        }

        void activate_stop_order(Order& order, core::Timestamp event_timestamp) {
            order.timestamp = event_timestamp;
            if (order.type == core::OrderType::STOP) {
                order.type = core::OrderType::MARKET;
                order.price = 0;
            }
            else {
                order.type = core::OrderType::LIMIT;
            }

            execute_inbound_order(order, event_timestamp, false);
        }

        // templated to handle both bids_ ans asks_ map
        template<typename LevelMap>
        void match_against_levels(Order& aggressor, LevelMap& levels,
            core::Side resting_side, core::Timestamp event_timestamp,
            core::Price& last_fill_price)
        {
            // iterating levels of opposite side to match the order
            for (auto level_it = levels.begin();
                level_it != levels.end() && aggressor.qty > 0; ++level_it) {
                // each level has a FIFO queue init
                Level* level = level_it->second;
                if (level == nullptr)
                    continue;

                // check for is_marketable for other levels as well 
                // (before we just checked for best opposite level)
                if (!is_marketable(aggressor, level->price))
                    break;

                // while (aggressor.qty > 0 && !level->is_empty()) {
                match_fifo_at_level(aggressor, *level, event_timestamp,
                    last_fill_price);
                // }
            }

            refresh_best_level(resting_side);

            // stop orders are triggered if market rises to or above the level for stop_buys
            // and vice-versa for stop_sells
            if (last_fill_price != 0) {
                trigger_stop_orders(event_timestamp);
            }
        }

        void match(Order& aggressor, core::Timestamp event_timestamp,
            core::Price& last_fill_price)
        {
            const Level* best_opposite = opposite_best_level(aggressor.side);
            if (best_opposite == nullptr ||
                !is_marketable(aggressor, best_opposite->price)) {
                return;
            }

            if (aggressor.side == core::Side::BUY) {
                match_against_levels(aggressor, asks_, core::Side::SELL, event_timestamp,
                    last_fill_price);
            }
            else {
                match_against_levels(aggressor, bids_, core::Side::BUY, event_timestamp,
                    last_fill_price);
            }
        }

        void execute_inbound_order(Order& order, core::Timestamp event_timestamp,
            bool emit_accept_event)
        {
            if (emit_accept_event) {
                order.status = core::OrderStatus::ACCEPTED;
                emit_accepted(event_timestamp, order);
            }

            if (order.is_stop_order()) {
                park_stop_order(order);
                orders_[order.id].last_status = core::OrderStatus::ACCEPTED;
                return;
            }

            const core::Quantity original_qty = order.qty;
            core::Price last_fill_price = 0;

            match(order, event_timestamp, last_fill_price);
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
        core::MemoryPool<Level, LevelCapacity> level_pool_{};

        std::unique_ptr<Event[]> event_storage_;
        std::size_t event_count_{ 0 };

        core::MatchId next_match_id_{ 1 };
        core::Price last_trade_price_{ 0 };
    };
}