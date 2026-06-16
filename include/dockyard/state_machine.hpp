#pragma once
#include <ranges>
#include <variant>

namespace fsm {

template <auto V> struct State {
  static constexpr auto value = V;
  using enum_type = decltype(V);
};

template <auto V> struct Edge {
  decltype(V) from;
  decltype(V) to;
};

template <typename Enum> struct machine_traits;

// Satisfied when machine_traits<Enum> is specialised with a constexpr
// iterable `transitions` member.
template <typename Enum>
concept EnumWithTransitions = requires {
  { machine_traits<Enum>::transitions } -> std::ranges::range;
};

template <EnumWithTransitions Enum>
consteval auto is_valid(Enum from, Enum to_state) -> bool {
  for (const auto &edge : machine_traits<Enum>::transitions)
    if (edge.from == from && edge.to == to_state)
      return true;
  return false;
}

template <typename Enum, typename Variant>
  requires EnumWithTransitions<Enum>
class Machine {
public:
  using enum_type = Enum;
  using variant_type = Variant;

  Machine() = default;
  explicit Machine(Variant initial) : state(std::move(initial)) {}

  template <Enum To, Enum From>
    requires(is_valid(From, To))
  [[nodiscard]] constexpr auto transition(State<From> /*from*/) noexcept
      -> State<To> {
    state = State<To>{};
    return {};
  }

  template <Enum To, typename Fn> auto try_transition(Fn &&action) -> bool {
    return std::visit(
        [&](auto current) -> bool {
          constexpr Enum from = decltype(current)::value;
          if constexpr (is_valid(from, To)) {
            std::forward<Fn>(action)(current);
            state = State<To>{};
            return true;
          } else {
            return false;
          }
        },
        state);
  }

  template <Enum V> [[nodiscard]] constexpr auto in() const noexcept -> bool {
    return std::holds_alternative<State<V>>(state);
  }

  [[nodiscard]] auto raw() const noexcept -> const Variant & { return state; }

private:
  Variant state;
};

} // namespace fsm
