#pragma once

namespace savr::appearance {

enum HandSkin {
    HAND_SKIN_LIGHT = 0,
    HAND_SKIN_DARK,
    HAND_SKIN_COUNT
};

void Init();
int  GetHandSkin();
void SetHandSkin(int skin);
void CycleHandSkin(int direction = 1);
const char* HandSkinName();

} // namespace savr::appearance
