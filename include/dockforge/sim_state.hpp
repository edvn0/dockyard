#pragma once
#include <array>
#include <cstdint>
#include <variant>
#include <dockyard/state_machine.hpp>

namespace sim {

enum class S : uint8_t { Editing, Playing, Paused };

using Editing  = fsm::State<S::Editing>;
using Playing  = fsm::State<S::Playing>;
using Paused   = fsm::State<S::Paused>;
using AnyState = std::variant<Editing, Playing, Paused>;
using Machine  = fsm::Machine<S, AnyState>;

} // namespace sim

namespace fsm {
template <> struct machine_traits<sim::S> {
    using edge_t = Edge<sim::S::Editing>;
    static constexpr std::array<edge_t, 5> transitions = {{
        {.from = sim::S::Editing, .to = sim::S::Playing},
        {.from = sim::S::Playing, .to = sim::S::Paused},
        {.from = sim::S::Paused,  .to = sim::S::Playing},
        {.from = sim::S::Playing, .to = sim::S::Editing},
        {.from = sim::S::Paused,  .to = sim::S::Editing},
    }};
};
} // namespace fsm
