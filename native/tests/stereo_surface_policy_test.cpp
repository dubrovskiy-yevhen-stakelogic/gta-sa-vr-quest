#include "../src/StereoSurfacePolicy.h"

#include <cstdio>

int main() {
    using savr::vrcam::detail::IsStereoMainPassSize;
    struct Case {
        const char* name;
        int width;
        int height;
        int registeredWidth;
        int registeredHeight;
        bool expected;
    };
    const Case cases[] = {
        // This is the affected player's actual game surface. Its height was
        // rejected by the old 1024 minimum before any stereo frame published.
        {"Quest 2 registered surface", 1440, 1008, 1440, 1008, true},
        {"Quest 2 old fallback rejection", 1440, 1008, 0, 0, false},
        {"Quest 3 representative surface", 1680, 1176, 1680, 1176, true},
        {"XR eye size is not the game surface", 1440, 1584, 1440, 1008, false},
        {"small reflection", 512, 512, 1440, 1008, false},
        {"large offscreen pass", 2048, 2048, 1440, 1008, false},
        {"same width other height", 1440, 1024, 1440, 1008, false},
        {"same height other width", 1536, 1008, 1440, 1008, false},
        {"surface rotation is not a match", 1008, 1440, 1440, 1008, false},
        {"minimum registered dimensions", 64, 64, 64, 64, true},
        {"width below minimum", 63, 1024, 63, 1024, false},
        {"height below minimum", 1024, 63, 1024, 63, false},
        {"negative width", -1, 1024, -1, 1024, false},
        {"zero height", 1024, 0, 1024, 0, false},
        {"largest accepted dimensions", 8191, 8191, 8191, 8191, true},
        {"width upper bound", 8192, 1024, 8192, 1024, false},
        {"height upper bound", 1024, 8192, 1024, 8192, false},
        {"legacy main pass", 1024, 1024, 0, 0, true},
        {"legacy reflection", 512, 512, 0, 0, false},
        {"legacy narrow pass", 1008, 1440, 0, 0, false},
        {"legacy lower height", 1440, 1023, 0, 0, false},
        {"incomplete registration uses legacy", 1440, 1008, 1440, 0, false},
        {"incomplete registration allows legacy main", 1024, 1024, 0, 1008, true},
    };

    unsigned failures = 0;
    for (const Case& test : cases) {
        const bool actual = IsStereoMainPassSize(
            test.width, test.height, test.registeredWidth, test.registeredHeight);
        if (actual != test.expected) {
            std::fprintf(stderr, "FAIL: %s (got %d, expected %d)\n",
                         test.name, actual ? 1 : 0, test.expected ? 1 : 0);
            ++failures;
        }
    }
    if (failures != 0) return 1;
    std::printf("stereo surface policy: %zu cases passed\n",
                sizeof(cases) / sizeof(cases[0]));
    return 0;
}
