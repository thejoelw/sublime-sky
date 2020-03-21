#pragma once

#include "worldgen/worldgenerator.h"
#include "worldgen/simplexnoise.h"

namespace game { class GameContext; }

namespace worldgen {

class SimpleGenerator : public WorldGenerator {
public:
    SimpleGenerator(game::GameContext &context);

    void generate(Request *request);

private:
    game::GameContext &context;
    SimplexNoise noise;

    world::SpaceState getState(glm::vec3 point);
};

}
