#include <dockyard/state_machine.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <variant>

// ─── Traffic-light machine ────────────────────────────────────────────────────

enum class Light : uint8_t { Red, Green, Yellow };

namespace fsm {
template <> struct machine_traits<Light> {
    using edge_t = Edge<Light::Red>;
    static constexpr std::array<edge_t, 3> transitions = {{
        {.from = Light::Red,    .to = Light::Green},
        {.from = Light::Green,  .to = Light::Yellow},
        {.from = Light::Yellow, .to = Light::Red},
    }};
};
} // namespace fsm

using LightState   = std::variant<fsm::State<Light::Red>,
                                  fsm::State<Light::Green>,
                                  fsm::State<Light::Yellow>>;
using LightMachine = fsm::Machine<Light, LightState>;

static_assert( fsm::is_valid(Light::Red,    Light::Green));
static_assert( fsm::is_valid(Light::Green,  Light::Yellow));
static_assert( fsm::is_valid(Light::Yellow, Light::Red));
static_assert(!fsm::is_valid(Light::Red,    Light::Yellow));
static_assert(!fsm::is_valid(Light::Green,  Light::Red));
static_assert(!fsm::is_valid(Light::Yellow, Light::Green));
static_assert(!fsm::is_valid(Light::Red,    Light::Red));

TEST_CASE("Given Red state, When in<> queried, Then only Red is true") {
    const LightMachine machine{fsm::State<Light::Red>{}};
    CHECK( machine.in<Light::Red>());
    CHECK(!machine.in<Light::Green>());
    CHECK(!machine.in<Light::Yellow>());
}

TEST_CASE("Given Red state, When advance to Green, Then state becomes Green") {
    LightMachine machine{fsm::State<Light::Red>{}};
    const bool fired = machine.try_transition<Light::Green>([](auto) {});
    CHECK(fired);
    CHECK(machine.in<Light::Green>());
}

TEST_CASE("Given Green state, When advance to Yellow, Then state becomes Yellow") {
    LightMachine machine{fsm::State<Light::Green>{}};
    const bool fired = machine.try_transition<Light::Yellow>([](auto) {});
    CHECK(fired);
    CHECK(machine.in<Light::Yellow>());
}

TEST_CASE("Given Yellow state, When advance to Red, Then state becomes Red") {
    LightMachine machine{fsm::State<Light::Yellow>{}};
    const bool fired = machine.try_transition<Light::Red>([](auto) {});
    CHECK(fired);
    CHECK(machine.in<Light::Red>());
}

TEST_CASE("Given Red state, When skipping to Yellow attempted, Then rejected and state unchanged") {
    LightMachine machine{fsm::State<Light::Red>{}};
    const bool fired = machine.try_transition<Light::Yellow>([](auto) {});
    CHECK(!fired);
    CHECK(machine.in<Light::Red>());
}

TEST_CASE("Given Light machine, When full cycle completes, Then returns to Red") {
    LightMachine machine{fsm::State<Light::Red>{}};
    CHECK(machine.try_transition<Light::Green>([](auto) {}));
    CHECK(machine.try_transition<Light::Yellow>([](auto) {}));
    CHECK(machine.try_transition<Light::Red>([](auto) {}));
    CHECK(machine.in<Light::Red>());
}

TEST_CASE("Given transition fires, When action receives state, Then action observes correct prior state") {
    LightMachine machine{fsm::State<Light::Red>{}};
    bool saw_red = false;
    machine.try_transition<Light::Green>([&](auto prior) {
        saw_red = (decltype(prior)::value == Light::Red);
    });
    CHECK(saw_red);
}

// ─── Door-lock machine ────────────────────────────────────────────────────────

enum class Door : uint8_t { Locked, Closed, Open };

namespace fsm {
template <> struct machine_traits<Door> {
    using edge_t = Edge<Door::Locked>;
    static constexpr std::array<edge_t, 4> transitions = {{
        {.from = Door::Locked, .to = Door::Closed},
        {.from = Door::Closed, .to = Door::Locked},
        {.from = Door::Closed, .to = Door::Open},
        {.from = Door::Open,   .to = Door::Closed},
    }};
};
} // namespace fsm

using DoorState   = std::variant<fsm::State<Door::Locked>,
                                 fsm::State<Door::Closed>,
                                 fsm::State<Door::Open>>;
using DoorMachine = fsm::Machine<Door, DoorState>;

static_assert( fsm::is_valid(Door::Locked, Door::Closed));
static_assert( fsm::is_valid(Door::Closed, Door::Locked));
static_assert( fsm::is_valid(Door::Closed, Door::Open));
static_assert( fsm::is_valid(Door::Open,   Door::Closed));
static_assert(!fsm::is_valid(Door::Locked, Door::Open));
static_assert(!fsm::is_valid(Door::Open,   Door::Locked));

TEST_CASE("Given Locked door, When unlock, Then state is Closed") {
    DoorMachine machine{fsm::State<Door::Locked>{}};
    CHECK(machine.try_transition<Door::Closed>([](auto) {}));
    CHECK(machine.in<Door::Closed>());
}

TEST_CASE("Given Locked door, When open attempted directly, Then rejected") {
    DoorMachine machine{fsm::State<Door::Locked>{}};
    const bool fired = machine.try_transition<Door::Open>([](auto) {});
    CHECK(!fired);
    CHECK(machine.in<Door::Locked>());
}

TEST_CASE("Given Closed door, When lock-unlock-open sequence, Then ends Open") {
    DoorMachine machine{fsm::State<Door::Closed>{}};
    CHECK(machine.try_transition<Door::Locked>([](auto) {}));
    CHECK(machine.try_transition<Door::Closed>([](auto) {}));
    CHECK(machine.try_transition<Door::Open>([](auto) {}));
    CHECK(machine.in<Door::Open>());
}
