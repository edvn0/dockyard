#include <dockyard/igame.hpp>

namespace {

struct TestGame final : dy::IGame {
    void init   (dy::GameMemory*, dy::Scene*, dy::IAssetLoader&) override {}
    void update (dy::GameMemory*, dy::Scene*, float) override {}
    void destroy(dy::GameMemory*, dy::Scene*) override {}
};

} // namespace

GAME_EXPORT dy::IGame* create_game() {
    return new TestGame{};
}
