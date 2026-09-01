#include "AircraftHlod.h"

#include "Log.h"
#include "Symbols.h"

#include <sys/system_properties.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace savr::airhlod {
namespace {

constexpr char kPackPath[] =
    "/sdcard/Android/data/com.rockstargames.gtasa/files/savr_aircraft.hlod";
constexpr char kEnableProperty[] = "debug.savr.air_hlod";
constexpr float kMinimumRadiusM = 1200.0f;
constexpr float kMaximumRadiusM = 1800.0f;
constexpr std::uint32_t kMaximumTiles = 128;
constexpr std::uint32_t kMaximumVerticesPerTile = 65535;
constexpr std::uint32_t kMaximumIndicesPerTile = 600000;
// RwIm3DTransform's public limit is 65536 vertices, but the mobile pipeline
// behind it uses much smaller transient buffers.  The 4096-vertex batches
// still corrupted the active Im3D pipeline pointer on this retail ARM64 build
// (the subsequent RxPipelineExecute dereferenced packed vertex colours as a
// pointer).  Keep each physical command close to the sizes used by retail's
// own immediate-mode effects; the offline pack is aggressively clustered so
// this does not explode the draw count.
constexpr std::uint32_t kMaximumVerticesPerBatch = 1024;
constexpr std::uint32_t kMaximumIndicesPerBatch = 3072;

struct Im3DVertex {
    float position[3]{};
    float normal[3]{};
    std::uint32_t color{};
    float u{};
    float v{};
};
static_assert(sizeof(Im3DVertex) == 36);

struct Batch {
    std::vector<Im3DVertex> vertices;
    std::vector<unsigned short> indices;
};

struct Tile {
    std::int32_t x{};
    std::int32_t y{};
    float minimum[3]{};
    float maximum[3]{};
    std::vector<Batch> batches;
};

struct State {
    bool attempted{};
    bool loaded{};
    bool enabled{true};
    float tileSize{};
    std::vector<Tile> tiles;
    std::uint64_t eyePasses{};
    std::uint64_t selectedTiles{};
    std::uint64_t drawCalls{};
    std::uint64_t drawFailures{};
};

State g_state;

extern "C" const unsigned char savr_aircraft_hlod_data[];
extern "C" const unsigned char savr_aircraft_hlod_data_end[];

template <typename T>
bool Read(FILE* file, T* value) {
    return file && value && std::fread(value, sizeof(T), 1, file) == 1;
}

bool ReadBytes(FILE* file, void* value, std::size_t bytes) {
    return bytes == 0 || (file && value && std::fread(value, 1, bytes, file) == bytes);
}

bool PropertyEnabled() {
    char text[PROP_VALUE_MAX]{};
    if (__system_property_get(kEnableProperty, text) <= 0) return true;
    return std::strcmp(text, "0") != 0 && std::strcmp(text, "false") != 0 &&
        std::strcmp(text, "off") != 0;
}

bool FiniteBounds(const Tile& tile) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(tile.minimum[i]) || !std::isfinite(tile.maximum[i]) ||
            tile.minimum[i] > tile.maximum[i] ||
            std::abs(tile.minimum[i]) > 10000.0f ||
            std::abs(tile.maximum[i]) > 10000.0f) {
            return false;
        }
    }
    return true;
}

bool BuildBatches(std::vector<Im3DVertex>&& sourceVertices,
                  const std::vector<unsigned short>& sourceIndices,
                  Tile* tile) {
    if (!tile || sourceIndices.empty() || sourceIndices.size() % 3 != 0)
        return false;

    std::vector<std::int32_t> remap(sourceVertices.size(), -1);
    std::vector<unsigned short> touched;
    touched.reserve(kMaximumVerticesPerBatch);
    Batch batch{};
    batch.vertices.reserve(kMaximumVerticesPerBatch);
    batch.indices.reserve(kMaximumIndicesPerBatch);

    const auto finishBatch = [&]() {
        if (batch.indices.empty()) return;
        tile->batches.push_back(std::move(batch));
        batch = Batch{};
        batch.vertices.reserve(kMaximumVerticesPerBatch);
        batch.indices.reserve(kMaximumIndicesPerBatch);
        for (const unsigned short oldIndex : touched) remap[oldIndex] = -1;
        touched.clear();
    };

    for (std::size_t triangle = 0; triangle < sourceIndices.size();
         triangle += 3) {
        std::uint32_t additionalVertices = 0;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const unsigned short oldIndex = sourceIndices[triangle + corner];
            if (oldIndex >= sourceVertices.size()) return false;
            bool firstInTriangle = true;
            for (std::size_t previous = 0; previous < corner; ++previous) {
                if (sourceIndices[triangle + previous] == oldIndex) {
                    firstInTriangle = false;
                    break;
                }
            }
            if (firstInTriangle && remap[oldIndex] < 0) ++additionalVertices;
        }
        if (!batch.indices.empty() &&
            (batch.indices.size() + 3 > kMaximumIndicesPerBatch ||
             batch.vertices.size() + additionalVertices >
                 kMaximumVerticesPerBatch)) {
            finishBatch();
        }
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const unsigned short oldIndex = sourceIndices[triangle + corner];
            std::int32_t& newIndex = remap[oldIndex];
            if (newIndex < 0) {
                if (batch.vertices.size() >= kMaximumVerticesPerBatch)
                    return false;
                newIndex = static_cast<std::int32_t>(batch.vertices.size());
                batch.vertices.push_back(sourceVertices[oldIndex]);
                touched.push_back(oldIndex);
            }
            batch.indices.push_back(static_cast<unsigned short>(newIndex));
        }
    }
    finishBatch();
    return !tile->batches.empty();
}

bool LoadPack() {
    if (g_state.attempted) return g_state.loaded;
    g_state.attempted = true;
    g_state.enabled = PropertyEnabled();
    if (!g_state.enabled) {
        LOGI("[hlod.air] disabled by %s", kEnableProperty);
        return false;
    }
    const std::size_t embeddedSize = static_cast<std::size_t>(
        savr_aircraft_hlod_data_end - savr_aircraft_hlod_data);
    FILE* file = embeddedSize > 1
        ? fmemopen(const_cast<unsigned char*>(savr_aircraft_hlod_data),
                   embeddedSize, "rb")
        : nullptr;
    const bool embedded = file != nullptr;
    if (!file) file = std::fopen(kPackPath, "rb");
    if (!file) {
        LOGW("[hlod.air] pack missing path=%s", kPackPath);
        return false;
    }
    std::array<char, 8> magic{};
    std::uint32_t version = 0;
    std::uint32_t tileCount = 0;
    if (!ReadBytes(file, magic.data(), magic.size()) ||
        !Read(file, &version) || !Read(file, &g_state.tileSize) ||
        !Read(file, &tileCount) ||
        std::memcmp(magic.data(), "SAVRHLD2", 8) != 0 || version != 2 ||
        !std::isfinite(g_state.tileSize) || g_state.tileSize < 128.0f ||
        g_state.tileSize > 2048.0f || tileCount == 0 ||
        tileCount > kMaximumTiles) {
        LOGE("[hlod.air] invalid header version=%u tiles=%u size=%.1f",
             version, tileCount, static_cast<double>(g_state.tileSize));
        std::fclose(file);
        return false;
    }

    std::uint64_t totalVertices = 0;
    std::uint64_t totalTriangles = 0;
    g_state.tiles.reserve(tileCount);
    for (std::uint32_t tileIndex = 0; tileIndex < tileCount; ++tileIndex) {
        Tile tile{};
        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;
        if (!Read(file, &tile.x) || !Read(file, &tile.y) ||
            !ReadBytes(file, tile.minimum, sizeof(tile.minimum)) ||
            !ReadBytes(file, tile.maximum, sizeof(tile.maximum)) ||
            !Read(file, &vertexCount) || !Read(file, &indexCount) ||
            !FiniteBounds(tile) || vertexCount == 0 ||
            vertexCount > kMaximumVerticesPerTile || indexCount == 0 ||
            indexCount > kMaximumIndicesPerTile || indexCount % 3 != 0) {
            LOGE("[hlod.air] invalid tile=%u v=%u i=%u", tileIndex,
                 vertexCount, indexCount);
            std::fclose(file);
            g_state.tiles.clear();
            return false;
        }
        std::vector<Im3DVertex> vertices(vertexCount);
        for (std::uint32_t vertexIndex = 0; vertexIndex < vertexCount;
             ++vertexIndex) {
            float position[3]{};
            std::array<std::uint8_t, 4> rgba{};
            if (!ReadBytes(file, position, sizeof(position)) ||
                !ReadBytes(file, rgba.data(), rgba.size()) ||
                !std::isfinite(position[0]) || !std::isfinite(position[1]) ||
                !std::isfinite(position[2])) {
                LOGE("[hlod.air] invalid vertex tile=%u vertex=%u",
                     tileIndex, vertexIndex);
                std::fclose(file);
                g_state.tiles.clear();
                return false;
            }
            Im3DVertex& out = vertices[vertexIndex];
            std::memcpy(out.position, position, sizeof(position));
            out.color = 0xff000000u |
                (static_cast<std::uint32_t>(rgba[2]) << 16u) |
                (static_cast<std::uint32_t>(rgba[1]) << 8u) |
                static_cast<std::uint32_t>(rgba[0]);
        }
        std::vector<unsigned short> indices(indexCount);
        if (!ReadBytes(file, indices.data(),
                       indexCount * sizeof(unsigned short)) ||
            std::any_of(indices.begin(), indices.end(),
                        [vertexCount](unsigned short index) {
                            return index >= vertexCount;
                        }) ||
            !BuildBatches(std::move(vertices), indices, &tile)) {
            LOGE("[hlod.air] invalid index data tile=%u", tileIndex);
            std::fclose(file);
            g_state.tiles.clear();
            return false;
        }
        totalVertices += vertexCount;
        totalTriangles += indexCount / 3u;
        g_state.tiles.push_back(std::move(tile));
    }
    const int tail = std::fgetc(file);
    std::fclose(file);
    if (tail != EOF) {
        LOGE("[hlod.air] unexpected trailing data");
        g_state.tiles.clear();
        return false;
    }
    g_state.loaded = true;
    std::uint64_t totalBatches = 0;
    for (const Tile& tile : g_state.tiles) totalBatches += tile.batches.size();
    LOGI("[hlod.air] loaded source=%s bytes=%zu tiles=%zu batches=%llu v=%llu t=%llu tile=%.0fm",
         embedded ? "embedded" : "external", embeddedSize,
         g_state.tiles.size(),
         static_cast<unsigned long long>(totalBatches),
         static_cast<unsigned long long>(totalVertices),
         static_cast<unsigned long long>(totalTriangles),
         static_cast<double>(g_state.tileSize));
    return true;
}

float DistanceSquaredToBounds(float x, float y, const Tile& tile) {
    const float dx = x < tile.minimum[0] ? tile.minimum[0] - x
        : (x > tile.maximum[0] ? x - tile.maximum[0] : 0.0f);
    const float dy = y < tile.minimum[1] ? tile.minimum[1] - y
        : (y > tile.maximum[1] ? y - tile.maximum[1] : 0.0f);
    return dx * dx + dy * dy;
}

} // namespace

void Render(float cameraX, float cameraY, bool aircraftActive,
            float requestedGroundRadiusM) {
    if (!aircraftActive || !std::isfinite(cameraX) || !std::isfinite(cameraY) ||
        !g.RwRenderStateSet || !g.RwIm3DTransform ||
        !g.RwIm3DRenderIndexedPrimitive || !g.RwIm3DEnd || !LoadPack()) {
        return;
    }
    const float radius = std::clamp(
        std::isfinite(requestedGroundRadiusM) ? requestedGroundRadiusM
                                              : kMinimumRadiusM,
        kMinimumRadiusM, kMaximumRadiusM);
    const float radiusSquared = radius * radius;

    // This layer is a colour-buffer safety net.  It deliberately does not
    // write depth: the stock opaque scene follows and replaces it everywhere
    // GTA has real geometry, even if an authored parent differs from a child.
    g.RwRenderStateSet(1, nullptr);  // texture off
    g.RwRenderStateSet(6, reinterpret_cast<void*>(1));  // z-test on
    g.RwRenderStateSet(8, reinterpret_cast<void*>(0));  // z-write off
    g.RwRenderStateSet(12, reinterpret_cast<void*>(0)); // opaque vertex colour
    g.RwRenderStateSet(14, reinterpret_cast<void*>(1)); // stock fog
    g.RwRenderStateSet(20, reinterpret_cast<void*>(1)); // cull none
    constexpr unsigned int kIm3DFlags = 1u | 8u | 16u;
    constexpr int kTriangleList = 3;
    std::uint64_t selected = 0;
    std::uint64_t calls = 0;
    std::uint64_t failures = 0;
    for (const Tile& tile : g_state.tiles) {
        if (DistanceSquaredToBounds(cameraX, cameraY, tile) > radiusSquared)
            continue;
        ++selected;
        for (const Batch& batch : tile.batches) {
            if (g.RwIm3DTransform(
                    const_cast<Im3DVertex*>(batch.vertices.data()),
                    static_cast<unsigned int>(batch.vertices.size()), nullptr,
                    kIm3DFlags)) {
                g.RwIm3DRenderIndexedPrimitive(
                    kTriangleList,
                    const_cast<unsigned short*>(batch.indices.data()),
                    static_cast<int>(batch.indices.size()));
                g.RwIm3DEnd();
                ++calls;
            } else {
                ++failures;
            }
        }
    }
    ++g_state.eyePasses;
    g_state.selectedTiles += selected;
    g_state.drawCalls += calls;
    g_state.drawFailures += failures;
    if (g_state.eyePasses == 1 || g_state.eyePasses % 144 == 0) {
        LOGI("[hlod.air] eye=%llu radius=%.0f tiles=%llu draw=%llu fail=%llu",
             static_cast<unsigned long long>(g_state.eyePasses),
             static_cast<double>(radius),
             static_cast<unsigned long long>(selected),
             static_cast<unsigned long long>(calls),
             static_cast<unsigned long long>(failures));
    }
}

} // namespace savr::airhlod
