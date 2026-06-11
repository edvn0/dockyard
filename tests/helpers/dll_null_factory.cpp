#include <dockyard/igame.hpp>

// Intentionally returns nullptr from create_game.
// Used to test that GameDll::load / force_reload handle a null factory result.
GAME_EXPORT dy::IGame* create_game() {
    return nullptr;
}
