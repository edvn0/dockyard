#include <dockforge/sim_state.hpp>
#include <doctest/doctest.h>

using namespace sim;

static_assert( fsm::is_valid(S::Editing, S::Playing));
static_assert( fsm::is_valid(S::Playing, S::Paused));
static_assert( fsm::is_valid(S::Paused,  S::Playing));
static_assert( fsm::is_valid(S::Playing, S::Editing));
static_assert( fsm::is_valid(S::Paused,  S::Editing));

static_assert(!fsm::is_valid(S::Editing, S::Paused));
static_assert(!fsm::is_valid(S::Editing, S::Editing));
static_assert(!fsm::is_valid(S::Playing, S::Playing));
static_assert(!fsm::is_valid(S::Paused,  S::Paused));

TEST_CASE("Given any state, When in<S> is queried, Then only the held alternative returns true") {
    SUBCASE("Editing") {
        const Machine state{Editing{}};
        CHECK( state.in<S::Editing>());
        CHECK(!state.in<S::Playing>());
        CHECK(!state.in<S::Paused>());
    }
    SUBCASE("Playing") {
        const Machine state{Playing{}};
        CHECK(!state.in<S::Editing>());
        CHECK( state.in<S::Playing>());
        CHECK(!state.in<S::Paused>());
    }
    SUBCASE("Paused") {
        const Machine state{Paused{}};
        CHECK(!state.in<S::Editing>());
        CHECK(!state.in<S::Playing>());
        CHECK( state.in<S::Paused>());
    }
}

TEST_CASE("Given Editing state, When play transition fires, Then state becomes Playing") {
    Machine state{Editing{}};
    const bool fired = state.try_transition<S::Playing>([](auto) {});
    CHECK(fired);
    CHECK(state.in<S::Playing>());
}

TEST_CASE("Given Playing state, When pause transition fires, Then state becomes Paused") {
    Machine state{Playing{}};
    const bool fired = state.try_transition<S::Paused>([](auto) {});
    CHECK(fired);
    CHECK(state.in<S::Paused>());
}

TEST_CASE("Given Paused state, When resume transition fires, Then state becomes Playing") {
    Machine state{Paused{}};
    const bool fired = state.try_transition<S::Playing>([](auto) {});
    CHECK(fired);
    CHECK(state.in<S::Playing>());
}

TEST_CASE("Given Playing state, When stop transition fires, Then state becomes Editing") {
    Machine state{Playing{}};
    const bool fired = state.try_transition<S::Editing>([](auto) {});
    CHECK(fired);
    CHECK(state.in<S::Editing>());
}

TEST_CASE("Given Paused state, When stop transition fires, Then state becomes Editing") {
    Machine state{Paused{}};
    const bool fired = state.try_transition<S::Editing>([](auto) {});
    CHECK(fired);
    CHECK(state.in<S::Editing>());
}

TEST_CASE("Given Editing state, When pause attempted, Then state is unchanged and returns false") {
    Machine state{Editing{}};
    const bool fired = state.try_transition<S::Paused>([](auto) {});
    CHECK(!fired);
    CHECK(state.in<S::Editing>());
}

TEST_CASE("Given Editing state, When stop attempted, Then state is unchanged and returns false") {
    Machine state{Editing{}};
    const bool fired = state.try_transition<S::Editing>([](auto) {});
    CHECK(!fired);
    CHECK(state.in<S::Editing>());
}

TEST_CASE("Given Playing state, When play attempted, Then state is unchanged and returns false") {
    Machine state{Playing{}};
    const bool fired = state.try_transition<S::Playing>([](auto) {});
    CHECK(!fired);
    CHECK(state.in<S::Playing>());
}

TEST_CASE("Given Paused state, When pause attempted, Then state is unchanged and returns false") {
    Machine state{Paused{}};
    const bool fired = state.try_transition<S::Paused>([](auto) {});
    CHECK(!fired);
    CHECK(state.in<S::Paused>());
}

TEST_CASE("Given Editing state, When play fires, Then side-effect runs exactly once") {
    Machine state{Editing{}};
    int count = 0;
    state.try_transition<S::Playing>([&](auto) { ++count; });
    CHECK(count == 1);
}

TEST_CASE("Given Playing state, When play attempted, Then side-effect does not run") {
    Machine state{Playing{}};
    int count = 0;
    state.try_transition<S::Playing>([&](auto) { ++count; });
    CHECK(count == 0);
}

TEST_CASE("Given Editing state, When full play-pause-resume-stop cycle runs, Then ends in Editing") {
    Machine state{Editing{}};

    CHECK(state.try_transition<S::Playing>([](auto) {}));
    CHECK(state.in<S::Playing>());

    CHECK(state.try_transition<S::Paused>([](auto) {}));
    CHECK(state.in<S::Paused>());

    CHECK(state.try_transition<S::Playing>([](auto) {}));
    CHECK(state.in<S::Playing>());

    CHECK(state.try_transition<S::Editing>([](auto) {}));
    CHECK(state.in<S::Editing>());
}
