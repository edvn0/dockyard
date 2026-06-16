#include <dockforge/sim_state.hpp>
#include <doctest/doctest.h>

using namespace sim;

static_assert( is_valid(S::Editing, S::Playing));
static_assert( is_valid(S::Playing, S::Paused));
static_assert( is_valid(S::Paused,  S::Playing));
static_assert( is_valid(S::Playing, S::Editing));
static_assert( is_valid(S::Paused,  S::Editing));

static_assert(!is_valid(S::Editing, S::Paused));
static_assert(!is_valid(S::Editing, S::Editing));
static_assert(!is_valid(S::Playing, S::Playing));
static_assert(!is_valid(S::Paused,  S::Paused));

TEST_CASE("Given any state, When in<S> is queried, Then only the held alternative returns true") {
    SUBCASE("Editing") {
        const AnyState state = Editing{};
        CHECK( in<S::Editing>(state));
        CHECK(!in<S::Playing>(state));
        CHECK(!in<S::Paused>(state));
    }
    SUBCASE("Playing") {
        const AnyState state = Playing{};
        CHECK(!in<S::Editing>(state));
        CHECK( in<S::Playing>(state));
        CHECK(!in<S::Paused>(state));
    }
    SUBCASE("Paused") {
        const AnyState state = Paused{};
        CHECK(!in<S::Editing>(state));
        CHECK(!in<S::Playing>(state));
        CHECK( in<S::Paused>(state));
    }
}

TEST_CASE("Given Editing state, When play transition fires, Then state becomes Playing") {
    AnyState state = Editing{};
    const bool fired = try_transition<S::Playing>(state, [](auto) {});
    CHECK(fired);
    CHECK(in<S::Playing>(state));
}

TEST_CASE("Given Playing state, When pause transition fires, Then state becomes Paused") {
    AnyState state = Playing{};
    const bool fired = try_transition<S::Paused>(state, [](auto) {});
    CHECK(fired);
    CHECK(in<S::Paused>(state));
}

TEST_CASE("Given Paused state, When resume transition fires, Then state becomes Playing") {
    AnyState state = Paused{};
    const bool fired = try_transition<S::Playing>(state, [](auto) {});
    CHECK(fired);
    CHECK(in<S::Playing>(state));
}

TEST_CASE("Given Playing state, When stop transition fires, Then state becomes Editing") {
    AnyState state = Playing{};
    const bool fired = try_transition<S::Editing>(state, [](auto) {});
    CHECK(fired);
    CHECK(in<S::Editing>(state));
}

TEST_CASE("Given Paused state, When stop transition fires, Then state becomes Editing") {
    AnyState state = Paused{};
    const bool fired = try_transition<S::Editing>(state, [](auto) {});
    CHECK(fired);
    CHECK(in<S::Editing>(state));
}

TEST_CASE("Given Editing state, When pause attempted, Then state is unchanged and returns false") {
    AnyState state = Editing{};
    const bool fired = try_transition<S::Paused>(state, [](auto) {});
    CHECK(!fired);
    CHECK(in<S::Editing>(state));
}

TEST_CASE("Given Editing state, When stop attempted, Then state is unchanged and returns false") {
    AnyState state = Editing{};
    const bool fired = try_transition<S::Editing>(state, [](auto) {});
    CHECK(!fired);
    CHECK(in<S::Editing>(state));
}

TEST_CASE("Given Playing state, When play attempted, Then state is unchanged and returns false") {
    AnyState state = Playing{};
    const bool fired = try_transition<S::Playing>(state, [](auto) {});
    CHECK(!fired);
    CHECK(in<S::Playing>(state));
}

TEST_CASE("Given Paused state, When pause attempted, Then state is unchanged and returns false") {
    AnyState state = Paused{};
    const bool fired = try_transition<S::Paused>(state, [](auto) {});
    CHECK(!fired);
    CHECK(in<S::Paused>(state));
}

TEST_CASE("Given Editing state, When play fires, Then side-effect runs exactly once") {
    AnyState state = Editing{};
    int count = 0;
    try_transition<S::Playing>(state, [&](auto) { ++count; });
    CHECK(count == 1);
}

TEST_CASE("Given Playing state, When play attempted, Then side-effect does not run") {
    AnyState state = Playing{};
    int count = 0;
    try_transition<S::Playing>(state, [&](auto) { ++count; });
    CHECK(count == 0);
}

TEST_CASE("Given Editing state, When full play-pause-resume-stop cycle runs, Then ends in Editing") {
    AnyState state = Editing{};

    CHECK(try_transition<S::Playing>(state, [](auto) {}));
    CHECK(in<S::Playing>(state));

    CHECK(try_transition<S::Paused>(state, [](auto) {}));
    CHECK(in<S::Paused>(state));

    CHECK(try_transition<S::Playing>(state, [](auto) {}));
    CHECK(in<S::Playing>(state));

    CHECK(try_transition<S::Editing>(state, [](auto) {}));
    CHECK(in<S::Editing>(state));
}
