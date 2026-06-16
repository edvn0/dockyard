#pragma once
#include <variant>

namespace fsm {

// ─── core types ──────────────────────────────────────────────────────────────

template <auto V>
struct State {
    static constexpr auto value = V;
    using enum_type = decltype(V);
};

template <auto V>
struct Edge { decltype(V) from; decltype(V) to; };

// ─── machine trait — specialise this ─────────────────────────────────────────
//
//  template <> struct machine_traits<MyEnum> {
//      static constexpr auto transitions = std::array{ ... };
//  };

template <typename Enum>
struct machine_traits; // undefined by default — linker error if missing

// ─── validation ──────────────────────────────────────────────────────────────

template <typename Enum>
consteval auto is_valid(Enum from, Enum to_state) -> bool {
    for (const auto& edge : machine_traits<Enum>::transitions)
        if (edge.from == from && edge.to == to_state) return true;
    return false;
}

// ─── typed state machine wrapper ─────────────────────────────────────────────

template <typename Enum, typename Variant>
class Machine {
public:
    using enum_type    = Enum;
    using variant_type = Variant;

    Machine() = default;
    explicit Machine(Variant initial) : state(std::move(initial)) {}

    // checked compile-time transition — only compiles if edge exists
    template <Enum To, Enum From>
        requires (is_valid(From, To))
    [[nodiscard]] constexpr auto transition(State<From> /*from*/) noexcept -> State<To> {
        state = State<To>{};
        return {};
    }

    // runtime transition attempt — returns false if no edge
    template <Enum To, typename Fn>
    auto try_transition(Fn&& action) -> bool {
        return std::visit([&](auto current) -> bool {
            constexpr Enum from = decltype(current)::value;
            if constexpr (is_valid(from, To)) {
                std::forward<Fn>(action)(current);
                state = State<To>{};
                return true;
            } else {
                return false;
            }
        }, state);
    }

    template <Enum V>
    [[nodiscard]] constexpr auto in() const noexcept -> bool {
        return std::holds_alternative<State<V>>(state);
    }

    [[nodiscard]] auto raw() const noexcept -> const Variant& { return state; }

private:
    Variant state;
};

} // namespace fsm
