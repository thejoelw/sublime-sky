#pragma once

#include "defs/VIEW_CHUNK_SUBDIV_OFFSET_LOG2.h"

#include "jw_util/bitset.h"

#include "spatial/hashtree.h"
#include "game/tickercontext.h"
#include "world/chunk.h"
#include "worldgen/worldgenerator.h"

namespace pointgen { class Chunk; }

namespace world {

class CellValue {
public:
    void initialize() {
        chunkId = nextChunkId;
        nextChunkId += Chunk::size * Chunk::size * Chunk::size;

        if (nextChunkId < Chunk::size * Chunk::size * Chunk::size) {
            // TODO: Log
            assert(false);
        }

        constantMaterialIndex = MaterialIndex::Null;
        chunk = 0;
        points = 0;

        hasFaces.fill<false>();
    }

    bool isLeaf() const {
        return isViewLeaf;
    }

    static unsigned int nextChunkId;
    unsigned int chunkId;

    // Constant material significances:
    // 0: Null - The chunk has not initiated generation
    // 1: Generating - The chunk has initiated generation

    MaterialIndex constantMaterialIndex;
    bool isViewLeaf;
    Chunk *chunk;
    pointgen::Chunk *points;
    std::vector<unsigned int> faceIndices;
    jw_util::Bitset<Chunk::size * Chunk::size * Chunk::size> hasFaces;
    jw_util::Bitset<Chunk::size * Chunk::size * Chunk::size> gasMasks[3];
};

class HashTreeWorld : public spatial::HashTree<HashTreeWorld, CellValue>, public game::TickerContext::TickableBase<HashTreeWorld> {
    friend class spatial::HashTree<HashTreeWorld, CellValue>;

public:
    template <bool populateState, bool populatePoints>
    class CellIterator {
    public:
        void init(spatial::UintCoord coord) {
        }

        CellValue &getNearby(spatial::UintCoord coord) {
            unsigned int cellIndex = coord.x % Chunk::size;

            coord.x /= Chunk::size;
            coord.y /= Chunk::size;
            coord.z /= Chunk::size;

            coord -= min;
            assert(coord.x < (static_cast<spatial::UintCoord::AxisType>(1) << sizeLog2_x));
            assert(coord.y < (static_cast<spatial::UintCoord::AxisType>(1) << sizeLog2_y));
            assert(coord.z < (static_cast<spatial::UintCoord::AxisType>(1) << sizeLog2_z));

            unsigned int index = (((coord.x << sizeLog2_y) | coord.y) << sizeLog2_z) | coord.z;

            if (!data[index]) {
                fillChunkRef(coord , data[index]);
            }

            return *data[index];
        }

    private:
        HashTreeWorld &hashTreeWorld;

        spatial::UintCoord min;
        CellValue **data;
#ifndef NDEBUG
        unsigned int sizeLog2_x;
#endif
        unsigned int sizeLog2_y;
        unsigned int sizeLog2_z;

        void fillChunkRef(spatial::UintCoord chunkCoord, CellValue *&cellValue) {
            spatial::CellKey cellKey(min + chunkCoord, Chunk::sizeLog2);
            cellValue = hashTreeWorld.findNodeMatching(cellKey);

            if (populateState) {

            }
        }
    };

    HashTreeWorld(game::GameContext &gameContext);

    void tick(game::TickerContext &tickerContext);

//    Cell &lookupChunk(spatial::CellKey cellKey);

    void updateGasMasks(CellValue *cellValue);

    /*
    CellValue &getCellValueContaining(spatial::UintCoord coord);
    MaterialIndex getMaterialIndex(spatial::UintCoord coord);
    VoronoiCell &getVoronoiCell(spatial::UintCoord coord);
    glm::vec3 getPoint(spatial::UintCoord coord);
    bool &getNeedsRegen(spatial::UintCoord coord);
    unsigned int getCellId(spatial::UintCoord coord);
    */
    struct RaytestResult {
        enum Result {
            HitSurface,
            HitGenerating,
            HitDistanceLimit,
        };
        Result result;

        // These properties are only guaranteed to be set if result == HitSurface:
        MaterialIndex surfaceMaterialIndex;
        spatial::CellKey surfaceHitCell;
//        float surfacePointDistance;
    };
    RaytestResult testViewRay(glm::vec3 origin, glm::vec3 dir, float distanceLimit);
//    RaytestResult testRay(glm::vec3 origin, glm::vec3 dir, float distanceLimit);
//    RaytestResult testRaySlow(glm::vec3 origin, glm::vec3 dir, float distanceLimit);

//    spatial::UintCoord getContainingCoord(glm::vec3 point);

//    void emitMeshUpdate(glm::vec3 changedMin, glm::vec3 changedMax, float pointSpacing);

    const pointgen::Chunk *getChunkPoints(Cell *cell);

    bool isGas(MaterialIndex materialIndex) const;

    void generateChunk(Cell *cell);

    bool shouldSubdivForPhysics(Cell *cell) const {
        return cell->first.sizeLog2 > Chunk::sizeLog2;
    }
    bool shouldSubdivForView(spatial::CellKey cellKey) {
        if (cellKey.sizeLog2 <= Chunk::sizeLog2) {
            return false;
        }

        signed int desiredSizeLog2 = guessViewChunkSizeLog2(cellKey.child<1, 1, 1>().getCoord<0, 0, 0>());
        return static_cast<signed int>(cellKey.sizeLog2) > desiredSizeLog2;
    }

private:
    spatial::UintCoord cameraCoord;
    spatial::UintCoord calcCameraCoord();

    static CellValue makeRootBranch() {
        CellValue res;
        res.initialize();
        return res;
    }

    signed int guessViewChunkSizeLog2(spatial::UintCoord coord) const {
        spatial::UintCoord::SignedAxisType dx = coord.x - cameraCoord.x;
        spatial::UintCoord::SignedAxisType dy = coord.y - cameraCoord.y;
        spatial::UintCoord::SignedAxisType dz = coord.z - cameraCoord.z;
        std::uint64_t dSqX = static_cast<std::int64_t>(dx) * static_cast<std::int64_t>(dx);
        std::uint64_t dSqY = static_cast<std::int64_t>(dy) * static_cast<std::int64_t>(dy);
        std::uint64_t dSqZ = static_cast<std::int64_t>(dz) * static_cast<std::int64_t>(dz);
        std::uint64_t dSq = dSqX + dSqY + dSqZ;

        if (!dSq) {
            return VIEW_CHUNK_SUBDIV_OFFSET_LOG2;
        }

        unsigned int numSigBits = sizeof(dSq) * CHAR_BIT - __builtin_clzll(dSq);
        signed int desiredSizeLog2 = numSigBits / 2 + VIEW_CHUNK_SUBDIV_OFFSET_LOG2;
        return desiredSizeLog2;
    }
};

}
