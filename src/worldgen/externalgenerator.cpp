#include "externalgenerator.h"

#include "game/gamecontext.h"
#include "network/messagebuilder.h"
#include "schemas/message_generated.h"
#include "network/connectionpoolspecialized.h"
#include "world/hashtreeworld.h"
#include "util/pool.h"

namespace worldgen {

ExternalGenerator::ExternalGenerator(game::GameContext &context)
    : context(context)
{
    assert(pointgen::Chunk::size == world::Chunk::size);
}

void ExternalGenerator::generate(spatial::CellKey cube, const pointgen::Chunk *points) {
    network::MessageBuilder::Lock lock(context);

    SsProtocol::Vec3_u32 cellCoord(cube.cellCoord.x, cube.cellCoord.y, cube.cellCoord.z);
    SsProtocol::Vec3_f cellsPositions[pointgen::Chunk::size][pointgen::Chunk::size][pointgen::Chunk::size];
    for (unsigned int i = 0; i < pointgen::Chunk::size; i++) {
        for (unsigned int j = 0; j < pointgen::Chunk::size; j++) {
            for (unsigned int k = 0; k < pointgen::Chunk::size; k++) {
                glm::vec3 pt = points->points[i][j][k];
                cellsPositions[i][j][k] = SsProtocol::Vec3_f(pt.x, pt.y, pt.z);
            }
        }
    }
    auto cellsVec = lock.getBuilder().CreateVectorOfStructs(&cellsPositions[0][0][0], pointgen::Chunk::size * pointgen::Chunk::size * pointgen::Chunk::size);
    auto chunkCommand = SsProtocol::CreateChunk(lock.getBuilder(), cube.sizeLog2, &cellCoord, cellsVec);
    auto message = SsProtocol::CreateMessage(lock.getBuilder(), SsProtocol::MessageUnion_Chunk, chunkCommand.Union());
    lock.getBuilder().Finish(message);

    context.get<network::ConnectionPoolSpecialized<SsProtocol::Capabilities_GenerateChunk>>().send(lock.getBuilder().GetBufferPointer(), lock.getBuilder().GetSize());
}

void ExternalGenerator::handleResponse(const SsProtocol::Chunk *chunk) {
    spatial::CellKey cube;
    cube.sizeLog2 = chunk->cell_size_log2();
    cube.cellCoord.x = chunk->cell_coord()->x();
    cube.cellCoord.y = chunk->cell_coord()->y();
    cube.cellCoord.z = chunk->cell_coord()->z();

    world::Chunk *dstChunk = context.get<util::Pool<world::Chunk>>().alloc();

    const std::uint32_t *ptPtr = chunk->cell_types()->data();
    for (unsigned int i = 0; i < pointgen::Chunk::size; i++) {
        for (unsigned int j = 0; j < pointgen::Chunk::size; j++) {
            for (unsigned int k = 0; k < pointgen::Chunk::size; k++) {
                dstChunk->cells[i][j][k].type.value = static_cast<world::SpaceState::Value>(*ptPtr++);
            }
        }
    }

    context.get<world::HashTreeWorld>().finishWorldGen(cube, world::SpaceState::SubdividedAsChunk, dstChunk);
}

}