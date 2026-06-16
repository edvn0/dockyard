#pragma once
#include <array>
#include <cstdint>
#include <variant>

namespace sim {

enum class S : uint8_t { Editing, Playing, Paused };

template <S V>
struct State {
    static constexpr S value = V;
};

using Editing  = State<S::Editing>;
using Playing  = State<S::Playing>;
using Paused   = State<S::Paused>;
using AnyState = std::variant<Editing, Playing, Paused>;

struct Edge { S from; S to; };

static constexpr std::array state_transitions = {
    Edge{.from = S::Editing, .to = S::Playing},  // play
    Edge{.from = S::Playing, .to = S::Paused},   // pause
    Edge{.from = S::Paused,  .to = S::Playing},  // resume
    Edge{.from = S::Playing, .to = S::Editing},  // stop
    Edge{.from = S::Paused,  .to = S::Editing},  // stop
};

consteval auto is_valid(S from_state, S to_state) -> bool {
    for (const auto &edge : state_transitions)
        if (edge.from == from_state && edge.to == to_state)
            return true;
    return false;
}

template <S To, S From>
    requires (is_valid(From, To))
[[nodiscard]] constexpr auto transition(State<From> /*from*/) noexcept -> State<To> {
    return {};
}

template <S To, typename Fn>
auto try_transition(AnyState &state, Fn &&action) -> bool {
    return std::visit([&](auto current) -> bool {
        constexpr S from_state = decltype(current)::value;
        if constexpr (is_valid(from_state, To)) {
            std::forward<Fn>(action)(current);
            state = State<To>{};
            return true;
        }
        return false;
    }, state);
}

template <S V>
[[nodiscard]] constexpr auto in(const AnyState &state) noexcept -> bool {
    return std::holds_alternative<State<V>>(state);
}

} // namespace sim
