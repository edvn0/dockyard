#pragma once

namespace dy {
class Scene;
struct GameMemory;

struct IGame {
    virtual void init   (GameMemory*, Scene*) = 0;
    virtual void update (GameMemory*, Scene*, float ts) = 0;
    virtual void destroy(GameMemory*, Scene*) = 0;
    virtual ~IGame() = default;
};

using GameFactory = IGame*(*)();

} // namespace dy

#ifdef _WIN32
    #define GAME_EXPORT extern "C" __declspec(dllexport)
#else
    #define GAME_EXPORT extern "C" __attribute__((visibility("default")))
#endif
