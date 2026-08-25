#pragma once

namespace savr::scopeaim {

// Scope state is advanced on the GameThread after physical weapon ownership.
// The bounded SA port covers the real sniper and both launchers. The unscoped
// country rifle/Mosin always uses its physical laser ray. Launcher
// activation is published only after its physical projectile route is ready.
void Update(bool interactionsBlocked);
void Reset();

// VrFire enables sniper optics only after its exact FireSniper hook and trigger
// route are both installed. A visible reticle must never coexist with vanilla
// camera-derived sniper bullets.
void SetSniperFireAvailable(bool available);
void SetProjectileFireAvailable(bool available);

struct VisualState {
    bool active{};
    int hand{-1};
    int weaponType{-1};
    float zoom{1.0f};
};

VisualState Snapshot();
bool IsActive();
bool IsActiveFor(int hand, int weaponType);

// Render the world through the narrowed optic frustum while the OpenXR
// projection layer remains at the normal headset FOV. This is the same
// full-view zoom scheme used by the current Quest Vice City implementation.
float RenderFovX(float baseFovXDegrees);

// When this hand/type owns the active optic, converge the calibrated muzzle ray
// on the HMD-centre world hit. An invalid centre ray returns false so callers can
// suppress the shot instead of silently firing outside the visible reticle.
// For a non-active scope this is a no-op and returns true.
bool ApplyReticleAim(int hand, int weaponType,
                     const float muzzleOrigin[3], float direction[3]);

} // namespace savr::scopeaim
