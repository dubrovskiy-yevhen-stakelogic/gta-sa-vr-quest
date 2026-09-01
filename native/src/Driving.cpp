#include "Driving.h"

#include "Locomotion.h"

#include "Log.h"
#include "PhysicalWeapon.h"
#include "Symbols.h"
#include "VrCamera.h"
#include "Xr.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>

namespace savr::driving {
namespace {

struct V3 { float x{}, y{}, z{}; };
V3 operator+(V3 a, V3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
V3 operator*(V3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
float Dot(V3 a, V3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
V3 Cross(V3 a, V3 b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
float Length(V3 v) { return std::sqrt(std::max(0.0f, Dot(v, v))); }
V3 Normalized(V3 v) {
    const float length = Length(v);
    return length > 0.000001f ? v*(1.0f/length) : V3{};
}

double MonotonicSeconds() {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<double>(value.tv_sec) +
           static_cast<double>(value.tv_nsec) / 1000000000.0;
}

bool IsBicycleModel(int model) {
    // GTA SA's three pedal-powered vehicles. Motorcycles keep the physical
    // handlebar/twist-throttle solver ported from Vice City.
    return model == 481 || model == 509 || model == 510;
}
V3 RotateAroundAxis(V3 v, V3 axis, float angle) {
    axis = Normalized(axis);
    const float c = std::cos(angle), s = std::sin(angle);
    return v*c+Cross(axis, v)*s+axis*(Dot(axis, v)*(1.0f-c));
}
float WrapAngle(float angle) {
    constexpr float pi = 3.14159265358979323846f, twoPi = 2.0f*pi;
    while (angle > pi) angle -= twoPi;
    while (angle < -pi) angle += twoPi;
    return angle;
}
float UnwrapAngle(float angle, float reference) {
    constexpr float pi = 3.14159265358979323846f, twoPi = 2.0f*pi;
    while (angle-reference > pi) angle -= twoPi;
    while (angle-reference < -pi) angle += twoPi;
    return angle;
}
float PlanarAngle(V3 vector, V3 right, V3 up) {
    return std::atan2(Dot(vector, up), Dot(vector, right));
}
bool NormalizeQuaternion(const float input[4],float output[4]) {
    if (!input||!output) return false;
    const float length=std::sqrt(input[0]*input[0]+input[1]*input[1]+
                                 input[2]*input[2]+input[3]*input[3]);
    if (!std::isfinite(length)||length<0.000001f) return false;
    for (int i=0;i<4;++i) output[i]=input[i]/length;
    return true;
}
float DotQuaternion(const float a[4],const float b[4]) {
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];
}
void MultiplyQuaternion(const float a[4],const float b[4],float output[4]) {
    output[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
    output[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    output[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
    output[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}

constexpr int kDefaults[F_COUNT] = {
    0, 0, 15, 0, 15,
    0, 15, 0, 15,
    0, 48, 43, 18, 30, 30, 82,
    0, 15, 0, 15, 0, 15,
    0, 0, 0, 0, 0, 0
};
// Temporary known-good baseline.  The vehicle camera and basic R2/L2/stick
// controls must be proven independently before the physical wheel is allowed
// to own hands/steering again.  Keep the hooks installed so DEFAULT driving is
// usable, but make the immersive solver and wheel renderer completely inert.
constexpr bool kImmersiveDrivingEnabled = true;
constexpr int kDrivingConfigVersion = 3;
const char* const kPath =
    "/sdcard/Android/data/com.rockstargames.gtasa/files/vr_driving.ini";

std::once_flag g_initOnce;
std::mutex g_saveMutex;
std::atomic<int> g_carMode{MODE_DEFAULT};
std::atomic<int> g_bikeMode{MODE_IMMERSIVE};
// Boats support the full VC set: DEFAULT sticks, IMMERSIVE helm wheel.
// Planes/helicopters are DEFAULT-only (no immersive flight controls yet).
std::atomic<int> g_boatMode{MODE_DEFAULT};
// Planes get a physical yoke in IMMERSIVE: rotate for roll, pull/push along
// its axis for pitch. Helicopters stay DEFAULT regardless of this mode.
std::atomic<int> g_planeMode{MODE_DEFAULT};
std::atomic<float> g_planePitch{0.0f};
std::atomic<int> g_yokeSensitivity{100}; // percent, 25..200
// HANDLEBAR+TRIGGER by default: it is the only bicycle mode with visible bar
// sockets to grab and calibrate — MOTION shows nothing on the bars at all,
// which read as "the grips are missing". MOTION stays selectable in the menu.
std::atomic<int> g_bicycleMode{BICYCLE_HANDLEBAR_TRIGGER};
// Palm pressed onto the wheel HUB (centre) sounds the horn. Strictly the hub:
// an open hand inside 45% of the wheel radius (9 cm cap) AND within 5 cm of
// the wheel plane. Hands gripping the rim or holding a weapon never qualify,
// so ordinary steering cannot honk by accident.
std::atomic<bool> g_hornPressed{false};
// Camera view per vehicle type. The Rhino tank ignores the setting and always
// uses the chase view — its hull has no usable cockpit line of sight.
std::atomic<int> g_carView{VIEW_FIRST_PERSON};
std::atomic<int> g_bikeView{VIEW_FIRST_PERSON};
// Helicopters and planes share one view setting: several plane models have no
// modelled cockpit at all, so first person is a black hull there.
std::atomic<int> g_airView{VIEW_FIRST_PERSON};
std::atomic<int> g_boatView{VIEW_FIRST_PERSON};
std::atomic<int> g_activeVehicleAppearance{0};
constexpr int kRhinoModelId = 432;
std::atomic<int> g_value[F_COUNT] = {
    0, 0, 15, 0, 15,
    0, 15, 0, 15,
    0, 48, 43, 18, 30, 30, 82,
    // plane seat D/H, boat seat D/H, immersive boat seat D/H
    0, 15, 0, 15, 0, 15,
    // bike/car-immersive/bike-immersive/plane/boat/boat-immersive side
    0, 0, 0, 0, 0, 0
};
std::atomic<bool> g_wheelVisible{true};
std::atomic<bool> g_highlights{true};
std::atomic<bool> g_bikeHandsFollowTilt{true};
std::atomic<bool> g_bikeHorizonLocked{true};
std::atomic<int> g_bikeVisualLeanPercent{50};
std::atomic<bool> g_keepRiderOnFlips{false};
std::atomic<bool> g_hideInteriorGlass{true};
std::atomic<bool> g_drivebyImmersive{false};
// Car camera motion: default LEVEL (comfort); FULL TILT throws the view with
// the car body over hills, jumps and rolls for players who dislike the
// stabilised horizon.
std::atomic<bool> g_carCameraTilt{false};
// MOTION steering (Vice City port, verbatim curve): the chosen hand's
// aim-pose yaw against a reference captured on the first throttle press.
// +-30 deg maps to half steering (fine control), 30..90 deg to the rest;
// 3 deg deadzone. GameThread-only except the atomic output.
std::atomic<float> g_motionSteering{0.0f};
std::atomic<int>   g_motionHand{1};
void* g_motionVehicle = nullptr;
bool  g_motionRefValid = false;
float g_motionRefHeading = 0.0f;
std::atomic<bool> g_motionEngaged{false};
// Boat camera motion, same semantics for appearance-4 hulls: LEVEL keeps
// the stabilised horizon, WITH BOAT throws the view with the hull.
std::atomic<bool> g_boatCameraTilt{false};
std::atomic<bool> g_inputBlocked{false};
std::atomic<bool> g_menuPreview{false};
std::atomic<int> g_menuCalibrationHand{-1};
std::atomic<int> g_activeVehicleType{VEHICLE_NONE};
std::atomic<int> g_activeVehicleModel{-1};
std::atomic<int> g_activeBikeAccelerator{BIKE_ACCEL_HOLD_TRIGGER};
std::atomic<bool> g_radioButtonDown{false};
std::atomic<bool> g_radioChangeJustPressed{false};
std::atomic<bool> g_radioPrevJustPressed{false};
// Station-name toast (player request: visible radio navigation).
double g_radioToastUntil = 0.0;
int    g_radioLastStation = -999;

std::mutex g_stateMutex;
WheelVisualState g_visual{};
// The aircraft yoke follows the seated head bone, but that bone and the
// world-to-tracking base can both be stale for the first few frames after a
// save loads directly in a plane.  Keep the exit-drift reference outside
// BuildWheelTracking so a real leave/re-enter transition can invalidate it,
// even when GTA reuses the same CVehicle instance.
void* g_wheelSeatVehicle{};
// Boarding timestamp: tall vehicles (Monster, Dozer) have long climb-in
// animations, and freezing that mid-climb pose as "the seat" hid the wheel
// forever once the real seated pose drifted past the exit gate.
double g_wheelSeatEnteredAt{0.0};
V3 g_wheelSeatLocal{};
// Exit-detection reference tracked separately from the anchor: it follows the
// RAW ped-root position, so an anchor-source switch (root -> head bone on
// tall/long cabs) can never register as a 2m "seat drift" and hide the wheel.
V3 g_wheelSeatGateLocal{};
// Head-bone-derived seat candidate cached alongside; at arming time the two
// are compared ONCE (seated, settled pose) to pick the anchor source.
V3 g_wheelSeatHeadLocal{};
bool g_wheelSeatHeadValid{};
bool g_wheelSeatArmed{};
bool g_wheelSeatLocalValid{};
// How long the root point has been still (frame-to-frame) during seeding;
// lets tall cabs arm as soon as the climb-in settles instead of always
// waiting the full 3-second ceiling.
double g_wheelSeatStableSince{0.0};
bool g_grabbed[2]{};
bool g_gripDown[2]{};
float g_grabReferenceAngle[2]{};
float g_continuousAngle[2]{};
bool g_angleValid[2]{};
float g_twoHandReferenceAngle{};
float g_twoHandContinuousAngle{};
bool g_twoHandAngleValid{};
bool g_bikeOneHandValid{};
int g_bikeOwnershipMask{};
V3 g_bikeReferenceHand{};
V3 g_bikeSeedChord{};
float g_bikeReferencePhysicalAngle{};
float g_bikeThrottle{};
bool g_bikeThrottleGestureActive{};
bool g_bikeThrottleReferenceValid{};
float g_bikeThrottleReferenceOrientation[4]{0.0f,0.0f,0.0f,1.0f};
float g_bikeLean{};
bool g_bikeLeanReferenceValid[2]{};
float g_bikeLeanReferenceY[2]{};
int g_bikeLeanGestureState{};

bool g_bicycleGestureActive{};
float g_bicycleSteering{};
float g_bicyclePedalLevel{};
float g_bicyclePedalPulse{};
float g_bicycleTapPhase{};
bool g_bicyclePoseValid{};
V3 g_bicyclePreviousHand[2]{};
double g_bicyclePreviousTime{};
bool g_bicycleTriggerDown{};
float g_bicycleTriggerPulse{};
double g_bicycleTriggerPreviousTime{};

struct ModelCalibration {
    int seatSide[MODE_COUNT]{};
    int seatForward[MODE_COUNT]{};
    int seatHeight[MODE_COUNT]{};
    int control[2][CONTROL_FIELD_COUNT]{};
    int wheel[WHEEL_CAL_FIELD_COUNT]{};
    int acceleratorMode{BIKE_ACCEL_HOLD_TRIGGER};
};

int ClampWheelCalibration(int field, int value) {
    switch (field) {
        case WHEEL_CAL_SIDE:
        case WHEEL_CAL_FORWARD:
        case WHEEL_CAL_HEIGHT:  return std::clamp(value, -500, 500);  // cm
        case WHEEL_CAL_RADIUS:  return std::clamp(value, -100, 200);  // cm delta
        default:                return std::clamp(value, -1440, 1440);// half-deg
    }
}
std::mutex g_modelMutex;
std::map<int, ModelCalibration> g_modelCalibration;

int ClampField(int field, int value) {
    switch (field) {
        case F_SIDE:
        case F_BIKE_SEAT_SIDE:
        case F_IMMERSIVE_CAR_SEAT_SIDE:
        case F_IMMERSIVE_BIKE_SEAT_SIDE:
        case F_PLANE_SEAT_SIDE:
        case F_BOAT_SEAT_SIDE:
        case F_IMMERSIVE_BOAT_SEAT_SIDE:
        case F_DISTANCE:
        case F_HEIGHT:
        case F_BIKE_SEAT_DISTANCE:
        case F_BIKE_SEAT_HEIGHT:
        case F_IMMERSIVE_CAR_SEAT_DISTANCE:
        case F_IMMERSIVE_BIKE_SEAT_DISTANCE:
        case F_IMMERSIVE_CAR_SEAT_HEIGHT:
        case F_IMMERSIVE_BIKE_SEAT_HEIGHT:
        case F_PLANE_SEAT_DISTANCE:
        case F_PLANE_SEAT_HEIGHT:
        case F_BOAT_SEAT_DISTANCE:
        case F_BOAT_SEAT_HEIGHT:
        case F_IMMERSIVE_BOAT_SEAT_DISTANCE:
        case F_IMMERSIVE_BOAT_SEAT_HEIGHT:
        case F_WHEEL_SIDE:
        case F_WHEEL_DISTANCE:
        case F_WHEEL_HEIGHT:
        case F_BIKE_DISTANCE:
        case F_BIKE_HEIGHT:
            return std::clamp(value, -500, 500);
        case F_WHEEL_RADIUS:   return std::clamp(value, 1, 200);
        case F_BIKE_HALF_WIDTH:return std::clamp(value, 1, 200);
        default:               return 0;
    }
}

int GlobalSeatSideField(int vehicleType, int mode) {
    const bool immersive = mode == MODE_IMMERSIVE;
    if (vehicleType == VEHICLE_BIKE)
        return immersive ? F_IMMERSIVE_BIKE_SEAT_SIDE : F_BIKE_SEAT_SIDE;
    if (vehicleType == VEHICLE_PLANE) return F_PLANE_SEAT_SIDE;
    if (vehicleType == VEHICLE_BOAT)
        return immersive ? F_IMMERSIVE_BOAT_SEAT_SIDE : F_BOAT_SEAT_SIDE;
    return immersive ? F_IMMERSIVE_CAR_SEAT_SIDE : F_SIDE;
}

int GlobalSeatField(int vehicleType, int mode, bool height) {
    const bool immersive = mode == MODE_IMMERSIVE;
    if (vehicleType == VEHICLE_BIKE) {
        if (immersive)
            return height ? F_IMMERSIVE_BIKE_SEAT_HEIGHT :
                            F_IMMERSIVE_BIKE_SEAT_DISTANCE;
        return height ? F_BIKE_SEAT_HEIGHT : F_BIKE_SEAT_DISTANCE;
    }
    if (vehicleType == VEHICLE_PLANE)
        return height ? F_PLANE_SEAT_HEIGHT : F_PLANE_SEAT_DISTANCE;
    if (vehicleType == VEHICLE_BOAT) {
        if (immersive)
            return height ? F_IMMERSIVE_BOAT_SEAT_HEIGHT :
                            F_IMMERSIVE_BOAT_SEAT_DISTANCE;
        return height ? F_BOAT_SEAT_HEIGHT : F_BOAT_SEAT_DISTANCE;
    }
    if (immersive)
        return height ? F_IMMERSIVE_CAR_SEAT_HEIGHT :
                        F_IMMERSIVE_CAR_SEAT_DISTANCE;
    return height ? F_HEIGHT : F_DISTANCE;
}

int ClampModelSeat(bool height, int value) {
    (void)height;
    return std::clamp(value, -500, 500);
}

int ClampControlValue(int field, int value) {
    return std::clamp(value, field >= CONTROL_ROT_X ? -2880 : -2000,
                             field >= CONTROL_ROT_X ?  2880 :  2000);
}

void Save() {
    std::lock_guard<std::mutex> lock(g_saveMutex);
    FILE* file = std::fopen(kPath, "w");
    if (!file) { LOGW("[driving] could not save %s", kPath); return; }
    std::fprintf(file, "DrivingConfigVersion=%d\n", kDrivingConfigVersion);
    // Keep DrivingMode for backward compatibility with 0.7/0.8.0, while the
    // two authoritative values match current Quest Vice City.
    std::fprintf(file, "DrivingMode=%d\n", g_carMode.load());
    std::fprintf(file, "CarDrivingType=%d\n", g_carMode.load());
    std::fprintf(file, "BikeDrivingType=%d\n", g_bikeMode.load());
    std::fprintf(file, "BoatDrivingType=%d\n", g_boatMode.load());
    std::fprintf(file, "PlaneDrivingType=%d\n", g_planeMode.load());
    std::fprintf(file, "YokeSensitivityPercent=%d\n", g_yokeSensitivity.load());
    std::fprintf(file, "BicycleControlMode=%d\n", g_bicycleMode.load());
    std::fprintf(file, "CarCameraView=%d\n", g_carView.load());
    std::fprintf(file, "BikeCameraView=%d\n", g_bikeView.load());
    std::fprintf(file, "AirCameraView=%d\n", g_airView.load());
    std::fprintf(file, "BoatCameraView=%d\n", g_boatView.load());
    std::fprintf(file, "PlaneSeatDistanceCm=%d\n",
                 g_value[F_PLANE_SEAT_DISTANCE].load());
    std::fprintf(file, "PlaneSeatHeightCm=%d\n",
                 g_value[F_PLANE_SEAT_HEIGHT].load());
    std::fprintf(file, "PlaneSeatSideCm=%d\n",
                 g_value[F_PLANE_SEAT_SIDE].load());
    std::fprintf(file, "BoatSeatDistanceCm=%d\n",
                 g_value[F_BOAT_SEAT_DISTANCE].load());
    std::fprintf(file, "BoatSeatHeightCm=%d\n",
                 g_value[F_BOAT_SEAT_HEIGHT].load());
    std::fprintf(file, "BoatSeatSideCm=%d\n",
                 g_value[F_BOAT_SEAT_SIDE].load());
    std::fprintf(file, "ImmersiveBoatSeatDistanceCm=%d\n",
                 g_value[F_IMMERSIVE_BOAT_SEAT_DISTANCE].load());
    std::fprintf(file, "ImmersiveBoatSeatHeightCm=%d\n",
                 g_value[F_IMMERSIVE_BOAT_SEAT_HEIGHT].load());
    std::fprintf(file, "ImmersiveBoatSeatSideCm=%d\n",
                 g_value[F_IMMERSIVE_BOAT_SEAT_SIDE].load());
    std::fprintf(file, "DefaultSeatSideCm=%d\n", g_value[F_SIDE].load());
    std::fprintf(file, "DefaultSeatDistanceCm=%d\n", g_value[F_DISTANCE].load());
    std::fprintf(file, "DefaultSeatHeightCm=%d\n", g_value[F_HEIGHT].load());
    std::fprintf(file, "DefaultBikeSeatDistanceCm=%d\n", g_value[F_BIKE_SEAT_DISTANCE].load());
    std::fprintf(file, "DefaultBikeSeatHeightCm=%d\n", g_value[F_BIKE_SEAT_HEIGHT].load());
    std::fprintf(file, "DefaultBikeSeatSideCm=%d\n",
                 g_value[F_BIKE_SEAT_SIDE].load());
    std::fprintf(file, "ImmersiveCarSeatDistanceCm=%d\n",
                 g_value[F_IMMERSIVE_CAR_SEAT_DISTANCE].load());
    std::fprintf(file, "ImmersiveCarSeatHeightCm=%d\n",
                 g_value[F_IMMERSIVE_CAR_SEAT_HEIGHT].load());
    std::fprintf(file, "ImmersiveCarSeatSideCm=%d\n",
                 g_value[F_IMMERSIVE_CAR_SEAT_SIDE].load());
    std::fprintf(file, "ImmersiveBikeSeatDistanceCm=%d\n",
                 g_value[F_IMMERSIVE_BIKE_SEAT_DISTANCE].load());
    std::fprintf(file, "ImmersiveBikeSeatHeightCm=%d\n",
                 g_value[F_IMMERSIVE_BIKE_SEAT_HEIGHT].load());
    std::fprintf(file, "ImmersiveBikeSeatSideCm=%d\n",
                 g_value[F_IMMERSIVE_BIKE_SEAT_SIDE].load());
    std::fprintf(file, "WheelCenterSideCm=%d\n", g_value[F_WHEEL_SIDE].load());
    std::fprintf(file, "WheelCenterDistanceCm=%d\n", g_value[F_WHEEL_DISTANCE].load());
    std::fprintf(file, "WheelCenterHeightCm=%d\n", g_value[F_WHEEL_HEIGHT].load());
    std::fprintf(file, "WheelRadiusCm=%d\n", g_value[F_WHEEL_RADIUS].load());
    std::fprintf(file, "BikeHalfWidthCm=%d\n", g_value[F_BIKE_HALF_WIDTH].load());
    std::fprintf(file, "BikeDistanceCm=%d\n", g_value[F_BIKE_DISTANCE].load());
    std::fprintf(file, "BikeHeightCm=%d\n", g_value[F_BIKE_HEIGHT].load());
    std::fprintf(file, "ImmersiveWheelVisible=%d\n", g_wheelVisible.load()?1:0);
    std::fprintf(file, "HandleHighlights=%d\n", g_highlights.load()?1:0);
    std::fprintf(file, "BikeHandsFollowTilt=%d\n",
                 g_bikeHandsFollowTilt.load()?1:0);
    std::fprintf(file, "BikeLockHorizon=%d\n", g_bikeHorizonLocked.load()?1:0);
    std::fprintf(file, "BikeVisualLeanPercent=%d\n",
                 g_bikeVisualLeanPercent.load());
    std::fprintf(file, "KeepRiderOnFlips=%d\n",
                 g_keepRiderOnFlips.load()?1:0);
    std::fprintf(file, "HideInteriorGlass=%d\n",
                 g_hideInteriorGlass.load()?1:0);
    std::fprintf(file, "DrivebyImmersive=%d\n",
                 g_drivebyImmersive.load()?1:0);
    std::fprintf(file, "CarCameraTilt=%d\n",
                 g_carCameraTilt.load()?1:0);
    std::fprintf(file, "MotionSteeringHand=%d\n", g_motionHand.load());
    std::fprintf(file, "BoatCameraTilt=%d\n",
                 g_boatCameraTilt.load()?1:0);
    {
        static const char* const controlKeys[CONTROL_FIELD_COUNT] = {
            "OffsetX", "OffsetY", "OffsetZ",
            "RotationX", "RotationY", "RotationZ"
        };
        std::lock_guard<std::mutex> modelLock(g_modelMutex);
        for (const auto& entry : g_modelCalibration) {
            const int model = entry.first;
            const ModelCalibration& calibration = entry.second;
            for (int mode = 0; mode < MODE_COUNT; ++mode) {
                std::fprintf(file, "ModelSeat.%d.%d.SideCm=%d\n",
                             model, mode, calibration.seatSide[mode]);
                std::fprintf(file, "ModelSeat.%d.%d.ForwardCm=%d\n",
                             model, mode, calibration.seatForward[mode]);
                std::fprintf(file, "ModelSeat.%d.%d.HeightCm=%d\n",
                             model, mode, calibration.seatHeight[mode]);
            }
            std::fprintf(file, "BikeAccelerator.%d=%d\n", model,
                         calibration.acceleratorMode);
            for (int hand = 0; hand < 2; ++hand)
                for (int field = 0; field < CONTROL_FIELD_COUNT; ++field)
                    std::fprintf(file, "Control.%d.%d.%s=%d\n",
                                 model, hand, controlKeys[field],
                                 calibration.control[hand][field]);
            for (int field = 0; field < WHEEL_CAL_FIELD_COUNT; ++field)
                std::fprintf(file, "WheelCalib.%d.%d=%d\n",
                             model, field, calibration.wheel[field]);
        }
    }
    std::fclose(file);
}

void Load() {
    int configVersion=0;
    int legacyMode=MODE_DEFAULT,carMode=MODE_DEFAULT,bikeMode=MODE_IMMERSIVE;
    int bicycleMode=BICYCLE_HANDLEBAR_TRIGGER;
    bool carModeSeen=false,bikeModeSeen=false;
    bool immersiveCarSeatSeen=false, immersiveBikeSeatSeen=false;
    int values[F_COUNT];
    std::copy(std::begin(kDefaults), std::end(kDefaults), values);
    int visible = 1, highlights = 1, bikeHandsFollowTilt = 1;
    int bikeLockHorizon = 1, bikeVisualLeanPercent = 50;
    int keepRiderOnFlips = 0;
    int hideInteriorGlass = 1;
    int drivebyImmersive = 0;
    int carCameraTilt = 0;
    int motionHand = 1;
    int boatCameraTilt = 0;
    if (FILE* file = std::fopen(kPath, "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), file)) {
            int value = 0;
            if      (std::sscanf(line, "DrivingConfigVersion=%d", &value)==1)
                configVersion=value;
            else if (std::sscanf(line, "DrivingMode=%d", &value)==1) legacyMode=value;
            else if (std::sscanf(line, "CarDrivingType=%d", &value)==1||
                     std::sscanf(line, "CarDrivingMode=%d", &value)==1) {
                carMode=value; carModeSeen=true;
            }
            else if (std::sscanf(line, "BikeDrivingType=%d", &value)==1||
                     std::sscanf(line, "BikeDrivingMode=%d", &value)==1) {
                bikeMode=value; bikeModeSeen=true;
            }
            // Renamed from BicycleImmersiveMode: earlier builds silently saved
            // MOTION (no bar sockets, nothing to grab or calibrate) as the
            // default. Ignoring the old key migrates everyone to the visible
            // HANDLEBAR+TRIGGER default once; the menu choice re-saves under
            // the new key.
            else if (std::sscanf(line, "BicycleControlMode=%d", &value)==1)
                bicycleMode=value;
            else if (std::sscanf(line, "CarCameraView=%d", &value)==1)
                g_carView.store(std::clamp(value,0,VIEW_MODE_COUNT-1));
            else if (std::sscanf(line, "BikeCameraView=%d", &value)==1)
                g_bikeView.store(std::clamp(value,0,VIEW_MODE_COUNT-1));
            else if (std::sscanf(line, "AirCameraView=%d", &value)==1)
                g_airView.store(std::clamp(value,0,VIEW_MODE_COUNT-1));
            else if (std::sscanf(line, "BoatCameraView=%d", &value)==1)
                g_boatView.store(std::clamp(value,0,VIEW_MODE_COUNT-1));
            else if (std::sscanf(line, "BoatDrivingType=%d", &value)==1)
                g_boatMode.store(value == MODE_IMMERSIVE ? MODE_IMMERSIVE
                                                         : MODE_DEFAULT);
            else if (std::sscanf(line, "PlaneDrivingType=%d", &value)==1)
                g_planeMode.store(value == MODE_IMMERSIVE ? MODE_IMMERSIVE
                                                          : MODE_DEFAULT);
            else if (std::sscanf(line, "YokeSensitivityPercent=%d", &value)==1)
                g_yokeSensitivity.store(std::clamp(value,25,200));
            else if (std::sscanf(line, "PlaneSeatDistanceCm=%d", &value)==1)
                values[F_PLANE_SEAT_DISTANCE]=value;
            else if (std::sscanf(line, "PlaneSeatHeightCm=%d", &value)==1)
                values[F_PLANE_SEAT_HEIGHT]=value;
            else if (std::sscanf(line, "PlaneSeatSideCm=%d", &value)==1)
                values[F_PLANE_SEAT_SIDE]=value;
            else if (std::sscanf(line, "BoatSeatDistanceCm=%d", &value)==1)
                values[F_BOAT_SEAT_DISTANCE]=value;
            else if (std::sscanf(line, "BoatSeatHeightCm=%d", &value)==1)
                values[F_BOAT_SEAT_HEIGHT]=value;
            else if (std::sscanf(line, "BoatSeatSideCm=%d", &value)==1)
                values[F_BOAT_SEAT_SIDE]=value;
            else if (std::sscanf(line, "ImmersiveBoatSeatDistanceCm=%d", &value)==1)
                values[F_IMMERSIVE_BOAT_SEAT_DISTANCE]=value;
            else if (std::sscanf(line, "ImmersiveBoatSeatHeightCm=%d", &value)==1)
                values[F_IMMERSIVE_BOAT_SEAT_HEIGHT]=value;
            else if (std::sscanf(line, "ImmersiveBoatSeatSideCm=%d", &value)==1)
                values[F_IMMERSIVE_BOAT_SEAT_SIDE]=value;
            else if (std::sscanf(line, "DefaultSeatSideCm=%d", &value)==1) values[F_SIDE]=value;
            else if (std::sscanf(line, "DefaultSeatDistanceCm=%d", &value)==1) values[F_DISTANCE]=value;
            else if (std::sscanf(line, "DefaultSeatHeightCm=%d", &value)==1) values[F_HEIGHT]=value;
            else if (std::sscanf(line, "DefaultBikeSeatDistanceCm=%d", &value)==1) values[F_BIKE_SEAT_DISTANCE]=value;
            else if (std::sscanf(line, "DefaultBikeSeatHeightCm=%d", &value)==1) values[F_BIKE_SEAT_HEIGHT]=value;
            else if (std::sscanf(line, "DefaultBikeSeatSideCm=%d", &value)==1) values[F_BIKE_SEAT_SIDE]=value;
            else if (std::sscanf(line, "ImmersiveCarSeatDistanceCm=%d", &value)==1) {
                values[F_IMMERSIVE_CAR_SEAT_DISTANCE]=value; immersiveCarSeatSeen=true;
            }
            else if (std::sscanf(line, "ImmersiveCarSeatHeightCm=%d", &value)==1) {
                values[F_IMMERSIVE_CAR_SEAT_HEIGHT]=value; immersiveCarSeatSeen=true;
            }
            else if (std::sscanf(line, "ImmersiveCarSeatSideCm=%d", &value)==1)
                values[F_IMMERSIVE_CAR_SEAT_SIDE]=value;
            else if (std::sscanf(line, "ImmersiveBikeSeatDistanceCm=%d", &value)==1) {
                values[F_IMMERSIVE_BIKE_SEAT_DISTANCE]=value; immersiveBikeSeatSeen=true;
            }
            else if (std::sscanf(line, "ImmersiveBikeSeatHeightCm=%d", &value)==1) {
                values[F_IMMERSIVE_BIKE_SEAT_HEIGHT]=value; immersiveBikeSeatSeen=true;
            }
            else if (std::sscanf(line, "ImmersiveBikeSeatSideCm=%d", &value)==1)
                values[F_IMMERSIVE_BIKE_SEAT_SIDE]=value;
            else if (std::sscanf(line, "WheelCenterSideCm=%d", &value)==1) values[F_WHEEL_SIDE]=value;
            else if (std::sscanf(line, "WheelCenterDistanceCm=%d", &value)==1) values[F_WHEEL_DISTANCE]=value;
            else if (std::sscanf(line, "WheelCenterHeightCm=%d", &value)==1) values[F_WHEEL_HEIGHT]=value;
            else if (std::sscanf(line, "WheelRadiusCm=%d", &value)==1) values[F_WHEEL_RADIUS]=value;
            else if (std::sscanf(line, "BikeHalfWidthCm=%d", &value)==1) values[F_BIKE_HALF_WIDTH]=value;
            else if (std::sscanf(line, "BikeDistanceCm=%d", &value)==1) values[F_BIKE_DISTANCE]=value;
            else if (std::sscanf(line, "BikeHeightCm=%d", &value)==1) values[F_BIKE_HEIGHT]=value;
            else if (std::sscanf(line, "ImmersiveWheelVisible=%d", &value)==1) visible=value;
            else if (std::sscanf(line, "HandleHighlights=%d", &value)==1) highlights=value;
            else if (std::sscanf(line, "BikeHandsFollowTilt=%d", &value)==1)
                bikeHandsFollowTilt=value;
            else if (std::sscanf(line, "BikeLockHorizon=%d", &value)==1) bikeLockHorizon=value;
            else if (std::sscanf(line, "BikeVisualLeanPercent=%d", &value)==1)
                bikeVisualLeanPercent=value;
            else if (std::sscanf(line, "KeepRiderOnFlips=%d", &value)==1)
                keepRiderOnFlips=value;
            else if (std::sscanf(line, "HideInteriorGlass=%d", &value)==1)
                hideInteriorGlass=value;
            else if (std::sscanf(line, "DrivebyImmersive=%d", &value)==1)
                drivebyImmersive=value;
            else if (std::sscanf(line, "CarCameraTilt=%d", &value)==1)
                carCameraTilt=value;
            else if (std::sscanf(line, "MotionSteeringHand=%d", &value)==1)
                motionHand=value;
            else if (std::sscanf(line, "BoatCameraTilt=%d", &value)==1)
                boatCameraTilt=value;
            else {
                int model = -1, mode = -1, hand = -1;
                if (std::sscanf(line, "ModelSeat.%d.%d.SideCm=%d",
                                &model, &mode, &value)==3 &&
                    model >= 0 && mode >= 0 && mode < MODE_COUNT) {
                    g_modelCalibration[model].seatSide[mode] =
                        ClampModelSeat(false, value);
                } else if (std::sscanf(line, "ModelSeat.%d.%d.ForwardCm=%d",
                                &model, &mode, &value)==3 &&
                    model >= 0 && mode >= 0 && mode < MODE_COUNT) {
                    g_modelCalibration[model].seatForward[mode] =
                        ClampModelSeat(false, value);
                } else if (std::sscanf(line, "ModelSeat.%d.%d.HeightCm=%d",
                                       &model, &mode, &value)==3 &&
                           model >= 0 && mode >= 0 && mode < MODE_COUNT) {
                    g_modelCalibration[model].seatHeight[mode] =
                        ClampModelSeat(true, value);
                } else if (std::sscanf(line, "BikeAccelerator.%d=%d",
                                       &model, &value)==2 && model >= 0) {
                    g_modelCalibration[model].acceleratorMode=
                        std::clamp(value,0,BIKE_ACCEL_MODE_COUNT-1);
                } else if (std::sscanf(line, "WheelCalib.%d.%d=%d",
                                       &model, &mode, &value)==3 &&
                           model >= 0 && mode >= 0 &&
                           mode < WHEEL_CAL_FIELD_COUNT) {
                    g_modelCalibration[model].wheel[mode]=
                        ClampWheelCalibration(mode, value);
                } else {
                    static const char* const controlKeys[CONTROL_FIELD_COUNT] = {
                        "OffsetX", "OffsetY", "OffsetZ",
                        "RotationX", "RotationY", "RotationZ"
                    };
                    for (int field = 0; field < CONTROL_FIELD_COUNT; ++field) {
                        char pattern[64];
                        std::snprintf(pattern, sizeof(pattern),
                                      "Control.%%d.%%d.%s=%%d", controlKeys[field]);
                        if (std::sscanf(line, pattern, &model, &hand, &value)==3 &&
                            model >= 0 && hand >= 0 && hand < 2) {
                            g_modelCalibration[model].control[hand][field] =
                                ClampControlValue(field, value);
                            break;
                        }
                    }
                }
            }
        }
        std::fclose(file);
    }
    // 0.8.9 stored TWIST GRIP (0) as the implicit value for every motorcycle,
    // including models which the player never configured. That makes R2 appear
    // broken because the solver keeps waiting for a physical wrist rotation.
    // Migrate old files once to the requested ordinary held-R2 default. The
    // version marker preserves any later per-model switch back to TWIST GRIP.
    if (configVersion < kDrivingConfigVersion) {
        int migrated=0;
        for (auto& entry : g_modelCalibration) {
            if (!IsBicycleModel(entry.first) &&
                entry.second.acceleratorMode==BIKE_ACCEL_PHYSICAL_OR_TAP) {
                entry.second.acceleratorMode=BIKE_ACCEL_HOLD_TRIGGER;
                ++migrated;
            }
        }
        LOGI("[driving] config v%d -> v%d: %d motorcycles default to HOLD R2",
             configVersion,kDrivingConfigVersion,migrated);
    }
    if (!carModeSeen) carMode=legacyMode;
    if (!immersiveCarSeatSeen) {
        values[F_IMMERSIVE_CAR_SEAT_DISTANCE]=values[F_DISTANCE];
        values[F_IMMERSIVE_CAR_SEAT_HEIGHT]=values[F_HEIGHT];
    }
    if (!immersiveBikeSeatSeen) {
        values[F_IMMERSIVE_BIKE_SEAT_DISTANCE]=values[F_BIKE_SEAT_DISTANCE];
        values[F_IMMERSIVE_BIKE_SEAT_HEIGHT]=values[F_BIKE_SEAT_HEIGHT];
    }
    // The legacy global DEFAULT is precisely what made a newly-added bike
    // cockpit invisible in 0.8.0. A file without BikeDrivingType migrates to
    // the useful VC default, while an explicitly saved bike choice is retained.
    if (!bikeModeSeen) {
        bikeMode=MODE_IMMERSIVE;
        // 0.8.0 had one global mode and the test device also retained
        // HandleHighlights=0, making a valid bike cockpit look completely
        // absent. This one-time format migration makes the new feature visible;
        // subsequent saves have BikeDrivingType and respect the toggle.
        highlights=1;
        LOGI("[driving] migrating legacy settings: BIKE=IMMERSIVE, highlights=ON");
    }
    // MOTION steering is not shipped.  Migrate old/invalid values to the
    // ordinary stick mode instead of silently enabling a hand-driven input
    // path that is no longer exposed by the menu.
    const int loadedCarMode=carMode==MODE_IMMERSIVE?MODE_IMMERSIVE:MODE_DEFAULT;
    const int loadedBikeMode=bikeMode==MODE_IMMERSIVE?MODE_IMMERSIVE:MODE_DEFAULT;
    g_carMode.store(kImmersiveDrivingEnabled?loadedCarMode:MODE_DEFAULT);
    g_bikeMode.store(kImmersiveDrivingEnabled?loadedBikeMode:MODE_DEFAULT);
    (void)bicycleMode;
    g_bicycleMode.store(BICYCLE_HANDLEBAR_TRIGGER);
    for (int field=0; field<F_COUNT; ++field)
        g_value[field].store(ClampField(field, values[field]));
    g_wheelVisible.store(visible!=0);
    g_highlights.store(highlights!=0);
    g_bikeHandsFollowTilt.store(bikeHandsFollowTilt!=0);
    g_bikeHorizonLocked.store(bikeLockHorizon!=0);
    g_bikeVisualLeanPercent.store(
        std::clamp(bikeVisualLeanPercent,25,100));
    g_keepRiderOnFlips.store(keepRiderOnFlips!=0);
    g_hideInteriorGlass.store(hideInteriorGlass!=0);
    g_drivebyImmersive.store(drivebyImmersive!=0);
    g_carCameraTilt.store(carCameraTilt!=0);
    g_motionHand.store(std::clamp(motionHand,0,1));
    g_boatCameraTilt.store(boatCameraTilt!=0);
    LOGI("[driving] car=%s bike=%s seat=%+d/%+d/%+d wheel=%+d/%+d/%+d r=%dcm",
         g_carMode.load()==MODE_IMMERSIVE?"IMMERSIVE":"DEFAULT",
         g_bikeMode.load()==MODE_IMMERSIVE?"IMMERSIVE":"DEFAULT",
         g_value[F_SIDE].load(), g_value[F_DISTANCE].load(), g_value[F_HEIGHT].load(),
         g_value[F_WHEEL_SIDE].load(), g_value[F_WHEEL_DISTANCE].load(),
         g_value[F_WHEEL_HEIGHT].load(), g_value[F_WHEEL_RADIUS].load());
    if (configVersion < kDrivingConfigVersion || !bikeModeSeen ||
        !immersiveCarSeatSeen || !immersiveBikeSeatSeen) Save();
    if (!kImmersiveDrivingEnabled &&
        (loadedCarMode==MODE_IMMERSIVE||loadedBikeMode==MODE_IMMERSIVE))
        LOGW("[driving] IMMERSIVE temporarily disabled; using DEFAULT baseline");
}
void EnsureInit() { std::call_once(g_initOnce, Load); }

void ClearGrabStateLocked() {
    g_hornPressed.store(false,std::memory_order_release);
    for (int hand=0; hand<2; ++hand) {
        g_grabbed[hand]=false; g_gripDown[hand]=false;
        g_grabReferenceAngle[hand]=0.0f; g_continuousAngle[hand]=0.0f;
        g_angleValid[hand]=false;
    }
    g_twoHandReferenceAngle=0.0f; g_twoHandContinuousAngle=0.0f;
    g_twoHandAngleValid=false;
    g_bikeOneHandValid=false;
    g_bikeOwnershipMask=0;
    g_bikeReferenceHand={};
    g_bikeSeedChord={};
    g_bikeReferencePhysicalAngle=0.0f;
    g_bikeThrottle=0.0f;
    g_bikeThrottleGestureActive=false;
    g_bikeThrottleReferenceValid=false;
    g_bikeLean=0.0f;
    g_bikeLeanGestureState=0;
    g_bikeLeanReferenceValid[0]=g_bikeLeanReferenceValid[1]=false;
    g_bicycleGestureActive=false;
    g_bicycleSteering=0.0f;
    g_bicyclePedalLevel=0.0f;
    g_bicyclePedalPulse=0.0f;
    g_bicycleTapPhase=0.0f;
    g_bicyclePoseValid=false;
    g_bicyclePreviousTime=0.0;
    g_bicycleTriggerDown=false;
    g_bicycleTriggerPulse=0.0f;
    g_bicycleTriggerPreviousTime=0.0;
}

void UpdateBikeThrottleLocked(const xr::InputState& input,
                              const xr::HandPose& rightPose,
                              bool rightGrabbed) {
    (void)input;
    if (!rightGrabbed||!rightPose.valid) {
        g_bikeThrottle=0.0f;
        g_bikeThrottleGestureActive=false;
        g_bikeThrottleReferenceValid=false;
        return;
    }
    if (!g_bikeThrottleGestureActive) {
        g_bikeThrottleReferenceValid=NormalizeQuaternion(
            rightPose.gripOri,g_bikeThrottleReferenceOrientation);
        g_bikeThrottleGestureActive=g_bikeThrottleReferenceValid;
        g_bikeThrottle=0.0f;
        LOGI("[driving] bike throttle reference captured valid=%d",
             g_bikeThrottleReferenceValid);
        return;
    }
    if (!g_bikeThrottleReferenceValid) { g_bikeThrottle=0.0f; return; }
    float current[4]{};
    if (!NormalizeQuaternion(rightPose.gripOri,current)) {
        g_bikeThrottle=0.0f; return;
    }
    if (DotQuaternion(g_bikeThrottleReferenceOrientation,current)<0.0f)
        for (float& value:current) value=-value;
    const float inverseReference[4]={
        -g_bikeThrottleReferenceOrientation[0],
        -g_bikeThrottleReferenceOrientation[1],
        -g_bikeThrottleReferenceOrientation[2],
         g_bikeThrottleReferenceOrientation[3]};
    float relative[4]{},normalizedRelative[4]{},normalizedTwist[4]{};
    MultiplyQuaternion(inverseReference,current,relative);
    if (!NormalizeQuaternion(relative,normalizedRelative)) {
        g_bikeThrottle=0.0f; return;
    }
    const float twist[4]={0.0f,0.0f,normalizedRelative[2],normalizedRelative[3]};
    if (!NormalizeQuaternion(twist,normalizedTwist)) {
        g_bikeThrottle=0.0f; return;
    }
    const float delta=WrapAngle(2.0f*std::atan2(normalizedTwist[2],
                                                normalizedTwist[3]));
    constexpr float deadZone=1.5f*3.14159265358979323846f/180.0f;
    constexpr float fullThrottle=43.0f*3.14159265358979323846f/180.0f;
    g_bikeThrottle=delta<=deadZone?0.0f:
        std::clamp((delta-deadZone)/(fullThrottle-deadZone),0.0f,1.0f);
}

void UpdateBicycleTriggerCadenceLocked(float trigger,bool active) {
    const double now=MonotonicSeconds();
    if (!active) {
        g_bicycleTriggerDown=false;
        g_bicycleTriggerPulse=0.0f;
        g_bicycleTriggerPreviousTime=0.0;
        return;
    }
    const float dt=g_bicycleTriggerPreviousTime>0.0?
        static_cast<float>(std::clamp(now-g_bicycleTriggerPreviousTime,0.0,0.1)):0.0f;
    g_bicycleTriggerPreviousTime=now;
    // Feed SA a decaying pedal drive rather than a 100 ms digital blip. One
    // press is now strong enough to overcome the bicycle's standing inertia;
    // repeated presses reinforce the drive, so cadence still controls speed.
    g_bicycleTriggerPulse=std::max(0.0f,g_bicycleTriggerPulse-dt*0.60f);
    if (!g_bicycleTriggerDown&&trigger>=0.65f) {
        g_bicycleTriggerDown=true;
        g_bicycleTriggerPulse=std::min(
            1.0f,std::max(0.75f,g_bicycleTriggerPulse+0.30f));
    } else if (g_bicycleTriggerDown&&trigger<=0.30f) {
        g_bicycleTriggerDown=false;
    }
}

void UpdateBikeLeanLocked(const xr::HandPose poses[2], bool left, bool right) {
    // Vice City requires both real hands on the bars. Raising both hands from
    // their captured neutral height requests a wheelie; lowering both requests
    // a stoppie/forward stand. Hysteresis prevents chatter at the threshold.
    if (!left || !right || !poses[0].valid || !poses[1].valid) {
        g_bikeLean=0.0f;
        g_bikeLeanGestureState=0;
        g_bikeLeanReferenceValid[0]=g_bikeLeanReferenceValid[1]=false;
        return;
    }
    bool ready=true;
    for (int hand=0;hand<2;++hand) {
        if (!g_bikeLeanReferenceValid[hand]) {
            g_bikeLeanReferenceY[hand]=poses[hand].gripPos[1];
            g_bikeLeanReferenceValid[hand]=true;
            ready=false;
        }
    }
    if (!ready) { g_bikeLean=0.0f; return; }
    const float height=0.5f*((poses[0].gripPos[1]-g_bikeLeanReferenceY[0])+
                             (poses[1].gripPos[1]-g_bikeLeanReferenceY[1]));
    constexpr float engage=0.20f;
    constexpr float release=0.10f;
    if (g_bikeLeanGestureState==0) {
        if (height>=engage) g_bikeLeanGestureState=-1;
        else if (height<=-engage) g_bikeLeanGestureState=1;
    } else if (g_bikeLeanGestureState<0) {
        if (height<=release) g_bikeLeanGestureState=0;
    } else if (height>=-release) {
        g_bikeLeanGestureState=0;
    }
    g_bikeLean=static_cast<float>(g_bikeLeanGestureState);
}

void UpdateBicycleGestureLocked(const xr::HandPose poses[2]) {
    // PC SA VR bicycle mode: average physical speed of both stage-relative
    // hands becomes pedal effort; HMD yaw relative to the bicycle becomes
    // steering. No synthetic handlebar or grip ownership is involved.
    const double now=MonotonicSeconds();
    const bool valid=poses[0].valid&&poses[1].valid;
    float handSpeed=0.0f;
    double dt=0.0;
    if (valid&&g_bicyclePoseValid&&g_bicyclePreviousTime>0.0) {
        dt=std::clamp(now-g_bicyclePreviousTime,0.001,0.100);
        for (int hand=0;hand<2;++hand) {
            const V3 current{poses[hand].gripPos[0],poses[hand].gripPos[1],
                             poses[hand].gripPos[2]};
            handSpeed+=Length(current-g_bicyclePreviousHand[hand])/
                       static_cast<float>(dt)*0.5f;
        }
    }
    for (int hand=0;hand<2;++hand)
        g_bicyclePreviousHand[hand]={poses[hand].gripPos[0],poses[hand].gripPos[1],
                                     poses[hand].gripPos[2]};
    g_bicyclePreviousTime=now;
    g_bicyclePoseValid=valid;

    constexpr float minimumSpeed=0.30f;
    constexpr float fullSpeed=1.40f;
    const float target=valid?std::clamp(
        (handSpeed-minimumSpeed)/(fullSpeed-minimumSpeed),0.0f,1.0f):0.0f;
    const float alpha=static_cast<float>(std::clamp(dt*5.0,0.0,1.0));
    g_bicyclePedalLevel+=(target-g_bicyclePedalLevel)*alpha;

    float localYaw=0.0f;
    g_bicycleSteering=0.0f;
    if (vrcam::GetLocalHeadYaw(&localYaw)) {
        constexpr float deadZone=0.10f;
        constexpr float fullLock=0.60f;
        const float magnitude=std::clamp(
            (std::abs(localYaw)-deadZone)/(fullLock-deadZone),0.0f,1.0f);
        // CurrentLocalHeadYaw is positive to the player's right; driving's
        // normalized steering uses the same sign.
        g_bicycleSteering=std::copysign(magnitude,localYaw);
    }

    if (g_bicyclePedalLevel>0.02f&&dt>0.0) {
        const float tapsPerSecond=1.5f+6.5f*g_bicyclePedalLevel;
        g_bicycleTapPhase+=static_cast<float>(dt)*tapsPerSecond;
        g_bicycleTapPhase-=std::floor(g_bicycleTapPhase);
        g_bicyclePedalPulse=g_bicycleTapPhase<0.70f?1.0f:0.0f;
    } else {
        g_bicycleTapPhase=0.0f;
        g_bicyclePedalPulse=0.0f;
    }
    g_bicycleGestureActive=true;
}

void ClearInteractionLocked() {
    g_visual = WheelVisualState{};
    ClearGrabStateLocked();
}

void ClearWheelSeatReferenceLocked() {
    g_wheelSeatVehicle=nullptr;
    g_wheelSeatLocal=V3{};
    g_wheelSeatGateLocal=V3{};
    g_wheelSeatHeadLocal=V3{};
    g_wheelSeatHeadValid=false;
    g_wheelSeatArmed=false;
    g_wheelSeatLocalValid=false;
    g_wheelSeatStableSince=0.0;
}

bool ReadEntityMatrix(void* entity, V3* right, V3* forward, V3* up, V3* pos) {
    if (!entity || !right || !forward || !up || !pos) return false;
    const std::uintptr_t matrix=*reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<const char*>(entity)+0x18);
    if (!matrix) return false;
    const float* m=reinterpret_cast<const float*>(matrix);
    *right={m[0],m[1],m[2]}; *forward={m[4],m[5],m[6]};
    *up={m[8],m[9],m[10]}; *pos={m[12],m[13],m[14]};
    if (!std::isfinite(pos->x)||!std::isfinite(pos->y)||!std::isfinite(pos->z)) return false;
    *right=Normalized(*right); *forward=Normalized(*forward); *up=Normalized(*up);
    return Length(*right)>0.5f&&Length(*forward)>0.5f&&Length(*up)>0.5f;
}

bool ReadScaledBikeLeanFrame(void* bike,V3* right,V3* forward,V3* up,V3* pos) {
    if (!bike||!right||!forward||!up||!pos||!g.CBike_CalculateLeanMatrix)
        return false;
    constexpr std::size_t kLeanCalculatedOffset=0x7a8;
    constexpr std::size_t kLeanMatrixOffset=0x7b0;
    constexpr std::size_t kLeanAngleOffset=0x838;
    auto* bytes=reinterpret_cast<char*>(bike);
    auto* calculated=reinterpret_cast<std::uint8_t*>(
        bytes+kLeanCalculatedOffset);
    auto* matrix=reinterpret_cast<float*>(bytes+kLeanMatrixOffset);
    auto* leanAngle=reinterpret_cast<float*>(bytes+kLeanAngleOffset);
    if (!std::isfinite(*leanAngle)) return false;

    const int frameModelId=*reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const char*>(bike)+0x32);
    if (IsBicycleModel(frameModelId)) {
        // Bicycles: PASSIVE READ ONLY. Every write/restore variant tried here
        // (lean 0%, flatten) left the matrix in different states at different
        // points of the deferred frame and the model/hands visibly SHOOK.
        // Read back exactly what the retail code last computed — perfectly
        // consistent with what is drawn — and never touch the bytes.
        *right={matrix[0],matrix[1],matrix[2]};
        *forward={matrix[4],matrix[5],matrix[6]};
        *up={matrix[8],matrix[9],matrix[10]};
        *pos={matrix[12],matrix[13],matrix[14]};
        *right=Normalized(*right);
        *forward=Normalized(*forward);
        *up=Normalized(*up);
        if (Length(*right)>0.5f&&Length(*forward)>0.5f&&
            Length(*up)>0.5f&&std::isfinite(pos->x)&&
            std::isfinite(pos->y)&&std::isfinite(pos->z))
            return true;
        // Matrix not initialised yet (first frames): entity matrix fallback.
        return ReadEntityMatrix(bike,right,forward,up,pos);
    }

    // Calculate a temporary render-only frame. Restore every game-owned byte
    // afterwards: steering, tyre forces and the physical bike simulation keep
    // their stock lean, while the model/camera/hands can use a gentler roll.
    const float leanPercent=
        static_cast<float>(g_bikeVisualLeanPercent.load(
            std::memory_order_acquire));
    const std::uint8_t savedCalculated=*calculated;
    const float savedAngle=*leanAngle;
    float savedMatrix[16]{};
    std::memcpy(savedMatrix,matrix,sizeof(savedMatrix));
    *leanAngle=savedAngle*(leanPercent/100.0f);
    *calculated=0;
    g.CBike_CalculateLeanMatrix(bike);
    *right={matrix[0],matrix[1],matrix[2]};
    *forward={matrix[4],matrix[5],matrix[6]};
    *up={matrix[8],matrix[9],matrix[10]};
    *pos={matrix[12],matrix[13],matrix[14]};
    *leanAngle=savedAngle;
    std::memcpy(matrix,savedMatrix,sizeof(savedMatrix));
    *calculated=savedCalculated;

    *right=Normalized(*right);
    *forward=Normalized(*forward);
    *up=Normalized(*up);
    return Length(*right)>0.5f&&Length(*forward)>0.5f&&Length(*up)>0.5f&&
        std::isfinite(pos->x)&&std::isfinite(pos->y)&&std::isfinite(pos->z);
}

bool WorldPointToTracking(V3 world, V3* tracking) {
    const float source[3]={world.x,world.y,world.z}; float result[3]{};
    if (!tracking || !vrcam::WorldPointToTracking(source,result)) return false;
    *tracking={result[0],result[1],result[2]};
    return std::isfinite(tracking->x)&&std::isfinite(tracking->y)&&std::isfinite(tracking->z);
}

bool IsQuadBikeModelId(int modelId) {
    return (g.CModelInfo_IsQuadBikeModel&&
            g.CModelInfo_IsQuadBikeModel(modelId))||modelId==471;
}

bool GetActivePlayerCar(void** vehicleOut=nullptr,void** pedOut=nullptr) {
    if (vehicleOut) *vehicleOut=nullptr;
    if (pedOut) *pedOut=nullptr;
    if (!g.FindPlayerVehicle||!g.FindPlayerPed||
        !g.CVehicle_GetVehicleAppearance||!g.CVehicle_IsDriver||
        !g.CModelInfo_IsTrainModel) return false;
    void* vehicle=g.FindPlayerVehicle(-1,false);
    void* ped=g.FindPlayerPed(-1);
    if (!vehicle||!ped) return false;
    // Android SA's exported eVehicleAppearance matches the reversed enum:
    // NONE=0, AUTOMOBILE=1, BIKE=2, HELI=3, BOAT=4, PLANE=5.
    constexpr int kAutomobileAppearance=1;
    const int appearance=g.CVehicle_GetVehicleAppearance(vehicle);
    const bool isDriver=g.CVehicle_IsDriver(vehicle,ped);
    // Trains carry no bike/boat/air handling flag and therefore report the
    // generic AUTOMOBILE appearance too. CEntity::SetModelIndexNoCreate in the
    // shipped 2.11 arm64 binary stores the uint16 model id at +0x32; combine
    // that verified field with the exported model predicate to keep trains out.
    const int modelId=*reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const char*>(vehicle)+0x32);
    const bool train=g.CModelInfo_IsTrainModel(modelId);
    // The QUAD reports the generic AUTOMOBILE appearance on 2.11 but is a
    // CQuadBike underneath — it should ride like a bike eventually. Until that
    // mapping exists it stays on plain stick controls: no virtual car wheel.
    if (IsQuadBikeModelId(modelId)) return false;
    // Boats and planes share the car-style wheel path: helm and yoke are both
    // steering wheels, and the frame builder + grab/steer solver are
    // appearance-agnostic (the plane adds an axial pull for pitch).
    constexpr int kBoatAppearance=4;
    constexpr int kPlaneAppearance=5;
    if ((appearance!=kAutomobileAppearance&&appearance!=kBoatAppearance&&
         appearance!=kPlaneAppearance)||
        !isDriver||train) {
        // Some mission vehicles unexpectedly fail these gates. Name the gate
        // once per model so a device log capture pins the exact cause.
        static int loggedRejectModel=-1;
        if (appearance!=2&&loggedRejectModel!=modelId) {
            LOGI("[driving] car gate reject model=%d appearance=%d driver=%d train=%d",
                 modelId,appearance,isDriver?1:0,train?1:0);
            loggedRejectModel=modelId;
        }
        return false;
    }
    if (vehicleOut) *vehicleOut=vehicle;
    if (pedOut) *pedOut=ped;
    return true;
}

bool GetActivePlayerBike(void** vehicleOut=nullptr,void** pedOut=nullptr) {
    if (vehicleOut) *vehicleOut=nullptr;
    if (pedOut) *pedOut=nullptr;
    if (!g.FindPlayerVehicle||!g.FindPlayerPed||
        !g.CVehicle_GetVehicleAppearance||!g.CVehicle_IsDriver) return false;
    void* vehicle=g.FindPlayerVehicle(-1,false);
    void* ped=g.FindPlayerPed(-1);
    if (!vehicle||!ped) return false;
    constexpr int kBikeAppearance=2;
    if (g.CVehicle_GetVehicleAppearance(vehicle)!=kBikeAppearance||
        !g.CVehicle_IsDriver(vehicle,ped)) return false;
    const int modelId=*reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const char*>(vehicle)+0x32);
    // QUAD can carry the bike handling flag while its object is actually an
    // Automobile-derived CQuadBike with a different node layout. Do not call
    // CBike node methods on it; it remains on ordinary stick controls for now.
    if ((g.CModelInfo_IsQuadBikeModel&&g.CModelInfo_IsQuadBikeModel(modelId))||
        modelId==471) return false;
    if (vehicleOut) *vehicleOut=vehicle;
    if (pedOut) *pedOut=ped;
    return true;
}

int GetActiveVehicleAppearance(bool requireDriver=true) {
    if (!g.FindPlayerVehicle||!g.CVehicle_GetVehicleAppearance) return 0;
    void* vehicle=g.FindPlayerVehicle(-1,false);
    if (!vehicle) return 0;
    if (requireDriver&&g.FindPlayerPed&&g.CVehicle_IsDriver) {
        void* ped=g.FindPlayerPed(-1);
        if (!ped||!g.CVehicle_IsDriver(vehicle,ped)) return 0;
    }
    const int appearance=g.CVehicle_GetVehicleAppearance(vehicle);
    if (appearance==2) {
        const int modelId=*reinterpret_cast<const std::uint16_t*>(
            reinterpret_cast<const char*>(vehicle)+0x32);
        if ((g.CModelInfo_IsQuadBikeModel&&
             g.CModelInfo_IsQuadBikeModel(modelId))||modelId==471)
            return 0;
    }
    return appearance;
}

int ModeForAppearance(int appearance) {
    // Third-person chase view implies plain stick driving: the immersive
    // wheel/hands live in the cockpit the camera just left. The Rhino tank is
    // always chase view (no cockpit sight lines).
    if (appearance==1) {
        const int model=g_activeVehicleModel.load(std::memory_order_acquire);
        // Rhino: always chase view. Quad: blocked from immersive until it is
        // mapped onto the bike handlebar controls.
        if (model==kRhinoModelId||IsQuadBikeModelId(model))
            return MODE_DEFAULT;
        const int carMode=g_carMode.load();
        if (g_carView.load()==VIEW_THIRD_PERSON&&carMode==MODE_IMMERSIVE)
            return MODE_DEFAULT;   // cockpit mode needs the cockpit camera
        return carMode;
    }
    if (appearance==2) {
        const int bikeMode=g_bikeMode.load();
        if (g_bikeView.load()==VIEW_THIRD_PERSON&&bikeMode==MODE_IMMERSIVE)
            return MODE_DEFAULT;
        return bikeMode;
    }
    if (appearance==4) {
        if (g_boatView.load()==VIEW_THIRD_PERSON) return MODE_DEFAULT;
        return g_boatMode.load();
    }
    if (appearance==5) {
        // Fixed-wing only; helicopters (3) stay DEFAULT below.
        if (g_airView.load()==VIEW_THIRD_PERSON) return MODE_DEFAULT;
        return g_planeMode.load();
    }
    return MODE_DEFAULT;
}

bool BuildWheelTracking(WheelVisualState* visual) {
    if (!visual) return false;
    void* vehicle=nullptr; void* ped=nullptr;
    if (!GetActivePlayerCar(&vehicle,&ped)) return false;
    const int modelId=*reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const char*>(vehicle)+0x32);
    V3 vr,vf,vu,vp;
    if (!ReadEntityMatrix(vehicle,&vr,&vf,&vu,&vp)) return false;
    // CPed::m_matrix is commonly null while seated on Android. reference Quest build needs the
    // player's world position only, so use the exported FindPlayerCoors HFA
    // instead of making the entire wheel depend on that optional ped matrix.
    V3 pp = vp + vu * 0.65f;
    if (g.FindPlayerCoors) {
        const GameSymbols::Vec3 player = g.FindPlayerCoors(-1);
        const V3 candidate{player.x, player.y, player.z};
        if (std::isfinite(candidate.x) && std::isfinite(candidate.y) &&
            std::isfinite(candidate.z) && Length(candidate - vp) < 10.0f) {
            pp = candidate;
        }
    }
    // The raw root-derived seat point, kept for exit detection below: it is
    // rigidly animated by the game (the attach point), so it is the stable
    // "is the ped still on this seat" signal regardless of anchor overrides.
    const V3 rootPp = pp;
    // Plane cockpits sit metres from both the model origin and (on this
    // mobile build) the seated ped coords — the yoke rendered five metres
    // behind the pilot. Anchor on the live pilot HEAD BONE exactly like the
    // camera does, stepped down to the shared waist baseline so the global
    // F_WHEEL offsets keep their car meaning.
    const bool planeYoke = g.CVehicle_GetVehicleAppearance &&
        g.CVehicle_GetVehicleAppearance(vehicle) == 5;
    visual->yoke = planeYoke;
    if (planeYoke && g.CPed_GetBonePosition) {
        GameSymbols::Vec3 headBone{};
        g.CPed_GetBonePosition(ped, &headBone, 5 /* BONE_HEAD */, true);
        const V3 head{headBone.x, headBone.y, headBone.z};
        if (std::isfinite(head.x) && std::isfinite(head.y) &&
            std::isfinite(head.z) && Length(head - vp) < 30.0f) {
            pp = head - vu * 0.55f;
        }
    }
    // Tall/long cabs (Monster, Dozer, Tanker): FindPlayerCoors returns the
    // ped ROOT, which on these models sits a metre below (Monster) or 1.3m
    // behind (Tanker) the actual driver — the wheel was built at the floor or
    // in the trailer. The live head bone knows where the driver really is,
    // but it also leans with the driving animation, so no per-frame decision
    // is safe (an instantaneous 3D threshold falsely fired on the Infernus'
    // forward lean and displaced its calibrated wheel). Instead the head
    // candidate is only CACHED here; the anchor source is chosen once at
    // arming time below, from the settled seated pose.
    V3 headCandidate{};
    bool headCandidateValid = false;
    if (!planeYoke && g.CPed_GetBonePosition) {
        GameSymbols::Vec3 headBone{};
        g.CPed_GetBonePosition(ped, &headBone, 5 /* BONE_HEAD */, true);
        const V3 head{headBone.x, headBone.y, headBone.z};
        if (std::isfinite(head.x) && std::isfinite(head.y) &&
            std::isfinite(head.z) && Length(head - vp) < 30.0f) {
            headCandidate = head - vu * 0.60f;
            headCandidateValid = true;
        }
    }
    // A finite value is not necessarily a usable tracking transform.  During a
    // cold aircraft load WorldPointToTracking used to briefly pass game-world
    // coordinates through unchanged (hundreds or thousands of metres).  That
    // one bad frame then became the permanent seat reference below.  Accept a
    // plane anchor only when it is actually close to the live OpenXR head.
    if (planeYoke) {
        V3 seatTracking{};
        float headPosition[3]{},headOrientation[4]{};
        const bool headValid=xr::GetHeadPose(headPosition,headOrientation);
        const V3 trackingHead{headPosition[0],headPosition[1],headPosition[2]};
        const bool seatValid=WorldPointToTracking(pp,&seatTracking);
        const float trackingDistance=(headValid&&seatValid)
            ? Length(seatTracking-trackingHead):1000.0f;
        if (!headValid||!seatValid||!std::isfinite(trackingDistance)||
            trackingDistance>2.0f) {
            static unsigned int deferredLog=0;
            if ((++deferredLog%180u)==1u)
                LOGI("[driving] aircraft wheel deferred: tracking seat %.2fm from head",
                     static_cast<double>(trackingDistance));
            return false;
        }
    }
    // Hide the wheel (and its dash-HUD anchor) the moment the exit animation
    // pulls the ped off the seat: the anchor follows the ped, so the wheel
    // used to climb out of the aircraft with the player.
    {
        const V3 seatDelta=pp-vp;
        const V3 local{Dot(seatDelta,vr),Dot(seatDelta,vf),
                       Dot(seatDelta,vu)};
        const V3 gateDelta=rootPp-vp;
        const V3 gateLocal{Dot(gateDelta,vr),Dot(gateDelta,vf),
                           Dot(gateDelta,vu)};
        const double nowSeat=[]{
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC,&ts);
            return static_cast<double>(ts.tv_sec)+ts.tv_nsec*1e-9;
        }();
        if (g_wheelSeatVehicle!=vehicle) {
            ClearWheelSeatReferenceLocked();
            g_wheelSeatVehicle=vehicle;
            g_wheelSeatEnteredAt=nowSeat;
        }
        // Keep re-seeding through the whole climb-in; arm as soon as the
        // ROOT point stops moving for half a second (the climb-in animation
        // has finished), with the original 3s as the ceiling. The wait was
        // user-visible on tall cabs: their wheel only appears in the right
        // place once armed.
        bool seeding=!g_wheelSeatLocalValid||nowSeat-g_wheelSeatEnteredAt<3.0;
        if (seeding&&g_wheelSeatLocalValid) {
            if (Length(gateLocal-g_wheelSeatGateLocal)<0.03f) {
                if (g_wheelSeatStableSince<=0.0)
                    g_wheelSeatStableSince=nowSeat;
                else if (nowSeat-g_wheelSeatStableSince>0.5)
                    seeding=false;
            } else {
                g_wheelSeatStableSince=0.0;
            }
        }
        if (seeding) {
            g_wheelSeatLocal=local;
            g_wheelSeatGateLocal=gateLocal;
            if (headCandidateValid) {
                const V3 headDelta=headCandidate-vp;
                g_wheelSeatHeadLocal=V3{Dot(headDelta,vr),Dot(headDelta,vf),
                                        Dot(headDelta,vu)};
                g_wheelSeatHeadValid=true;
            }
            g_wheelSeatLocalValid=true;
        }
        else {
            if (!g_wheelSeatArmed) {
                g_wheelSeatArmed=true;
                // One-time anchor-source decision from the settled pose:
                // trust the head bone only when the root is clearly not at
                // the seat — well below it (tall cab) or metres away (cab
                // far from the model origin). The Infernus' seated forward
                // lean (~0.4m horizontal, ~0.1m vertical) stays on the
                // calibrated root anchor.
                if (g_wheelSeatHeadValid) {
                    const V3 delta=g_wheelSeatHeadLocal-g_wheelSeatLocal;
                    const float vertical=std::abs(delta.z);
                    const float total=Length(delta);
                    if (vertical>0.45f||total>1.0f) {
                        LOGI("[driving] wheel anchor: head-bone seat vertical=%.2f total=%.2f",
                             vertical,total);
                        g_wheelSeatLocal=g_wheelSeatHeadLocal;
                    }
                }
            }
            // Exit detection compares the ROOT-derived point against its own
            // reference: an anchor-source flip (root vs head bone) between
            // frames is invisible here and cannot fake a 2m "exit".
            const V3 drift=gateLocal-g_wheelSeatGateLocal;
            // 0.30m gate with a much slower reference leak: the previous
            // 0.05/frame leak CHASED the exit slide and delayed the hide —
            // the yoke visibly followed the pilot out before vanishing.
            if (Length(drift)>0.30f) {
                static unsigned int hiddenLog=0;
                if ((++hiddenLog%90u)==1u)
                    LOGI("[driving] wheel hidden: seat drift %.2fm",
                         Length(drift));
                return false;
            }
            // NO reference leak once armed: the cached point is now the
            // anchor itself, and even a 0.01/frame chase converged on the
            // animation sway within seconds (72fps), so the wheel still slid
            // while steering. The 3s re-seed window covers seat settling; a
            // real seat change re-enters through the vehicle-switch reset.
            // Once armed, the wheel anchors on the CACHED seat point rigidly
            // attached to the vehicle frame, not the live ped/head sample:
            // the steering/lean animation moves the head bone and the whole
            // wheel would slide side to side with it.
            pp=vp+vr*g_wheelSeatLocal.x+vf*g_wheelSeatLocal.y+
                vu*g_wheelSeatLocal.z;
        }
    }
    // Per-model wheel calibration (VC parity): position deltas in the vehicle
    // frame, radius delta, and pitch/yaw/roll of the wheel plane itself.
    int wheelCal[WHEEL_CAL_FIELD_COUNT]{};
    {
        std::lock_guard<std::mutex> modelLock(g_modelMutex);
        const auto found=g_modelCalibration.find(modelId);
        if (found!=g_modelCalibration.end())
            std::memcpy(wheelCal,found->second.wheel,sizeof(wheelCal));
    }
    const V3 worldCenter=pp+
        vr*((static_cast<float>(g_value[F_WHEEL_SIDE].load())+
             wheelCal[WHEEL_CAL_SIDE])/100.0f)+
        vf*((static_cast<float>(g_value[F_WHEEL_DISTANCE].load())+
             wheelCal[WHEEL_CAL_FORWARD])/100.0f)+
        vu*((static_cast<float>(g_value[F_WHEEL_HEIGHT].load())+
             wheelCal[WHEEL_CAL_HEIGHT])/100.0f);
    V3 wheelAxisRight=vr,wheelAxisUp=vu,wheelAxisNormal=vf;
    {
        constexpr float halfDegToRad=3.14159265358979323846f/360.0f;
        const float pitch=wheelCal[WHEEL_CAL_PITCH]*halfDegToRad;
        const float yaw=wheelCal[WHEEL_CAL_YAW]*halfDegToRad;
        const float roll=wheelCal[WHEEL_CAL_ROLL]*halfDegToRad;
        if (pitch!=0.0f) {
            wheelAxisUp=RotateAroundAxis(wheelAxisUp,wheelAxisRight,pitch);
            wheelAxisNormal=RotateAroundAxis(wheelAxisNormal,wheelAxisRight,pitch);
        }
        if (yaw!=0.0f) {
            wheelAxisRight=RotateAroundAxis(wheelAxisRight,wheelAxisUp,yaw);
            wheelAxisNormal=RotateAroundAxis(wheelAxisNormal,wheelAxisUp,yaw);
        }
        if (roll!=0.0f) {
            wheelAxisRight=RotateAroundAxis(wheelAxisRight,wheelAxisNormal,roll);
            wheelAxisUp=RotateAroundAxis(wheelAxisUp,wheelAxisNormal,roll);
        }
    }
    V3 center,pointRight,pointUp,pointNormal;
    if (!WorldPointToTracking(worldCenter,&center)||
        !WorldPointToTracking(worldCenter+wheelAxisRight,&pointRight)||
        !WorldPointToTracking(worldCenter+wheelAxisUp,&pointUp)||
        !WorldPointToTracking(worldCenter+wheelAxisNormal,&pointNormal)) return false;
    V3 normal=Normalized(pointNormal-center);
    V3 right=pointRight-center; right=Normalized(right-normal*Dot(right,normal));
    V3 up=Normalized(Cross(right,normal));
    if (Dot(up,Normalized(pointUp-center))<0.0f) { right=right*-1.0f; up=up*-1.0f; }
    if (Length(right)<0.5f||Length(up)<0.5f||Length(normal)<0.5f) return false;
    visual->active=true; visual->visible=g_wheelVisible.load();
    visual->dashAnchorValid=true;
    visual->modelId=modelId;
    visual->center[0]=center.x; visual->center[1]=center.y; visual->center[2]=center.z;
    visual->right[0]=right.x; visual->right[1]=right.y; visual->right[2]=right.z;
    visual->up[0]=up.x; visual->up[1]=up.y; visual->up[2]=up.z;
    visual->normal[0]=normal.x; visual->normal[1]=normal.y; visual->normal[2]=normal.z;
    visual->radius=std::clamp(
        (static_cast<float>(g_value[F_WHEEL_RADIUS].load())+
         wheelCal[WHEEL_CAL_RADIUS])/100.0f, 0.06f, 0.45f);
    static int loggedModel=-1;
    if (loggedModel!=modelId) {
        LOGI("[driving] immersive wheel ready center=(%.2f,%.2f,%.2f) radius=%.2f visible=%d grips=%d",
             center.x,center.y,center.z,visual->radius,visual->visible,
             g_highlights.load());
        loggedModel=modelId;
    }
    return true;
}

bool BuildBikeTracking(WheelVisualState* visual) {
    if (!visual) return false;
    void* vehicle=nullptr;
    if (!GetActivePlayerBike(&vehicle,nullptr)) return false;
    V3 vr,vf,vu,vp;
    // SA renders a bike's visual lean from CBike::m_mLeanMatrix, not from the
    // ordinary CEntity matrix. Reading the latter made pinned hands look frozen
    // while the rendered motorcycle rolled underneath them. The 2.11 arm64
    // layout is verified by CalculateLeanMatrix: flag +0x7a8, matrix +0x7b0.
    if (!ReadScaledBikeLeanFrame(vehicle,&vr,&vf,&vu,&vp))
        return false;
    const int modelId=*reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const char*>(vehicle)+0x32);
    // BIKE_HANDLEBARS=7 on motorcycles, but bicycles fill the same node array
    // from the BMX enum where HANDLEBARS=6 and slot 7 is the CHAINSET (the
    // pedal crank!). Anchoring hands to the crank is what threw them around
    // during wheelies and pedalling sway on BMX/Bike/MTB.
    const int handleNode=IsBicycleModel(modelId)?6:7;
    V3 worldCenter=vp+
        vf*(static_cast<float>(g_value[F_BIKE_DISTANCE].load())/100.0f)+
        vu*(static_cast<float>(g_value[F_BIKE_HEIGHT].load())/100.0f);
    bool usedModelHandle=false;
    if (g.CBike_IsComponentPresent&&g.CBike_GetComponentWorldPosition&&
        g.CBike_IsComponentPresent(vehicle,handleNode)) {
        GameSymbols::Vec3 node{};
        g.CBike_GetComponentWorldPosition(vehicle,handleNode,&node);
        const V3 candidate{node.x,node.y,node.z};
        if (std::isfinite(candidate.x)&&std::isfinite(candidate.y)&&
            std::isfinite(candidate.z)&&Length(candidate-vp)<5.0f) {
            // Keep the real node live: its suspension/steering translation can
            // vary by model and frame. Apply calibration in the lean frame.
            worldCenter=candidate+
                vf*(static_cast<float>(g_value[F_BIKE_DISTANCE].load()-30)/100.0f)+
                vu*(static_cast<float>(g_value[F_BIKE_HEIGHT].load()-82)/100.0f);
            usedModelHandle=true;
        }
    }
    V3 center,pointRight,pointForward,pointUp;
    if (!WorldPointToTracking(worldCenter,&center)||
        !WorldPointToTracking(worldCenter+vr,&pointRight)||
        !WorldPointToTracking(worldCenter+vf,&pointForward)||
        !WorldPointToTracking(worldCenter+vu,&pointUp)) return false;
    V3 normal=Normalized(pointUp-center);
    V3 right=pointRight-center;
    right=Normalized(right-normal*Dot(right,normal));
    V3 forward=pointForward-center;
    forward=Normalized(forward-normal*Dot(forward,normal)-right*Dot(forward,right));
    if (Dot(Cross(right,forward),normal)<0.0f) forward=forward*-1.0f;
    if (Length(right)<0.5f||Length(forward)<0.5f||Length(normal)<0.5f) return false;
    visual->active=true;
    visual->dashAnchorValid=true;
    // This switch belongs only to the synthetic car wheel in current VC. The
    // bike needs its short bar surrogate because the stock handlebar model is
    // not part of our late OpenXR hand pass.
    visual->visible=true;
    visual->bike=true;
    visual->bikeHandsFollowTilt=g_bikeHandsFollowTilt.load();
    visual->modelId=modelId;
    visual->center[0]=center.x; visual->center[1]=center.y; visual->center[2]=center.z;
    visual->right[0]=right.x; visual->right[1]=right.y; visual->right[2]=right.z;
    // On a bike this is the second axis of the horizontal steering plane.
    visual->up[0]=forward.x; visual->up[1]=forward.y; visual->up[2]=forward.z;
    visual->normal[0]=normal.x; visual->normal[1]=normal.y; visual->normal[2]=normal.z;
    visual->radius=static_cast<float>(g_value[F_BIKE_HALF_WIDTH].load())/100.0f;
    // Android 2.11 arm64 CPhysical::GetSpeed reads m_vecMoveSpeed at +0x68.
    // CBike's constructor stores tHandlingData* at +0x4a8; the converted
    // cTransmission::m_MaxFlatVelocity is at +0x88 in that handling record.
    // Drive the held-R2 wrist animation from actual road speed, not trigger
    // pressure, so coasting and deceleration remain visually believable.
    if (!IsBicycleModel(modelId) &&
        g_activeBikeAccelerator.load(std::memory_order_acquire)==
            BIKE_ACCEL_HOLD_TRIGGER) {
        constexpr std::size_t kMoveSpeedOffset=0x68;
        constexpr std::size_t kHandlingOffset=0x4a8;
        constexpr std::size_t kMaxFlatVelocityOffset=0x88;
        const auto* vehicleBytes=reinterpret_cast<const std::uint8_t*>(vehicle);
        const V3 velocity=*reinterpret_cast<const V3*>(
            vehicleBytes+kMoveSpeedOffset);
        const void* handling=*reinterpret_cast<void* const*>(
            vehicleBytes+kHandlingOffset);
        const float maximum=handling?
            *reinterpret_cast<const float*>(
                reinterpret_cast<const std::uint8_t*>(handling)+
                kMaxFlatVelocityOffset):0.0f;
        const float roadSpeed=std::sqrt(std::max(
            0.0f,velocity.x*velocity.x+velocity.y*velocity.y));
        if (std::isfinite(roadSpeed)&&std::isfinite(maximum)&&
            maximum>0.01f&&maximum<10.0f) {
            visual->throttleVisual=std::clamp(roadSpeed/maximum,0.0f,1.0f);
        }
    }
    static int loggedModel=-1;
    if (loggedModel!=modelId) {
        LOGI("[driving] immersive bike bars ready model=%d node=%d source=%s center=(%.2f,%.2f,%.2f) halfWidth=%.2f",
             modelId,handleNode,usedModelHandle?"HANDLE_NODE":"LEAN_FALLBACK",
             center.x,center.y,center.z,visual->radius);
        loggedModel=modelId;
    }
    return true;
}

void FillHandleVisuals(WheelVisualState* visual) {
    const V3 center{visual->center[0],visual->center[1],visual->center[2]};
    const V3 baseRight{visual->right[0],visual->right[1],visual->right[2]};
    const V3 baseUp{visual->up[0],visual->up[1],visual->up[2]};
    const V3 normal{visual->normal[0],visual->normal[1],visual->normal[2]};
    const float visualAngle=
        visual->bike ? visual->physicalAngle : -visual->physicalAngle;
    const V3 wheelRight=Normalized(RotateAroundAxis(baseRight,normal,visualAngle));
    const V3 wheelUp=Normalized(RotateAroundAxis(baseUp,normal,visualAngle));
    ModelCalibration calibration{};
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        const auto found=g_modelCalibration.find(visual->modelId);
        if (found!=g_modelCalibration.end()) calibration=found->second;
    }
    for (int hand=0; hand<2; ++hand) {
        V3 pos=center+wheelRight*((hand==0?-1.0f:1.0f)*visual->radius);
        V3 handleRight=wheelRight,handleUp=wheelUp,handleForward=normal;
        if (visual->bike) {
            // Match the Quest VC handlebar grip frame: palms face inward and
            // both hands point along the turned bike heading.
            handleRight=normal*(hand==0?-1.0f:1.0f);
            handleForward=wheelUp;
            handleUp=Normalized(Cross(handleRight,handleForward));
        }
        // Per-model values are live calibration corrections in the same half-
        // centimetre / half-degree scale as current Quest Vice City. Position
        // corrections stay in the turned vehicle control frame, then rotations
        // refine the actual wrist frame shown by the pinned calibration hand.
        const int* value=calibration.control[hand];
        pos=pos+wheelRight*(static_cast<float>(value[CONTROL_OFFSET_X])/200.0f)+
                wheelUp*(static_cast<float>(value[CONTROL_OFFSET_Y])/200.0f)+
                normal*(static_cast<float>(value[CONTROL_OFFSET_Z])/200.0f);
        constexpr float halfDegToRad=
            3.14159265358979323846f/360.0f;
        const float pitch=value[CONTROL_ROT_X]*halfDegToRad;
        const float yaw=value[CONTROL_ROT_Y]*halfDegToRad;
        const float roll=value[CONTROL_ROT_Z]*halfDegToRad;
        if (pitch!=0.0f) {
            handleUp=RotateAroundAxis(handleUp,handleRight,pitch);
            handleForward=RotateAroundAxis(handleForward,handleRight,pitch);
        }
        if (yaw!=0.0f) {
            handleRight=RotateAroundAxis(handleRight,handleUp,yaw);
            handleForward=RotateAroundAxis(handleForward,handleUp,yaw);
        }
        if (roll!=0.0f) {
            handleRight=RotateAroundAxis(handleRight,handleForward,roll);
            handleUp=RotateAroundAxis(handleUp,handleForward,roll);
        }
        if (visual->bike&&hand==1&&visual->throttleVisual>0.0f) {
            // The grip/bar axis is handleUp. At the handling top speed the
            // pinned right wrist reaches the same 43-degree travel used by the
            // physical twist-throttle solver. This changes only the rendered
            // socket basis; OnGetAccelerate still receives ordinary held R2.
            constexpr float fullThrottleTwist=
                43.0f*3.14159265358979323846f/180.0f;
            const float angle=std::clamp(
                visual->throttleVisual,0.0f,1.0f)*-fullThrottleTwist;
            const V3 restRight=handleRight;
            const V3 restForward=handleForward;
            handleRight=RotateAroundAxis(handleRight,handleUp,angle);
            handleForward=RotateAroundAxis(handleForward,handleUp,angle);
            // BigHandRight's shipped closed-pose palm centre is 2.8 cm along
            // the rendered forward axis and 1.7 cm below the socket. Rotating
            // only the basis makes that centre visibly arc downward. Translate
            // the socket by the opposite arc so the palm remains planted on
            // the throttle while its wrist still shows the speed-based twist.
            constexpr float kPalmForward=0.028f;
            constexpr float kPalmBelow=0.017f;
            const V3 restPalm=restForward*kPalmForward-restRight*kPalmBelow;
            const V3 twistedPalm=handleForward*kPalmForward-handleRight*kPalmBelow;
            pos=pos+restPalm-twistedPalm;
        }
        handleRight=Normalized(handleRight);
        handleUp=Normalized(handleUp);
        handleForward=Normalized(handleForward);
        const V3 axes[4]={pos,handleRight,handleUp,handleForward};
        float* out[4]={visual->handlePosition[hand],visual->handleRight[hand],
                       visual->handleUp[hand],visual->handleForward[hand]};
        for (int a=0;a<4;++a) { out[a][0]=axes[a].x; out[a][1]=axes[a].y; out[a][2]=axes[a].z; }
    }
}

// Android SA 2.11 polls touch widgets inside these queries. Hook the queries,
// rather than trusting the Java gamepad provider, so Quest input is authoritative.
using AxisQueryFn=int(*)(void*);
using SprintFn=bool(*)(void*,int);
using ButtonQueryFn=bool(*)(void*);
using BikePreRenderFn=void(*)(void*);
using KnockOffAffectsPedFn=bool(*)(const void*,void*);
AxisQueryFn g_origAccelerate{},g_origBrake{},g_origSteering{},
            g_origSteeringUpDown{},g_origHandBrake{};
SprintFn g_origSprint{};
ButtonQueryFn g_origNextStation{};
ButtonQueryFn g_origLastStation{};
BikePreRenderFn g_origBikePreRender{};
KnockOffAffectsPedFn g_origKnockOffAffectsPed{};

bool OnKnockOffBikeAffectsPed(const void* event,void* ped) {
    if (event&&ped&&g_keepRiderOnFlips.load(std::memory_order_acquire)&&
        g.FindPlayerPed&&g.FindPlayerVehicle&&g.CVehicle_IsDriver) {
        void* player=g.FindPlayerPed(-1);
        void* vehicle=g.FindPlayerVehicle(-1,false);
        const auto* bytes=reinterpret_cast<const std::uint8_t*>(event);
        void* eventVehicle=*reinterpret_cast<void* const*>(bytes+0x48);
        const std::uint8_t knockOffType=*(bytes+0x42);
        // Retail SA types 53/54 are the ordinary fall and skid-back fall used
        // when a bike overturns. Collision, impact and explosion types still
        // pass through so the option does not turn the rider invincible.
        if (ped==player&&vehicle&&eventVehicle==vehicle&&
            g.CVehicle_IsDriver(vehicle,ped)&&
            (knockOffType==53u||knockOffType==54u)) {
            return false;
        }
    }
    return g_origKnockOffAffectsPed?
        g_origKnockOffAffectsPed(event,ped):true;
}

BikePreRenderFn g_origBmxPreRender=nullptr;
void BikePreRenderCommon(void* bike, BikePreRenderFn orig);
void OnBikePreRenderThunk(void* bike) {
    BikePreRenderCommon(bike, g_origBikePreRender);
}
// Bicycles: CBmx::PreRender calls CVehicle::PreRender directly and never
// CBike::PreRender, so the handlebar-rotation hook below never ran for them —
// that is why the pedal bike's bar stayed straight while motorcycles turned.
void OnBmxPreRenderThunk(void* bike) {
    BikePreRenderCommon(bike, g_origBmxPreRender);
}

void BikePreRenderCommon(void* bike, BikePreRenderFn origPreRender) {
    constexpr std::size_t kLeanCalculatedOffset=0x7a8;
    constexpr std::size_t kLeanMatrixOffset=0x7b0;
    constexpr std::size_t kLeanAngleOffset=0x838;
    float savedAngle=0.0f;
    float savedMatrix[16]{};
    std::uint8_t savedCalculated=0;
    bool scaled=false;
    bool activePlayerBike=false;
    if (bike) {
        void* playerBike=nullptr;
        const bool havePlayerBike=GetActivePlayerBike(&playerBike,nullptr);
        std::lock_guard<std::mutex> lock(g_stateMutex);
        const int model=*reinterpret_cast<const std::uint16_t*>(
            reinterpret_cast<const char*>(bike)+0x32);
        activePlayerBike=g_visual.active&&g_visual.bike&&
            g_visual.modelId==model&&havePlayerBike&&playerBike==bike;
    }
    if (bike&&origPreRender) {
        auto* bytes=reinterpret_cast<char*>(bike);
        auto* angle=reinterpret_cast<float*>(bytes+kLeanAngleOffset);
        auto* matrix=reinterpret_cast<float*>(bytes+kLeanMatrixOffset);
        auto* calculated=reinterpret_cast<std::uint8_t*>(
            bytes+kLeanCalculatedOffset);
        if (activePlayerBike&&std::isfinite(*angle)) {
            const int leanModel=*reinterpret_cast<const std::uint16_t*>(
                reinterpret_cast<const char*>(bike)+0x32);
            savedAngle=*angle;
            savedCalculated=*calculated;
            std::memcpy(savedMatrix,matrix,sizeof(savedMatrix));
            if (!IsBicycleModel(leanModel)) {
                *angle=savedAngle*(static_cast<float>(
                    g_bikeVisualLeanPercent.load(
                        std::memory_order_acquire))/100.0f);
                *calculated=0;
                scaled=true;
            }
            // Bicycles: hands follow the retail frame verbatim; no writes.
        }
        origPreRender(bike);
        if (scaled) {
            *angle=savedAngle;
            std::memcpy(matrix,savedMatrix,sizeof(savedMatrix));
            *calculated=savedCalculated;
        }
    } else if (origPreRender) {
        origPreRender(bike);
    }
    if (!bike||!activePlayerBike||!g.RwFrameUpdateObjects) return;
    float angle=0.0f;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        const int model=*reinterpret_cast<const std::uint16_t*>(
            reinterpret_cast<const char*>(bike)+0x32);
        if (!g_visual.active||!g_visual.bike||g_visual.modelId!=model) return;
        angle=g_visual.physicalAngle;
        // Bicycles in MOTION control mode steer through g_bicycleSteering and
        // never touch the physical bar solver — the visual bar sat straight.
        // Derive the render angle from the same value the game steers with.
        if (IsBicycleModel(model)&&std::abs(angle)<1.0e-4f&&
            g_bicycleMode.load(std::memory_order_acquire)==BICYCLE_MOTION)
            angle=-std::clamp(g_bicycleSteering,-1.0f,1.0f)*0.55f;
    }
    if (!std::isfinite(angle)||std::abs(angle)<1.0e-5f) return;
    constexpr std::size_t kBikeNodesOffset=0x758;
    const int model=*reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const char*>(bike)+0x32);
    // Motorcycles: BIKE_HANDLEBARS=7. Bicycles use the BMX node enum where
    // HANDLEBARS=6 — slot 7 is the pedal CHAINSET, which is why "the bar never
    // turned" on BMX/Bike/MTB: we were silently rotating the crank instead.
    const int handlebarNode=IsBicycleModel(model)?6:7;
    void* frame=*reinterpret_cast<void**>(
        reinterpret_cast<char*>(bike)+kBikeNodesOffset+
        handlebarNode*sizeof(void*));
    // Diagnostics: name the model once so a log capture shows whether the
    // rotation path runs and whether the handlebar node exists.
    {
        static int loggedRotateModel=-1;
        if (loggedRotateModel!=model) {
            loggedRotateModel=model;
            LOGI("[driving] handlebar rotate model=%d node=%d frame=%p angle=%.2f",
                 model,handlebarNode,frame,angle);
        }
    }
    if (!frame) return;
    float* matrix=reinterpret_cast<float*>(
        reinterpret_cast<char*>(frame)+0x20); // RwFrame local matrix
    V3 right{matrix[0],matrix[1],matrix[2]};
    V3 forward{matrix[4],matrix[5],matrix[6]};
    V3 up{matrix[8],matrix[9],matrix[10]};
    if (Length(right)<0.5f||Length(forward)<0.5f||Length(up)<0.5f) return;
    right=RotateAroundAxis(right,Normalized(up),angle);
    forward=RotateAroundAxis(forward,Normalized(up),angle);
    matrix[0]=right.x; matrix[1]=right.y; matrix[2]=right.z;
    matrix[4]=forward.x; matrix[5]=forward.y; matrix[6]=forward.z;
    g.RwFrameUpdateObjects(frame);
}

bool VrGameplayInputAllowed() {
    return vrcam::IsStereoActive()&&!g_inputBlocked.load(std::memory_order_acquire);
}

// Interior glass, part two: the windscreen is its own named atomic (hidden in
// the render callback), but SIDE windows are translucent MATERIALS inside the
// door atomics. While the player sits in a vehicle with the option on, drop
// the alpha of every translucent material in its clump to near-invisible and
// restore the exact bytes on exit. Materials are shared per MODEL, so twin
// traffic cars lose their tint too — only while the player is inside.
struct GlassMaterialSave { void* material; std::uint8_t alpha; };
GlassMaterialSave g_glassSaved[128];
int g_glassSavedCount=0;
void* g_glassVehicle=nullptr;

void* GlassMaterialCB(void* material,void* data) {
    (void)data;
    if (!material||g_glassSavedCount>=128) return material;
    auto* alpha=reinterpret_cast<std::uint8_t*>(
        static_cast<char*>(material)+0xb); // RpMaterial color.a (SetAlphaCB)
    if (*alpha<250&&*alpha>8) {
        g_glassSaved[g_glassSavedCount++]={material,*alpha};
        *alpha=8;
    }
    return material;
}

void* GlassAtomicCB(void* atomic,void* data) {
    if (!atomic||!g.RpGeometryForAllMaterials) return atomic;
    void* geometry=*reinterpret_cast<void**>(
        static_cast<char*>(atomic)+0x30);
    if (geometry)
        g.RpGeometryForAllMaterials(geometry,&GlassMaterialCB,data);
    return atomic;
}

void RestoreGlassMaterials() {
    for (int i=0;i<g_glassSavedCount;++i)
        *reinterpret_cast<std::uint8_t*>(
            static_cast<char*>(g_glassSaved[i].material)+0xb)=
            g_glassSaved[i].alpha;
    if (g_glassSavedCount>0)
        LOGI("[driving] interior glass: restored %d materials",
             g_glassSavedCount);
    g_glassSavedCount=0;
    g_glassVehicle=nullptr;
}

void UpdateInteriorGlassMaterials() {
    void* vehicle=(g.FindPlayerVehicle&&IsInteriorGlassHidden()&&
                   vrcam::IsStereoActive())
        ? g.FindPlayerVehicle(-1,false) : nullptr;
    if (vehicle==g_glassVehicle) return;
    RestoreGlassMaterials();
    if (!vehicle||!g.RpClumpForAllAtomics) return;
    void* clump=*reinterpret_cast<void**>(static_cast<char*>(vehicle)+0x20);
    if (!clump) return;
    g_glassVehicle=vehicle;
    g.RpClumpForAllAtomics(clump,&GlassAtomicCB,nullptr);
    LOGI("[driving] interior glass: cleared %d side-window materials",
         g_glassSavedCount);
}

// The mobile pad queries do not consistently consume CPad::NewState.  In
// particular, their CHID/touch paths can remain at zero even though UpdatePads
// already received a live Touch controller.  Vice City solves this one level
// earlier by writing Touch R2/L2 directly to Cross/Square.  These hooks are the
// equivalent SA boundary: while the local player is in any vehicle, return the
// VR value without first entering the game's touch-widget query/trampoline.
// Besides matching reference Quest build, that keeps vehicle entry independent of several
// mobile-only UI calls in the original queries.
bool HasPlayerVehicle() {
    return g.FindPlayerVehicle&&g.FindPlayerVehicle(-1,false)!=nullptr;
}

int OnGetAccelerate(void* pad) {
    if (VrGameplayInputAllowed()&&HasPlayerVehicle()) {
        xr::InputState input{}; xr::GetInput(input);
        const int appearance=GetActiveVehicleAppearance();
        if (appearance==2&&ModeForAppearance(appearance)==MODE_IMMERSIVE) {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            const int model=g_activeVehicleModel.load(std::memory_order_acquire);
            if (IsBicycleModel(model)) {
                if (g_bicycleMode.load(std::memory_order_acquire)==
                        BICYCLE_MOTION)
                    return static_cast<int>(
                        std::clamp(g_bicyclePedalPulse,0.0f,1.0f)*255.0f);
                // HANDLEBAR + R2 is deliberately just the ordinary continuous
                // accelerator key. Holding R2 must drive exactly like holding
                // the keyboard forward key; no cadence or twist is involved.
                return static_cast<int>(
                    std::clamp(input.triggers[1],0.0f,1.0f)*255.0f);
            }
            if (g_activeBikeAccelerator.load(std::memory_order_acquire)==
                BIKE_ACCEL_HOLD_TRIGGER)
                return static_cast<int>(
                    std::clamp(input.triggers[1],0.0f,1.0f)*255.0f);
            return static_cast<int>(std::clamp(g_bikeThrottle,0.0f,1.0f)*255.0f);
        }
        // reference Quest build replaces Cross completely while driving: A is the handbrake
        // and R2 alone is the authoritative accelerator.
        return static_cast<int>(std::clamp(input.triggers[1],0.0f,1.0f)*255.0f);
    }
    return g_origAccelerate?g_origAccelerate(pad):0;
}
int OnGetBrake(void* pad) {
    if (VrGameplayInputAllowed()&&HasPlayerVehicle()) {
        xr::InputState input{}; xr::GetInput(input);
        // Likewise Square/X cannot compete with the authoritative L2 brake.
        return static_cast<int>(std::clamp(input.triggers[0],0.0f,1.0f)*255.0f);
    }
    return g_origBrake?g_origBrake(pad):0;
}
void ReadMappedGameplaySticks(const xr::InputState& input,
                              float& moveX,float& moveY,
                              float& turnX,float& turnY) {
    locomotion::MapGameplaySticks(
        input.leftStick[0],input.leftStick[1],
        input.rightStick[0],input.rightStick[1],
        &moveX,&moveY,&turnX,&turnY);
}

int OnGetSteering(void* pad) {
    EnsureInit();
    if (VrGameplayInputAllowed()&&HasPlayerVehicle()) {
        const int steerAppearance=GetActiveVehicleAppearance();
        if ((steerAppearance==1||steerAppearance==2)&&
            ModeForAppearance(steerAppearance)==MODE_MOTION) {
            // MOTION: controller-yaw steering, same stock-minus convention
            // as the immersive wheel below.
            return static_cast<int>(-std::clamp(
                g_motionSteering.load(std::memory_order_relaxed),
                -1.0f,1.0f)*128.0f);
        }
        if (ModeForAppearance(GetActiveVehicleAppearance())==MODE_IMMERSIVE) {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            const int model=g_activeVehicleModel.load(std::memory_order_acquire);
            if (IsBicycleModel(model)&&
                g_bicycleMode.load(std::memory_order_acquire)==BICYCLE_MOTION)
                return static_cast<int>(
                    -std::clamp(g_bicycleSteering,-1.0f,1.0f)*128.0f);
            if (g_visual.active)
                // Quest VC injects its normalized value directly into
                // CAutomobile::m_fSteerInput. Android SA instead consumes the
                // pad query as -GetSteeringLeftRight()/128, so compensate for
                // that stock minus here while keeping the shared physical and
                // visual wheel angles identical to VC.
                return static_cast<int>(
                    -std::clamp(g_visual.steering,-1.0f,1.0f)*128.0f);
        }
        // DEFAULT, unsupported vehicle types, and the short enter animation all
        // retain immediate stick steering.  The Android query does not reliably
        // read the NewState axis written by VrCamera::OnUpdatePads.
        xr::InputState input{}; xr::GetInput(input);
        float moveX=0.0f,moveY=0.0f,turnX=0.0f,turnY=0.0f;
        ReadMappedGameplaySticks(input,moveX,moveY,turnX,turnY);
        const int steer=static_cast<int>(
            std::clamp(moveX,-1.0f,1.0f)*128.0f);
        // Helis feed this value into their roll channel (ProcessControlInputs
        // stores -steer/128 at heli+0xbd4); log it so a capture shows whether
        // the stick reaches the game.
        if (std::abs(steer)>12&&
            g_activeVehicleAppearance.load(std::memory_order_acquire)==3) {
            static unsigned int rollLog=0;
            if ((++rollLog%120u)==1u)
                LOGI("[driving] heli roll input steer=%d",steer);
        }
        return steer;
    }
    return g_origSteering?g_origSteering(pad):0;
}
int OnGetSteeringUpDown(void* pad) {
    EnsureInit();
    if (VrGameplayInputAllowed()&&HasPlayerVehicle()) {
        const int appearance=GetActiveVehicleAppearance();
        if (appearance==2&&ModeForAppearance(2)==MODE_IMMERSIVE) {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            // CBike negates this pad query before applying m_fLeanInput. VC's
            // immersive lean uses -1 for raised hands (wheelie), hence this
            // sign.
            return static_cast<int>(-std::clamp(g_bikeLean,-1.0f,1.0f)*128.0f);
        }
        if (appearance==5&&ModeForAppearance(5)==MODE_IMMERSIVE) {
            // Real-yoke pitch: pull the grabbed wheel toward you = nose up,
            // push away = nose down. +1 pull maps to full stick-back.
            const float pull=std::clamp(
                g_planePitch.load(std::memory_order_acquire),-1.0f,1.0f);
            return static_cast<int>(pull*128.0f);
        }
        if (appearance==3) {
            // Heli pitch: CHeli::ProcessControlInputs stores this query /128
            // at heli+0xbd0, but the retail body only reads the TOUCH tilt —
            // the left stick Y never reached the helicopter, which is why the
            // stick felt completely dead (roll alone is heavily auto-levelled
            // and nearly invisible). Stick forward = nose down.
            xr::InputState input{}; xr::GetInput(input);
            float moveX=0.0f,moveY=0.0f,turnX=0.0f,turnY=0.0f;
            ReadMappedGameplaySticks(input,moveX,moveY,turnX,turnY);
            return static_cast<int>(
                -std::clamp(moveY,-1.0f,1.0f)*128.0f);
        }
    }
    return g_origSteeringUpDown?g_origSteeringUpDown(pad):0;
}
using GroupFwdFn=int(*)(void*,unsigned char,unsigned char);
GroupFwdFn g_origGroupForward=nullptr;
int OnGetGroupControlForward(void* pad,unsigned char b1,unsigned char b2) {
    if (VrGameplayInputAllowed()&&!HasPlayerVehicle()&&
        vrcam::RecruitGestureActive()) {
        static unsigned int forcedLog=0;
        if ((++forcedLog%90u)==1u)
            LOGI("[recruit] GroupControlForward forced (b1=%d b2=%d)",b1,b2);
        return 1;
    }
    return g_origGroupForward?g_origGroupForward(pad,b1,b2):0;
}

using HornFn=int(*)(void*,unsigned char);
HornFn g_origHorn=nullptr;
int OnGetHorn(void* pad,unsigned char arg) {
    if (VrGameplayInputAllowed()&&HasPlayerVehicle()&&
        g_hornPressed.load(std::memory_order_acquire)) return 1;
    return g_origHorn?g_origHorn(pad,arg):0;
}
int OnGetHandBrake(void* pad) {
    if (VrGameplayInputAllowed()&&HasPlayerVehicle()) {
        xr::InputState input{}; xr::GetInput(input);
        return input.a?255:0;
    }
    return g_origHandBrake?g_origHandBrake(pad):0;
}
bool OnGetSprint(void* pad,int sprintType) {
    if (vrcam::IsStereoActive()) {
        if (g_inputBlocked.load(std::memory_order_acquire)||HasPlayerVehicle())
            return false;
        xr::InputState input{}; xr::GetInput(input);
        // Vice City Cross mapping through the remappable bindings: any face
        // button assigned to SPRINT feeds Cross, plus R2 as before.
        return locomotion::ActionHeld(locomotion::BIND_ACT_SPRINT,
                                      input.a,input.b,input.x,input.y,
                                      input.l3,input.r3,true)||
               input.triggers[1]>=0.50f;
    }
    return g_origSprint?g_origSprint(pad,sprintType):false;
}

bool OnNextStationJustUp(void* pad) {
    if (VrGameplayInputAllowed() && HasPlayerVehicle() &&
        g_radioChangeJustPressed.exchange(false, std::memory_order_acq_rel)) {
        LOGI("[driving] Touch X -> next radio station");
        return true;
    }
    return g_origNextStation ? g_origNextStation(pad) : false;
}
bool OnLastStationJustUp(void* pad) {
    if (VrGameplayInputAllowed() && HasPlayerVehicle() &&
        g_radioPrevJustPressed.exchange(false, std::memory_order_acq_rel)) {
        LOGI("[driving] grip+X -> previous radio station");
        return true;
    }
    return g_origLastStation ? g_origLastStation(pad) : false;
}

bool PatchAbsoluteJump(void* target,void* replacement) {
    if (!target||!replacement) return false;
    const std::uintptr_t pageSize=static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    auto* code=reinterpret_cast<std::uint32_t*>(target);
    const std::uintptr_t start=reinterpret_cast<std::uintptr_t>(code)&~(pageSize-1);
    const std::uintptr_t end=(reinterpret_cast<std::uintptr_t>(code)+16+pageSize-1)&~(pageSize-1);
    if (mprotect(reinterpret_cast<void*>(start),end-start,PROT_READ|PROT_WRITE|PROT_EXEC)!=0) return false;
    code[0]=0x58000051u; code[1]=0xD61F0220u;
    *reinterpret_cast<void**>(code+2)=replacement;
    __builtin___clear_cache(reinterpret_cast<char*>(code),reinterpret_cast<char*>(code)+16);
    return true;
}

void* InstallVerifiedTrampoline(void* target,void* replacement,
                                const std::uint32_t expected[4],const char* name) {
    if (!target||!replacement) return nullptr;
    std::uint32_t observed[4]{}; std::memcpy(observed,target,sizeof(observed));
    if (std::memcmp(observed,expected,sizeof(observed))!=0) {
        LOGE("[driving] %s prologue mismatch: %08x %08x %08x %08x",
             name,observed[0],observed[1],observed[2],observed[3]); return nullptr;
    }
    const std::uintptr_t pageSize=static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    void* trampoline=mmap(nullptr,pageSize,PROT_READ|PROT_WRITE|PROT_EXEC,
                          MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (trampoline==MAP_FAILED) return nullptr;
    auto* t=reinterpret_cast<std::uint32_t*>(trampoline);
    std::memcpy(t,target,16); t[4]=0x58000051u; t[5]=0xD61F0220u;
    *reinterpret_cast<void**>(t+6)=reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(target)+16);
    __builtin___clear_cache(reinterpret_cast<char*>(t),reinterpret_cast<char*>(t)+32);
    if (!PatchAbsoluteJump(target,replacement)) { munmap(trampoline,pageSize); return nullptr; }
    LOGI("[driving] %s VR query hook installed",name);
    return trampoline;
}

void* InstallSprintTrampoline(void* target,void* replacement) {
    if (!target||!replacement) return nullptr;
    constexpr std::uint32_t expected[4]={
        0x79422008u,0x2A1F03E0u,0x35000668u,0xD0001CC8u
    };
    std::uint32_t observed[4]{}; std::memcpy(observed,target,sizeof(observed));
    if (std::memcmp(observed,expected,sizeof(observed))!=0) {
        LOGE("[driving] CPad::GetSprint prologue mismatch: %08x %08x %08x %08x",
             observed[0],observed[1],observed[2],observed[3]); return nullptr;
    }
    const std::uintptr_t pageSize=static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    void* trampoline=mmap(nullptr,pageSize,PROT_READ|PROT_WRITE|PROT_EXEC,
                          MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (trampoline==MAP_FAILED) return nullptr;
    auto* t=reinterpret_cast<std::uint32_t*>(trampoline);

    // The fourth displaced instruction is an ADRP used by the original global
    // input-disable gate.  Jumping back to target+12 is invalid because the
    // 16-byte absolute hook stores its address literal over that instruction.
    // Resolve the ADRP page while the original bytes are still present, load it
    // literally in the trampoline, and resume at the untouched target+16 LDR.
    const std::uint32_t adrp=observed[3];
    std::int64_t adrpPages=static_cast<std::int64_t>(
        (((adrp>>5)&0x7ffffu)<<2)|((adrp>>29)&0x3u));
    if (adrpPages&(1ll<<20)) adrpPages-=1ll<<21;
    const auto adrpPcPage=static_cast<std::intptr_t>(
        (reinterpret_cast<std::uintptr_t>(target)+12u)&~std::uintptr_t{0xfffu});
    const auto globalPage=static_cast<std::uintptr_t>(adrpPcPage+adrpPages*4096ll);

    t[0]=0x79422008u; // LDRH W8,[X0,#0x110]
    t[1]=0x2A1F03E0u; // MOV W0,WZR
    t[2]=0x35000088u; // CBNZ W8,local RET at +0x18
    t[3]=0x580000A8u; // LDR X8,global-page literal at +0x20
    t[4]=0x580000D1u; // LDR X17,resume literal at +0x28
    t[5]=0xD61F0220u; // BR X17
    t[6]=0xD65F03C0u; // RET
    t[7]=0xD503201Fu; // NOP / literal alignment
    *reinterpret_cast<std::uintptr_t*>(t+8)=globalPage;
    *reinterpret_cast<void**>(t+10)=reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(target)+16u);
    __builtin___clear_cache(reinterpret_cast<char*>(t),reinterpret_cast<char*>(t)+48);
    if (!PatchAbsoluteJump(target,replacement)) { munmap(trampoline,pageSize); return nullptr; }
    LOGI("[driving] CPad::GetSprint VR A-button hook installed");
    return trampoline;
}

void* InstallHandBrakeTrampoline(void* target,void* replacement) {
    if (!target||!replacement) return nullptr;
    constexpr std::uint32_t expected[4]={
        0x79422008u, // LDRH W8,[X0,#0x110]
        0x34000068u, // CBZ W8,function+0x10
        0x2A1F03E0u, // MOV W0,WZR
        0xD65F03C0u  // RET
    };
    std::uint32_t observed[4]{}; std::memcpy(observed,target,sizeof(observed));
    if (std::memcmp(observed,expected,sizeof(observed))!=0) {
        LOGE("[driving] CPad::GetHandBrake prologue mismatch: %08x %08x %08x %08x",
             observed[0],observed[1],observed[2],observed[3]); return nullptr;
    }
    const std::uintptr_t pageSize=static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    void* trampoline=mmap(nullptr,pageSize,PROT_READ|PROT_WRITE|PROT_EXEC,
                          MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (trampoline==MAP_FAILED) return nullptr;
    auto* t=reinterpret_cast<std::uint32_t*>(trampoline);
    // Disabled controls return zero; the enabled path resumes at the original
    // stack prologue at +0x10, leaving all later PC-relative code in-place.
    t[0]=0x79422008u;
    t[1]=0x35000068u; // CBNZ W8,local zero return at +0x10
    t[2]=0x58000091u; // LDR X17,literal at +0x18
    t[3]=0xD61F0220u;
    t[4]=0x2A1F03E0u;
    t[5]=0xD65F03C0u;
    *reinterpret_cast<void**>(t+6)=reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(target)+16);
    __builtin___clear_cache(reinterpret_cast<char*>(t),reinterpret_cast<char*>(t)+32);
    if (!PatchAbsoluteJump(target,replacement)) { munmap(trampoline,pageSize); return nullptr; }
    LOGI("[driving] CPad::GetHandBrake VR A-button hook installed");
    if (g.CPad_GetHorn) {
        // 2.11 arm64 @0x49d6b4: stp/str/mov/ldrh — no PC-relative words in
        // the copied prologue.
        constexpr std::uint32_t hornPrologue[4]={
            0xa9be7bfdu,0xf9000bf3u,0x910003fdu,0x79422008u};
        g_origHorn=reinterpret_cast<HornFn>(
            InstallVerifiedTrampoline(g.CPad_GetHorn,
                reinterpret_cast<void*>(&OnGetHorn),
                hornPrologue,"CPad::GetHorn"));
        if (g_origHorn)
            LOGI("[driving] wheel-hub horn hook installed");
    }
    return trampoline;
}

void* InstallStationTrampoline(void* target, void* replacement) {
    if (!target || !replacement) return nullptr;
    constexpr std::uint32_t expected[4]={
        0x79422008u, // LDRH W8,[X0,#0x110]
        0x34000068u, // CBZ W8,function+0x10
        0x2A1F03E0u, // MOV W0,WZR
        0xD65F03C0u  // RET
    };
    std::uint32_t observed[4]{}; std::memcpy(observed,target,sizeof(observed));
    if (std::memcmp(observed,expected,sizeof(observed))!=0) {
        LOGE("[driving] CPad::NextStationJustUp prologue mismatch: %08x %08x %08x %08x",
             observed[0],observed[1],observed[2],observed[3]); return nullptr;
    }
    const std::uintptr_t pageSize=static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    void* trampoline=mmap(nullptr,pageSize,PROT_READ|PROT_WRITE|PROT_EXEC,
                          MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (trampoline==MAP_FAILED) return nullptr;
    auto* t=reinterpret_cast<std::uint32_t*>(trampoline);
    t[0]=0x79422008u;
    t[1]=0x35000068u; // controls disabled -> local zero return
    t[2]=0x58000091u; // enabled -> untouched body at target+0x10
    t[3]=0xD61F0220u;
    t[4]=0x2A1F03E0u;
    t[5]=0xD65F03C0u;
    *reinterpret_cast<void**>(t+6)=reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(target)+16);
    __builtin___clear_cache(reinterpret_cast<char*>(t),reinterpret_cast<char*>(t)+32);
    if (!PatchAbsoluteJump(target,replacement)) { munmap(trampoline,pageSize); return nullptr; }
    LOGI("[driving] CPad::NextStationJustUp VR X-button hook installed");
    return trampoline;
}

// Right-stick vehicle accessors. In touch mode ([pad+0x118]!=0) their retail
// bodies branch into CHID analog-axis reads and never touch NewState — so no
// amount of pad writing reaches the Hydra nozzles. Serve the Quest right
// stick directly while flying; every other vehicle (tank turret, tow truck,
// hydraulics) keeps the retail path.
using CarGunUpDownFn = int (*)(void*, int, void*, float, int);
using CarGunLeftRightFn = int (*)(void*, int, int);
CarGunUpDownFn g_origCarGunUpDown = nullptr;
CarGunLeftRightFn g_origCarGunLeftRight = nullptr;

bool AircraftRightStickActive() {
    if (!VrGameplayInputAllowed() || !HasPlayerVehicle()) return false;
    // Aircraft, plus the Rhino: TankControl reads the same CarGun accessors
    // for turret rotation/elevation.
    if (g_activeVehicleModel.load(std::memory_order_acquire) == kRhinoModelId)
        return true;
    const int appearance = GetActiveVehicleAppearance();
    return appearance == 3 || appearance == 5;
}

int OnGetCarGunUpDown(void* pad, int ignoreDucking, void* vehicle,
                      float range, int useSecondary) {
    if (AircraftRightStickActive()) {
        xr::InputState input{}; xr::GetInput(input);
        float moveX=0.0f,moveY=0.0f,turnX=0.0f,turnY=0.0f;
        ReadMappedGameplaySticks(input,moveX,moveY,turnX,turnY);
        float v = turnY;
        if (std::abs(v) < 0.18f) v = 0.0f;
        // Stick forward sweeps the Hydra nozzles toward level flight.
        return static_cast<int>(std::clamp(v, -1.0f, 1.0f) * 128.0f);
    }
    return g_origCarGunUpDown
        ? g_origCarGunUpDown(pad, ignoreDucking, vehicle, range, useSecondary)
        : 0;
}

int OnGetCarGunLeftRight(void* pad, int ignoreDucking, int useSecondary) {
    if (AircraftRightStickActive()) {
        xr::InputState input{}; xr::GetInput(input);
        float moveX=0.0f,moveY=0.0f,turnX=0.0f,turnY=0.0f;
        ReadMappedGameplaySticks(input,moveX,moveY,turnX,turnY);
        float v = turnX;
        if (std::abs(v) < 0.18f) v = 0.0f;
        return static_cast<int>(std::clamp(v, -1.0f, 1.0f) * 128.0f);
    }
    return g_origCarGunLeftRight
        ? g_origCarGunLeftRight(pad, ignoreDucking, useSecondary) : 0;
}

// Tank main gun. GetCarGunFired carries the same touch gate ([pad+0x118]) as
// the analog accessors, so the Rhino could never fire on Quest. B pulls the
// trigger while seated in the tank; everything else keeps the retail body
// (reached through a gate-reproducing trampoline — the prologue's PC-relative
// CBNZ/CBZ pair cannot be relocated).
using CarGunFiredFn = int (*)(void*, int, int);
CarGunFiredFn g_origCarGunFired = nullptr;

int OnGetCarGunFired(void* pad, int ignoreDucking, int useSecondary) {
    if (VrGameplayInputAllowed() &&
        g_activeVehicleModel.load(std::memory_order_acquire) ==
            kRhinoModelId) {
        xr::InputState input{}; xr::GetInput(input);
        return input.b ? 1 : 0;
    }
    return g_origCarGunFired
        ? g_origCarGunFired(pad, ignoreDucking, useSecondary) : 0;
}

void* InstallCarGunFiredTrampoline(void* target, void* replacement) {
    if (!target || !replacement) return nullptr;
    constexpr std::uint32_t expected[4] = {
        0x79422008u, // LDRH W8,[X0,#0x110]
        0x35000068u, // CBNZ W8, +0x10 (zero return)
        0x39446008u, // LDRB W8,[X0,#0x118]
        0x34000068u  // CBZ  W8, +0x18 (real body)
    };
    std::uint32_t observed[4]{};
    std::memcpy(observed, target, sizeof(observed));
    if (std::memcmp(observed, expected, sizeof(expected)) != 0) {
        LOGW("[driving] CarGunFired prologue mismatch %08x %08x %08x %08x",
             observed[0], observed[1], observed[2], observed[3]);
        return nullptr;
    }
    const std::uintptr_t pageSize =
        static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    void* trampoline = mmap(nullptr, pageSize, PROT_READ|PROT_WRITE|PROT_EXEC,
                            MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return nullptr;
    auto* t = reinterpret_cast<std::uint32_t*>(trampoline);
    t[0] = 0x79422008u;             // LDRH W8,[X0,#0x110]
    t[1] = 0x350000A8u;             // CBNZ W8, +20 (zero return)
    t[2] = 0x39446008u;             // LDRB W8,[X0,#0x118]
    t[3] = 0x35000068u;             // CBNZ W8, +12 (zero return: touch mode)
    t[4] = 0x58000091u;             // LDR X17, literal (+16)
    t[5] = 0xD61F0220u;             // BR X17
    t[6] = 0x2A1F03E0u;             // MOV W0, WZR
    t[7] = 0xD65F03C0u;             // RET
    *reinterpret_cast<void**>(t + 8) = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(target) + 0x18);
    __builtin___clear_cache(reinterpret_cast<char*>(t),
                            reinterpret_cast<char*>(t) + 40);
    if (!PatchAbsoluteJump(target, replacement)) {
        munmap(trampoline, pageSize);
        return nullptr;
    }
    LOGI("[driving] Rhino CarGunFired B-trigger hook installed");
    return trampoline;
}

// CPad::CarGunJustDown — what CAutomobile::TankControl ACTUALLY polls for the
// Rhino cannon on 2.11 — never reads the gamepad at all: past the
// controls-disabled gate it is a bare CTouchInterface::IsTouched(widget 11)
// query, so button B could not fire no matter what the pad hooks fed. Same
// treatment as GetCarGunFired: PC-relative CBZ prologue, gate-reproducing
// trampoline, VR replaces the answer with the live B button while driving
// the tank. TankControl's own 800ms big-gun cooldown paces held fire.
using CarGunJustDownFn = int (*)(void*);
using TouchQueryFn = int (*)(void*);
CarGunJustDownFn g_origCarGunJustDown = nullptr;
// X pressed this frame and not the last, sampled once per frame on foot.
std::atomic<bool> g_jumpEdge{false};

int OnCarGunJustDown(void* pad) {
    if (VrGameplayInputAllowed() &&
        g_activeVehicleModel.load(std::memory_order_acquire) ==
            kRhinoModelId) {
        xr::InputState input{}; xr::GetInput(input);
        return input.b ? 1 : 0;
    }
    return g_origCarGunJustDown ? g_origCarGunJustDown(pad) : 0;
}

void* InstallTouchQueryTrampoline(void* target, void* replacement,
                                  const char* name) {
    if (!target || !replacement) return nullptr;
    constexpr std::uint32_t expected[4] = {
        0x79422008u, // LDRH W8,[X0,#0x110]
        0x34000068u, // CBZ  W8, +0x10 (real body)
        0x2a1f03e0u, // MOV  W0, WZR
        0xd65f03c0u  // RET
    };
    std::uint32_t observed[4]{};
    std::memcpy(observed, target, sizeof(observed));
    if (std::memcmp(observed, expected, sizeof(expected)) != 0) {
        LOGW("[driving] %s prologue mismatch %08x %08x %08x %08x",
             name, observed[0], observed[1], observed[2], observed[3]);
        return nullptr;
    }
    const std::uintptr_t pageSize =
        static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    void* trampoline = mmap(nullptr, pageSize, PROT_READ|PROT_WRITE|PROT_EXEC,
                            MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return nullptr;
    auto* t = reinterpret_cast<std::uint32_t*>(trampoline);
    t[0] = 0x79422008u;             // LDRH W8,[X0,#0x110]
    t[1] = 0x34000068u;             // CBZ  W8, +12 (to the BR)
    t[2] = 0x2A1F03E0u;             // MOV  W0, WZR
    t[3] = 0xD65F03C0u;             // RET
    t[4] = 0x58000051u;             // LDR X17, literal (+8)
    t[5] = 0xD61F0220u;             // BR X17
    *reinterpret_cast<void**>(t + 6) = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(target) + 0x10);
    __builtin___clear_cache(reinterpret_cast<char*>(t),
                            reinterpret_cast<char*>(t) + 40);
    if (!PatchAbsoluteJump(target, replacement)) {
        munmap(trampoline, pageSize);
        return nullptr;
    }
    LOGI("[driving] %s touch-query hook installed", name);
    return trampoline;
}

// Heli tail rotor. CHeli::ProcessControlInputs polls CPad::GetTurretLeft/
// GetTurretRight for yaw on every input type except keyboard (disasm
// @0x6c8848: only type 2 skips the block) - and both are pure touch queries
// (IsTouched widgets 26/27, same prologue as CarGunJustDown). The right VR
// stick therefore could not yaw a helicopter through any pad write. Same
// gate-reproducing trampoline; the right stick X axis answers instead.
TouchQueryFn g_origTurretLeft = nullptr;
TouchQueryFn g_origTurretRight = nullptr;

int OnGetTurretLeft(void* pad) {
    if (VrGameplayInputAllowed() &&
        g_activeVehicleAppearance.load(std::memory_order_acquire) == 3) {
        xr::InputState input{}; xr::GetInput(input);
        float moveX=0.0f,moveY=0.0f,turnX=0.0f,turnY=0.0f;
        ReadMappedGameplaySticks(input,moveX,moveY,turnX,turnY);
        return turnX < -0.35f ? 1 : 0;
    }
    return g_origTurretLeft ? g_origTurretLeft(pad) : 0;
}

int OnGetTurretRight(void* pad) {
    if (VrGameplayInputAllowed() &&
        g_activeVehicleAppearance.load(std::memory_order_acquire) == 3) {
        xr::InputState input{}; xr::GetInput(input);
        float moveX=0.0f,moveY=0.0f,turnX=0.0f,turnY=0.0f;
        ReadMappedGameplaySticks(input,moveX,moveY,turnX,turnY);
        return turnX > 0.35f ? 1 : 0;
    }
    return g_origTurretRight ? g_origTurretRight(pad) : 0;
}

// On-foot jump. CPad::JumpJustDown on 2.11 is IsDoubleTapped(widget 168) ||
// IsDoubleTapped(widget 31) - again pure touch, so the earlier NewState write
// never made CJ jump. The Vice City mapping (X button) answers instead; the
// edge is computed once per frame in UpdateInput so repeated per-frame polls
// see one coherent "just down".
TouchQueryFn g_origJumpJustDown = nullptr;

int OnJumpJustDown(void* pad) {
    if (VrGameplayInputAllowed() &&
        g_jumpEdge.load(std::memory_order_acquire))
        return 1;
    return g_origJumpJustDown ? g_origJumpJustDown(pad) : 0;
}

// CAutomobile::TankControl only reads GetCarGunLeftRight/UpDown when the
// active CCam mode is NOT 18 (mobile MODE_CAM_ON_A_STRING); in the ordinary
// chase mode the turret is slaved to activeCam.m_vecFront, which the VR
// camera never updates — the turret sat frozen and the right stick did
// nothing. Spoof the mode away from 18 strictly around the retail call while
// the player drives the Rhino so the stick path (fed by our CarGun hooks)
// runs; restore the exact halfword immediately.
using TankControlFn = void (*)(void*);
TankControlFn g_origTankControl = nullptr;

void OnTankControl(void* automobile) {
    std::uint16_t* mode = nullptr;
    std::uint16_t saved = 0;
    if (VrGameplayInputAllowed() && g.TheCamera &&
        g_activeVehicleModel.load(std::memory_order_acquire) ==
            kRhinoModelId) {
        auto* cameraBytes = reinterpret_cast<std::uint8_t*>(g.TheCamera);
        const unsigned activeCam = cameraBytes[0x5f];
        if (activeCam < 3u) {
            mode = reinterpret_cast<std::uint16_t*>(
                cameraBytes + activeCam * 0x228u + 0x186u);
            saved = *mode;
            if (saved == 18) *mode = 0;
            else mode = nullptr;
        }
    }
    if (g_origTankControl) g_origTankControl(automobile);
    if (mode) *mode = saved;
}

// CHID::GetInputType (0=touch, 1=joystick) gates every NewState right-stick
// read in the CPad car-gun accessors: with touch reported, GetCarGunUpDown
// returns 0 and the Hydra's thrust nozzles (plus other right-stick vehicle
// controls) are dead no matter what OnUpdatePads writes. While VR flies an
// aircraft we ARE the gamepad, so report one. The retail prologue is
// ADRP-relative and cannot be relocated into a trampoline; the entry is
// replaced outright and the tiny retail body reimplemented for the
// pass-through case (instance table read + vtable call at +0x30, from the
// 2.11 disasm of 0x72dea0).
int OnGetInputType() {
    if (VrGameplayInputAllowed()) {
        const int appearance =
            g_activeVehicleAppearance.load(std::memory_order_acquire);
        if (appearance == 3 || appearance == 5) return 1; // joystick
    }
    if (!g.LoadBase) return 0;
    const auto* indexPtr = *reinterpret_cast<const std::int32_t* const*>(
        g.LoadBase + 0x839a08u);
    void* const* instances = *reinterpret_cast<void* const* const*>(
        g.LoadBase + 0x83a2c8u);
    if (indexPtr == nullptr || instances == nullptr) return 0;
    void* instance = instances[*indexPtr];
    if (instance == nullptr) return 0;
    void** vtable = *reinterpret_cast<void***>(instance);
    const auto internal = reinterpret_cast<int (*)(void*)>(vtable[6]);
    return internal ? internal(instance) : 0;
}

bool InstallInputTypePatch() {
    if (!g.CHID_GetInputType) return false;
    constexpr std::uint32_t expected[4] = {
        0x90000868u, 0xb0000869u, 0xf9450508u, 0xb9800108u};
    std::uint32_t observed[4]{};
    std::memcpy(observed, g.CHID_GetInputType, sizeof(observed));
    if (std::memcmp(observed, expected, sizeof(expected)) != 0) {
        LOGW("[driving] CHID::GetInputType prologue mismatch "
             "%08x %08x %08x %08x",
             observed[0], observed[1], observed[2], observed[3]);
        return false;
    }
    return PatchAbsoluteJump(g.CHID_GetInputType,
                             reinterpret_cast<void*>(&OnGetInputType));
}

void InstallPadHooks() {
    constexpr std::uint32_t accel[4]={0xD100C3FFu,0xA9017BFDu,0xA9024FF4u,0x910043FDu};
    constexpr std::uint32_t brake[4]={0xD100C3FFu,0xA9017BFDu,0xA9024FF4u,0x910043FDu};
    constexpr std::uint32_t steer[4]={0xD10103FFu,0xA9017BFDu,0xF90013F5u,0xA9034FF4u};
    constexpr std::uint32_t steerUpDown[4]={0xD100C3FFu,0xA9017BFDu,0xA9024FF4u,0x910043FDu};
    g_origAccelerate=reinterpret_cast<AxisQueryFn>(InstallVerifiedTrampoline(
        reinterpret_cast<void*>(g.CPad_GetAccelerate),reinterpret_cast<void*>(&OnGetAccelerate),accel,"CPad::GetAccelerate"));
    g_origBrake=reinterpret_cast<AxisQueryFn>(InstallVerifiedTrampoline(
        reinterpret_cast<void*>(g.CPad_GetBrake),reinterpret_cast<void*>(&OnGetBrake),brake,"CPad::GetBrake"));
    g_origSteering=reinterpret_cast<AxisQueryFn>(InstallVerifiedTrampoline(
        reinterpret_cast<void*>(g.CPad_GetSteeringLeftRight),reinterpret_cast<void*>(&OnGetSteering),steer,"CPad::GetSteeringLeftRight"));
    g_origSteeringUpDown=reinterpret_cast<AxisQueryFn>(InstallVerifiedTrampoline(
        reinterpret_cast<void*>(g.CPad_GetSteeringUpDown),reinterpret_cast<void*>(&OnGetSteeringUpDown),steerUpDown,"CPad::GetSteeringUpDown"));
    g_origHandBrake=reinterpret_cast<AxisQueryFn>(InstallHandBrakeTrampoline(
        reinterpret_cast<void*>(g.CPad_GetHandBrake),reinterpret_cast<void*>(&OnGetHandBrake)));
    g_origSprint=reinterpret_cast<SprintFn>(InstallSprintTrampoline(
        reinterpret_cast<void*>(g.CPad_GetSprint),reinterpret_cast<void*>(&OnGetSprint)));
    g_origNextStation=reinterpret_cast<ButtonQueryFn>(InstallStationTrampoline(
        reinterpret_cast<void*>(g.CPad_NextStationJustUp),
        reinterpret_cast<void*>(&OnNextStationJustUp)));
    // LastStationJustUp shares the exact NextStation prologue (verified in
    // the shipped arm64), so the same verified trampoline applies.
    g_origLastStation=reinterpret_cast<ButtonQueryFn>(InstallStationTrampoline(
        reinterpret_cast<void*>(g.CPad_LastStationJustUp),
        reinterpret_cast<void*>(&OnLastStationJustUp)));
    LOGI("[driving] aircraft gamepad input-type patch=%d",
         InstallInputTypePatch()?1:0);
    constexpr std::uint32_t carGunUpDown[4]={
        0x6dbc23e9u,0xa9017bfdu,0xa90257f6u,0xa9034ff4u};
    constexpr std::uint32_t carGunLeftRight[4]={
        0xa9bd7bfdu,0xf9000bf5u,0xa9024ff4u,0x910003fdu};
    g_origCarGunUpDown=reinterpret_cast<CarGunUpDownFn>(
        InstallVerifiedTrampoline(g.CPad_GetCarGunUpDown,
            reinterpret_cast<void*>(&OnGetCarGunUpDown),
            carGunUpDown,"CPad::GetCarGunUpDown"));
    g_origCarGunLeftRight=reinterpret_cast<CarGunLeftRightFn>(
        InstallVerifiedTrampoline(g.CPad_GetCarGunLeftRight,
            reinterpret_cast<void*>(&OnGetCarGunLeftRight),
            carGunLeftRight,"CPad::GetCarGunLeftRight"));
    LOGI("[driving] aircraft right-stick hooks up=%d lr=%d",
         g_origCarGunUpDown?1:0,g_origCarGunLeftRight?1:0);
    g_origCarGunFired=reinterpret_cast<CarGunFiredFn>(
        InstallCarGunFiredTrampoline(g.CPad_GetCarGunFired,
            reinterpret_cast<void*>(&OnGetCarGunFired)));
    g_origCarGunJustDown=reinterpret_cast<CarGunJustDownFn>(
        InstallTouchQueryTrampoline(g.CPad_CarGunJustDown,
            reinterpret_cast<void*>(&OnCarGunJustDown),
            "Rhino CarGunJustDown"));
    g_origTurretLeft=reinterpret_cast<TouchQueryFn>(
        InstallTouchQueryTrampoline(g.CPad_GetTurretLeft,
            reinterpret_cast<void*>(&OnGetTurretLeft),
            "Heli GetTurretLeft"));
    g_origTurretRight=reinterpret_cast<TouchQueryFn>(
        InstallTouchQueryTrampoline(g.CPad_GetTurretRight,
            reinterpret_cast<void*>(&OnGetTurretRight),
            "Heli GetTurretRight"));
    g_origJumpJustDown=reinterpret_cast<TouchQueryFn>(
        InstallTouchQueryTrampoline(g.CPad_JumpJustDown,
            reinterpret_cast<void*>(&OnJumpJustDown),
            "Jump JumpJustDown"));
    g_origGroupForward=reinterpret_cast<GroupFwdFn>(
        InstallTouchQueryTrampoline(g.CPad_GetGroupControlForward,
            reinterpret_cast<void*>(&OnGetGroupControlForward),
            "Recruit GroupControlForward"));
    if (g.CAutomobile_TankControl) {
        // Verified against the 2.11 arm64 disasm @0x6a9888: plain callee-save
        // prologue, no PC-relative instructions in the first four words.
        constexpr std::uint32_t tankControl[4]={
            0xd10483ffu,0xfd006beau,0x6d0da3e9u,0xa90efbfdu};
        g_origTankControl=reinterpret_cast<TankControlFn>(
            InstallVerifiedTrampoline(
                reinterpret_cast<void*>(g.CAutomobile_TankControl),
                reinterpret_cast<void*>(&OnTankControl),
                tankControl,"CAutomobile::TankControl"));
        if (g_origTankControl)
            LOGI("[driving] Rhino TankControl camera-mode spoof installed");
    }

    // Retail 2.11 only translates BIKE_HANDLEBARS in CBike::PreRender. Rotate
    // that real frame after the stock update so Sanchez and the other bikes
    // visually follow the immersive steering angle instead of leaving only the
    // front fork moving.
    if (g.LoadBase&&g.CBike_PreRender&&
        reinterpret_cast<std::uintptr_t>(g.CBike_PreRender)-g.LoadBase==
            0x6ba318u) {
        constexpr std::uint32_t bikePreRender[4]={
            0x6db63befu,0x6d0133edu,0x6d022bebu,0x6d0323e9u};
        g_origBikePreRender=reinterpret_cast<BikePreRenderFn>(
            InstallVerifiedTrampoline(
                reinterpret_cast<void*>(g.CBike_PreRender),
                reinterpret_cast<void*>(&OnBikePreRenderThunk),bikePreRender,
                "CBike::PreRender"));
    }
    // Bicycles override PreRender entirely (CBmx::PreRender never calls the
    // CBike version), so the handlebar rotation needs its own entry hook.
    if (g.LoadBase&&g.CBmx_PreRender&&
        reinterpret_cast<std::uintptr_t>(g.CBmx_PreRender)-g.LoadBase==
            0x6bfbb8u) {
        constexpr std::uint32_t bmxPreRender[4]={
            0xfc170fecu,0x6d012bebu,0x6d0223e9u,0xa9037bfdu};
        g_origBmxPreRender=reinterpret_cast<BikePreRenderFn>(
            InstallVerifiedTrampoline(
                reinterpret_cast<void*>(g.CBmx_PreRender),
                reinterpret_cast<void*>(&OnBmxPreRenderThunk),bmxPreRender,
                "CBmx::PreRender"));
    }
    LOGI("[driving] handlebar hooks bike=%d bmx=%d",
         g_origBikePreRender?1:0,g_origBmxPreRender?1:0);
    if (g.LoadBase&&g.CEventKnockOffBike_AffectsPed&&
        reinterpret_cast<std::uintptr_t>(g.CEventKnockOffBike_AffectsPed)-
            g.LoadBase==0x4ea90cu) {
        constexpr std::uint32_t affectsPed[4]={
            0xA9BE7BFDu,0xA9014FF4u,0x910003FDu,0xAA0003F4u};
        g_origKnockOffAffectsPed=reinterpret_cast<KnockOffAffectsPedFn>(
            InstallVerifiedTrampoline(
                reinterpret_cast<void*>(g.CEventKnockOffBike_AffectsPed),
                reinterpret_cast<void*>(&OnKnockOffBikeAffectsPed),affectsPed,
                "CEventKnockOffBike::AffectsPed"));
    }
}

} // namespace

void Init() { EnsureInit(); InstallPadHooks(); }

void ResetDefaultPreset() {
    EnsureInit();
    for (int field=0;field<F_COUNT;++field) g_value[field].store(kDefaults[field]);
    Save(); ResetInteraction();
}
int GetOffsetCm(int field) {
    EnsureInit(); return field>=0&&field<F_COUNT?g_value[field].load():0;
}
void AdjustOffsetCm(int field,int direction) {
    EnsureInit(); if (field<0||field>=F_COUNT||direction==0) return;
    g_value[field].store(ClampField(field,g_value[field].load()+(direction<0?-1:1)));
    Save();
}
const char* FieldLabel(int field) {
    switch(field) {
        case F_SIDE:return "DEFAULT CAR SIDE";
        case F_DISTANCE:return "DEFAULT CAR FORWARD";
        case F_HEIGHT:return "DEFAULT CAR HEIGHT";
        case F_WHEEL_SIDE:return "CAR WHEEL SIDE";
        case F_BIKE_SEAT_DISTANCE:return "DEFAULT BIKE FORWARD";
        case F_BIKE_SEAT_HEIGHT:return "DEFAULT BIKE HEIGHT";
        case F_IMMERSIVE_CAR_SEAT_DISTANCE:return "IMMERSIVE CAR FORWARD";
        case F_IMMERSIVE_CAR_SEAT_HEIGHT:return "IMMERSIVE CAR HEIGHT";
        case F_IMMERSIVE_BIKE_SEAT_DISTANCE:return "IMMERSIVE BIKE FORWARD";
        case F_IMMERSIVE_BIKE_SEAT_HEIGHT:return "IMMERSIVE BIKE HEIGHT";
        case F_WHEEL_DISTANCE:return "CAR WHEEL FORWARD";
        case F_WHEEL_HEIGHT:return "CAR WHEEL HEIGHT";
        case F_WHEEL_RADIUS:return "CAR WHEEL RADIUS";
        case F_BIKE_HALF_WIDTH:return "BIKE HALF WIDTH";
        case F_BIKE_DISTANCE:return "BIKE DISTANCE";
        case F_BIKE_HEIGHT:return "BIKE HEIGHT";
        default:return "DRIVING";
    }
}
int GetMode() {
    EnsureInit();
    return ModeForAppearance(GetActiveVehicleAppearance(false));
}
const char* GetModeName() { return GetMode()==MODE_IMMERSIVE?"IMMERSIVE":"DEFAULT"; }
void CycleMode(int direction) {
    const int appearance=GetActiveVehicleAppearance(false);
    if (appearance==2) CycleBikeMode(direction);
    else if (appearance==1) CycleCarMode(direction);
    else if (appearance==4) CycleModeForVehicleType(VEHICLE_BOAT,direction);
}
int GetCarMode() { EnsureInit(); return g_carMode.load(); }
int GetBikeMode() { EnsureInit(); return g_bikeMode.load(); }
const char* ModeName(int mode) {
    switch (mode) {
        case MODE_IMMERSIVE: return "IMMERSIVE";
        case MODE_MOTION:    return "MOTION";
        default:             return "DEFAULT";
    }
}
const char* GetCarModeName() { return ModeName(GetCarMode()); }
const char* GetBikeModeName() { return ModeName(GetBikeMode()); }
void CycleCarMode(int direction) {
    EnsureInit(); if (direction==0) return;
    if (!kImmersiveDrivingEnabled) {
        g_carMode.store(MODE_DEFAULT);
        ResetInteraction();
        LOGW("[driving] IMMERSIVE is disabled in this diagnostic build");
        return;
    }
    g_carMode.store((g_carMode.load()+MODE_MOTION+(direction<0?-1:1))%MODE_MOTION);
    Save(); ResetInteraction(); LOGI("[driving] car mode -> %s",GetCarModeName());
}
void CycleBikeMode(int direction) {
    EnsureInit(); if (direction==0) return;
    if (!kImmersiveDrivingEnabled) {
        g_bikeMode.store(MODE_DEFAULT);
        ResetInteraction();
        return;
    }
    g_bikeMode.store((g_bikeMode.load()+MODE_MOTION+(direction<0?-1:1))%MODE_MOTION);
    Save(); ResetInteraction(); LOGI("[driving] bike mode -> %s",GetBikeModeName());
}

const char* VehicleTypeName(int vehicleType) {
    switch (vehicleType) {
        case VEHICLE_BIKE:  return "BIKE";
        case VEHICLE_PLANE: return "AIRCRAFT";
        case VEHICLE_BOAT:  return "BOAT";
        default:            return "CAR";
    }
}
int GetModeForVehicleType(int vehicleType) {
    switch (vehicleType) {
        case VEHICLE_BIKE:  return GetBikeMode();
        case VEHICLE_PLANE: EnsureInit(); return g_planeMode.load();
        case VEHICLE_BOAT:  EnsureInit(); return g_boatMode.load();
        default:            return GetCarMode();
    }
}
const char* GetModeNameForVehicleType(int vehicleType) {
    return ModeName(GetModeForVehicleType(vehicleType));
}
void CycleModeForVehicleType(int vehicleType,int direction) {
    if (vehicleType==VEHICLE_BIKE) { CycleBikeMode(direction); return; }
    if (vehicleType==VEHICLE_PLANE) {
        EnsureInit(); if (direction==0) return;
        if (!kImmersiveDrivingEnabled) { g_planeMode.store(MODE_DEFAULT); return; }
        g_planeMode.store((g_planeMode.load()+MODE_MOTION+
                           (direction<0?-1:1))%MODE_MOTION);
        Save(); ResetInteraction();
        LOGI("[driving] plane mode -> %s",
             g_planeMode.load()==MODE_IMMERSIVE?"IMMERSIVE":"DEFAULT");
        return;
    }
    if (vehicleType==VEHICLE_BOAT) {
        EnsureInit(); if (direction==0) return;
        if (!kImmersiveDrivingEnabled) { g_boatMode.store(MODE_DEFAULT); return; }
        g_boatMode.store((g_boatMode.load()+MODE_MOTION+
                          (direction<0?-1:1))%MODE_MOTION);
        Save(); ResetInteraction();
        LOGI("[driving] boat mode -> %s",
             g_boatMode.load()==MODE_IMMERSIVE?"IMMERSIVE":"DEFAULT");
        return;
    }
    CycleCarMode(direction);
}

int GetCameraViewForVehicleType(int vehicleType) {
    EnsureInit();
    switch (vehicleType) {
        case VEHICLE_BIKE:  return g_bikeView.load();
        case VEHICLE_PLANE: return g_airView.load();
        case VEHICLE_BOAT:  return g_boatView.load();
        default:            return g_carView.load();
    }
}
const char* GetCameraViewName(int vehicleType) {
    return GetCameraViewForVehicleType(vehicleType)==VIEW_THIRD_PERSON?
        "THIRD PERSON":"FIRST PERSON";
}
void CycleCameraView(int vehicleType,int direction) {
    EnsureInit(); if (direction==0) return;
    auto& view=vehicleType==VEHICLE_BIKE?g_bikeView:
               vehicleType==VEHICLE_PLANE?g_airView:
               vehicleType==VEHICLE_BOAT?g_boatView:g_carView;
    view.store((view.load()+VIEW_MODE_COUNT+(direction<0?-1:1))%VIEW_MODE_COUNT);
    Save();
    LOGI("[driving] %s camera view -> %s",VehicleTypeName(vehicleType),
         GetCameraViewName(vehicleType));
}
int GetAirView() { EnsureInit(); return g_airView.load(); }
const char* GetAirViewName() {
    return GetAirView()==VIEW_THIRD_PERSON?"THIRD PERSON":"FIRST PERSON";
}
void CycleAirView(int direction) {
    EnsureInit(); if (direction==0) return;
    g_airView.store((g_airView.load()+VIEW_MODE_COUNT+
                     (direction<0?-1:1))%VIEW_MODE_COUNT);
    Save();
    LOGI("[driving] aircraft camera view -> %s",GetAirViewName());
}

bool ThirdPersonViewActive() {
    EnsureInit();
    const int appearance=
        g_activeVehicleAppearance.load(std::memory_order_acquire);
    if (appearance<=0) return false;
    if (appearance==1) {
        if (g_activeVehicleModel.load(std::memory_order_acquire)==
            kRhinoModelId)
            return true;
        return g_carView.load()==VIEW_THIRD_PERSON;
    }
    if (appearance==2) return g_bikeView.load()==VIEW_THIRD_PERSON;
    if (appearance==3||appearance==5)
        return g_airView.load()==VIEW_THIRD_PERSON;
    if (appearance==4) return g_boatView.load()==VIEW_THIRD_PERSON;
    return false;
}

int GetBicycleImmersiveMode() {
    EnsureInit(); return g_bicycleMode.load(std::memory_order_acquire);
}
const char* GetBicycleImmersiveModeName() {
    return GetBicycleImmersiveMode()==BICYCLE_HANDLEBAR_TRIGGER?
        "HANDLEBAR + R2":"MOTION";
}
void CycleBicycleImmersiveMode(int direction) {
    EnsureInit();
    (void)direction;
    g_bicycleMode.store(BICYCLE_HANDLEBAR_TRIGGER,std::memory_order_release);
}

int GetCurrentBikeAcceleratorMode() {
    EnsureInit();
    const int model=GetActiveVehicleModelId();
    if (model<0) return BIKE_ACCEL_HOLD_TRIGGER;
    std::lock_guard<std::mutex> lock(g_modelMutex);
    const auto found=g_modelCalibration.find(model);
    return found==g_modelCalibration.end()?BIKE_ACCEL_HOLD_TRIGGER:
        found->second.acceleratorMode;
}
const char* GetCurrentBikeAcceleratorModeName() {
    if (IsBicycleModel(GetActiveVehicleModelId())) return "HOLD R2";
    if (GetCurrentBikeAcceleratorMode()==BIKE_ACCEL_HOLD_TRIGGER)
        return "HOLD R2";
    return "TWIST GRIP";
}
void CycleCurrentBikeAcceleratorMode(int direction) {
    EnsureInit(); if (!direction||!HasCurrentModelForType(VEHICLE_BIKE)) return;
    const int model=GetActiveVehicleModelId();
    if (IsBicycleModel(model)) return;
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        int& mode=g_modelCalibration[model].acceleratorMode;
        mode=(mode+BIKE_ACCEL_MODE_COUNT+(direction<0?-1:1))%
            BIKE_ACCEL_MODE_COUNT;
    }
    Save(); ResetInteraction();
    LOGI("[driving] model %d accelerator -> %s",model,
         GetCurrentBikeAcceleratorModeName());
}

namespace {
// Row tables per vehicle type and mode. Planes are DEFAULT-only; boats get
// the full VC set with the immersive helm wheel.
constexpr int kDefaultBikeItems[] = {
    MENU_VEHICLE_TYPE, MENU_DRIVING_TYPE, MENU_CAMERA_VIEW,
    MENU_GLOBAL_SIDE,
    MENU_GLOBAL_FORWARD, MENU_GLOBAL_HEIGHT,
    MENU_MODEL_SIDE,
    MENU_MODEL_FORWARD, MENU_MODEL_HEIGHT,
    MENU_KEEP_RIDER_ON_FLIPS, MENU_DRIVEBY_AIM, MENU_RESET, MENU_BACK
};
constexpr int kDefaultCarItems[] = {
    MENU_VEHICLE_TYPE, MENU_DRIVING_TYPE, MENU_CAMERA_VIEW,
    MENU_CAR_CAMERA_TILT,
    MENU_GLOBAL_SIDE, MENU_GLOBAL_FORWARD, MENU_GLOBAL_HEIGHT,
    MENU_MODEL_SIDE, MENU_MODEL_FORWARD, MENU_MODEL_HEIGHT,
    MENU_INTERIOR_GLASS, MENU_DRIVEBY_AIM, MENU_RESET, MENU_BACK
};
constexpr int kImmersiveBikeItems[] = {
    MENU_VEHICLE_TYPE, MENU_DRIVING_TYPE, MENU_CAMERA_VIEW,
    MENU_BIKE_ACCELERATOR,
    MENU_GLOBAL_SIDE, MENU_GLOBAL_FORWARD, MENU_GLOBAL_HEIGHT,
    MENU_MODEL_SIDE, MENU_MODEL_FORWARD, MENU_MODEL_HEIGHT,
    MENU_CONTROL_CALIBRATION, MENU_HANDLE_HIGHLIGHTS,
    MENU_BIKE_HAND_TILT, MENU_LOCAL_HORIZON, MENU_BIKE_VISUAL_LEAN,
    MENU_KEEP_RIDER_ON_FLIPS, MENU_RESET, MENU_BACK
};
constexpr int kImmersiveCarItems[] = {
    MENU_VEHICLE_TYPE, MENU_DRIVING_TYPE, MENU_CAMERA_VIEW,
    MENU_CAR_CAMERA_TILT,
    MENU_GLOBAL_SIDE, MENU_GLOBAL_FORWARD, MENU_GLOBAL_HEIGHT,
    MENU_MODEL_SIDE, MENU_MODEL_FORWARD, MENU_MODEL_HEIGHT,
    MENU_CONTROL_CALIBRATION, MENU_HANDLE_HIGHLIGHTS,
    MENU_WHEEL_VISIBLE, MENU_INTERIOR_GLASS, MENU_RESET, MENU_BACK
};
constexpr int kDefaultPlaneItems[] = {
    MENU_VEHICLE_TYPE, MENU_DRIVING_TYPE, MENU_CAMERA_VIEW,
    MENU_GLOBAL_SIDE, MENU_GLOBAL_FORWARD, MENU_GLOBAL_HEIGHT,
    MENU_MODEL_SIDE, MENU_MODEL_FORWARD, MENU_MODEL_HEIGHT,
    MENU_INTERIOR_GLASS, MENU_RESET, MENU_BACK
};
constexpr int kImmersivePlaneItems[] = {
    MENU_VEHICLE_TYPE, MENU_DRIVING_TYPE, MENU_CAMERA_VIEW,
    MENU_GLOBAL_SIDE, MENU_GLOBAL_FORWARD, MENU_GLOBAL_HEIGHT,
    MENU_MODEL_SIDE, MENU_MODEL_FORWARD, MENU_MODEL_HEIGHT,
    MENU_CONTROL_CALIBRATION, MENU_YOKE_SENSITIVITY,
    MENU_HANDLE_HIGHLIGHTS,
    MENU_WHEEL_VISIBLE, MENU_INTERIOR_GLASS, MENU_RESET, MENU_BACK
};
constexpr int kDefaultBoatItems[] = {
    MENU_VEHICLE_TYPE, MENU_DRIVING_TYPE, MENU_CAMERA_VIEW,
    MENU_BOAT_CAMERA_TILT,
    MENU_GLOBAL_SIDE, MENU_GLOBAL_FORWARD, MENU_GLOBAL_HEIGHT,
    MENU_MODEL_SIDE, MENU_MODEL_FORWARD, MENU_MODEL_HEIGHT,
    MENU_RESET, MENU_BACK
};
constexpr int kImmersiveBoatItems[] = {
    MENU_VEHICLE_TYPE, MENU_DRIVING_TYPE, MENU_CAMERA_VIEW,
    MENU_BOAT_CAMERA_TILT,
    MENU_GLOBAL_SIDE, MENU_GLOBAL_FORWARD, MENU_GLOBAL_HEIGHT,
    MENU_MODEL_SIDE, MENU_MODEL_FORWARD, MENU_MODEL_HEIGHT,
    MENU_CONTROL_CALIBRATION, MENU_HANDLE_HIGHLIGHTS,
    MENU_WHEEL_VISIBLE, MENU_RESET, MENU_BACK
};

void MenuTableForType(int vehicleType, const int** items, int* count) {
    const bool immersive =
        GetModeForVehicleType(vehicleType) == MODE_IMMERSIVE;
    switch (vehicleType) {
        case VEHICLE_BIKE:
            *items = immersive ? kImmersiveBikeItems : kDefaultBikeItems;
            *count = immersive ? static_cast<int>(std::size(kImmersiveBikeItems))
                               : static_cast<int>(std::size(kDefaultBikeItems));
            return;
        case VEHICLE_PLANE:
            *items = immersive ? kImmersivePlaneItems : kDefaultPlaneItems;
            *count = immersive
                ? static_cast<int>(std::size(kImmersivePlaneItems))
                : static_cast<int>(std::size(kDefaultPlaneItems));
            return;
        case VEHICLE_BOAT:
            *items = immersive ? kImmersiveBoatItems : kDefaultBoatItems;
            *count = immersive ? static_cast<int>(std::size(kImmersiveBoatItems))
                               : static_cast<int>(std::size(kDefaultBoatItems));
            return;
        default:
            *items = immersive ? kImmersiveCarItems : kDefaultCarItems;
            *count = immersive ? static_cast<int>(std::size(kImmersiveCarItems))
                               : static_cast<int>(std::size(kDefaultCarItems));
            return;
    }
}
} // namespace

int GetMenuItemCount(int vehicleType) {
    const int* items=nullptr; int count=0;
    MenuTableForType(vehicleType,&items,&count);
    return count;
}
int GetMenuItemForRow(int vehicleType,int row) {
    const int* items=nullptr; int count=0;
    MenuTableForType(vehicleType,&items,&count);
    if (row<0||row>=count) return MENU_BACK;
    return items[row];
}

int GetActiveVehicleType() {
    EnsureInit(); return g_activeVehicleType.load(std::memory_order_acquire);
}
int GetActiveVehicleModelId() {
    EnsureInit(); return g_activeVehicleModel.load(std::memory_order_acquire);
}
bool HasCurrentModelForType(int vehicleType) {
    return GetActiveVehicleType()==vehicleType&&GetActiveVehicleModelId()>=0;
}
const char* GetActiveVehicleModelName() {
    switch(GetActiveVehicleModelId()) {
        case 448:return "PIZZABOY";
        case 461:return "PCJ-600";
        case 462:return "FAGGIO";
        case 463:return "FREEWAY";
        case 468:return "SANCHEZ";
        case 481:return "BMX";
        case 509:return "BIKE";
        case 510:return "MOUNTAIN BIKE";
        case 521:return "FCR-900";
        case 522:return "NRG-500";
        case 523:return "HPV1000";
        case 581:return "BF-400";
        case 586:return "WAYFARER";
        default:return GetActiveVehicleModelId()>=0?"MODEL":"NO VEHICLE";
    }
}
bool IsMenuItemAvailable(int vehicleType,int item) {
    switch(item) {
        case MENU_MODEL_SIDE:
        case MENU_MODEL_FORWARD:
        case MENU_MODEL_HEIGHT:
            return HasCurrentModelForType(vehicleType);
        case MENU_CONTROL_CALIBRATION:
            return GetModeForVehicleType(vehicleType)==MODE_IMMERSIVE&&
                   HasCurrentModelForType(vehicleType)&&
                   (!IsBicycleModel(GetActiveVehicleModelId())||
                    GetBicycleImmersiveMode()==BICYCLE_HANDLEBAR_TRIGGER);
        case MENU_BIKE_ACCELERATOR:
            return HasCurrentModelForType(VEHICLE_BIKE);
        default:
            return true;
    }
}

int GetGlobalSeatForwardCm(int vehicleType) {
    EnsureInit();
    return g_value[GlobalSeatField(vehicleType,
        GetModeForVehicleType(vehicleType),false)].load();
}
int GetGlobalSeatSideCm(int vehicleType) {
    EnsureInit();
    return g_value[GlobalSeatSideField(
        vehicleType, GetModeForVehicleType(vehicleType))].load();
}
int GetGlobalSeatHeightCm(int vehicleType) {
    EnsureInit();
    return g_value[GlobalSeatField(vehicleType,
        GetModeForVehicleType(vehicleType),true)].load();
}
void AdjustGlobalSeatForwardCm(int vehicleType,int direction) {
    EnsureInit(); if (!direction) return;
    const int field=GlobalSeatField(vehicleType,
        GetModeForVehicleType(vehicleType),false);
    g_value[field].store(ClampField(field,g_value[field].load()+(direction<0?-1:1)));
    Save();
}
void AdjustGlobalSeatHeightCm(int vehicleType,int direction) {
    EnsureInit(); if (!direction) return;
    const int field=GlobalSeatField(vehicleType,
        GetModeForVehicleType(vehicleType),true);
    g_value[field].store(ClampField(field,g_value[field].load()+(direction<0?-1:1)));
    Save();
}
void AdjustGlobalSeatSideCm(int vehicleType,int direction) {
    EnsureInit(); if (!direction) return;
    const int field=GlobalSeatSideField(
        vehicleType, GetModeForVehicleType(vehicleType));
    g_value[field].store(ClampField(
        field, g_value[field].load()+(direction<0?-1:1)));
    Save();
}

int GetCurrentModelSeatSideCm(int vehicleType) {
    if (!HasCurrentModelForType(vehicleType)) return 0;
    const int model=GetActiveVehicleModelId();
    const int mode=GetModeForVehicleType(vehicleType);
    std::lock_guard<std::mutex> lock(g_modelMutex);
    const auto found=g_modelCalibration.find(model);
    return found==g_modelCalibration.end()?0:found->second.seatSide[mode];
}
int GetCurrentModelSeatForwardCm(int vehicleType) {
    if (!HasCurrentModelForType(vehicleType)) return 0;
    const int model=GetActiveVehicleModelId();
    const int mode=GetModeForVehicleType(vehicleType);
    std::lock_guard<std::mutex> lock(g_modelMutex);
    const auto found=g_modelCalibration.find(model);
    return found==g_modelCalibration.end()?0:found->second.seatForward[mode];
}
int GetCurrentModelSeatHeightCm(int vehicleType) {
    if (!HasCurrentModelForType(vehicleType)) return 0;
    const int model=GetActiveVehicleModelId();
    const int mode=GetModeForVehicleType(vehicleType);
    std::lock_guard<std::mutex> lock(g_modelMutex);
    const auto found=g_modelCalibration.find(model);
    return found==g_modelCalibration.end()?0:found->second.seatHeight[mode];
}
void AdjustCurrentModelSeatSideCm(int vehicleType,int direction) {
    if (!direction||!HasCurrentModelForType(vehicleType)) return;
    const int model=GetActiveVehicleModelId();
    const int mode=GetModeForVehicleType(vehicleType);
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        int& value=g_modelCalibration[model].seatSide[mode];
        value=ClampModelSeat(false,value+(direction<0?-1:1));
    }
    Save();
}
void AdjustCurrentModelSeatForwardCm(int vehicleType,int direction) {
    if (!direction||!HasCurrentModelForType(vehicleType)) return;
    const int model=GetActiveVehicleModelId();
    const int mode=GetModeForVehicleType(vehicleType);
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        int& value=g_modelCalibration[model].seatForward[mode];
        value=ClampModelSeat(false,value+(direction<0?-1:1));
    }
    Save();
}
void AdjustCurrentModelSeatHeightCm(int vehicleType,int direction) {
    if (!direction||!HasCurrentModelForType(vehicleType)) return;
    const int model=GetActiveVehicleModelId();
    const int mode=GetModeForVehicleType(vehicleType);
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        int& value=g_modelCalibration[model].seatHeight[mode];
        value=ClampModelSeat(true,value+(direction<0?-1:1));
    }
    Save();
}

int GetActiveSeatForwardCm() {
    const int vehicleType=GetActiveVehicleType();
    if (vehicleType==VEHICLE_NONE) return 0;
    return GetGlobalSeatForwardCm(vehicleType)+
           GetCurrentModelSeatForwardCm(vehicleType);
}
int GetActiveSeatSideCm() {
    const int vehicleType=GetActiveVehicleType();
    if (vehicleType==VEHICLE_NONE) return 0;
    return GetGlobalSeatSideCm(vehicleType)+
           GetCurrentModelSeatSideCm(vehicleType);
}
int GetActiveSeatHeightCm() {
    const int vehicleType=GetActiveVehicleType();
    if (vehicleType==VEHICLE_NONE) return 0;
    return GetGlobalSeatHeightCm(vehicleType)+
           GetCurrentModelSeatHeightCm(vehicleType);
}

bool IsControlCalibrationAvailable(int vehicleType) {
    return GetModeForVehicleType(vehicleType)==MODE_IMMERSIVE&&
           HasCurrentModelForType(vehicleType);
}
int GetControlCalibrationValue(int hand,int field) {
    if (hand<0||hand>1||field<0||field>=CONTROL_FIELD_COUNT) return 0;
    const int model=GetActiveVehicleModelId();
    if (model<0) return 0;
    std::lock_guard<std::mutex> lock(g_modelMutex);
    const auto found=g_modelCalibration.find(model);
    return found==g_modelCalibration.end()?0:
        found->second.control[hand][field];
}
void AdjustControlCalibrationValue(int hand,int field,int direction) {
    if (!direction||hand<0||hand>1||field<0||field>=CONTROL_FIELD_COUNT)
        return;
    const int vehicleType=GetActiveVehicleType();
    if (!IsControlCalibrationAvailable(vehicleType)) return;
    const int model=GetActiveVehicleModelId();
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        int& value=g_modelCalibration[model].control[hand][field];
        value=ClampControlValue(field,value+(direction<0?-1:1));
    }
    Save(); ResetInteraction();
}
const char* ControlCalibrationFieldName(int field) {
    static const char* const names[CONTROL_FIELD_COUNT] = {
        "LOCAL X OFFSET", "LOCAL Y OFFSET", "LOCAL Z OFFSET",
        "LOCAL ROT X", "LOCAL ROT Y", "LOCAL ROT Z"
    };
    return field>=0&&field<CONTROL_FIELD_COUNT?names[field]:"CONTROL";
}

int GetWheelCalibrationValue(int field) {
    if (field<0||field>=WHEEL_CAL_FIELD_COUNT) return 0;
    const int model=GetActiveVehicleModelId();
    if (model<0) return 0;
    std::lock_guard<std::mutex> lock(g_modelMutex);
    const auto found=g_modelCalibration.find(model);
    return found==g_modelCalibration.end()?0:found->second.wheel[field];
}
void AdjustWheelCalibrationValue(int field,int direction) {
    if (!direction||field<0||field>=WHEEL_CAL_FIELD_COUNT) return;
    const int model=GetActiveVehicleModelId();
    if (model<0) return;
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        int& value=g_modelCalibration[model].wheel[field];
        value=ClampWheelCalibration(field,value+(direction<0?-1:1));
    }
    Save();
}
int GetYokeSensitivityPercent() {
    EnsureInit(); return g_yokeSensitivity.load();
}
void AdjustYokeSensitivity(int direction) {
    EnsureInit(); if (!direction) return;
    g_yokeSensitivity.store(std::clamp(
        g_yokeSensitivity.load()+(direction<0?-10:10),25,200));
    Save();
}

const char* WheelCalibrationFieldName(int field) {
    static const char* const names[WHEEL_CAL_FIELD_COUNT] = {
        "WHEEL SIDE", "WHEEL FORWARD", "WHEEL HEIGHT", "WHEEL RADIUS",
        "WHEEL PITCH", "WHEEL YAW", "WHEEL ROLL"
    };
    return field>=0&&field<WHEEL_CAL_FIELD_COUNT?names[field]:"WHEEL";
}

void ResetVehiclePreset(int vehicleType) {
    EnsureInit();
    const int mode=GetModeForVehicleType(vehicleType);
    const int sideField=GlobalSeatSideField(vehicleType,mode);
    const int forwardField=GlobalSeatField(vehicleType,mode,false);
    const int heightField=GlobalSeatField(vehicleType,mode,true);
    g_value[sideField].store(kDefaults[sideField]);
    g_value[forwardField].store(kDefaults[forwardField]);
    g_value[heightField].store(kDefaults[heightField]);
    if (vehicleType==VEHICLE_BIKE)
        g_bikeVisualLeanPercent.store(50,std::memory_order_release);
    if (HasCurrentModelForType(vehicleType)) {
        const int model=GetActiveVehicleModelId();
        std::lock_guard<std::mutex> lock(g_modelMutex);
        ModelCalibration& calibration=g_modelCalibration[model];
        calibration.seatSide[mode]=0;
        calibration.seatForward[mode]=0;
        calibration.seatHeight[mode]=0;
        if (mode==MODE_IMMERSIVE)
            std::memset(calibration.control,0,sizeof(calibration.control));
        calibration.acceleratorMode=BIKE_ACCEL_HOLD_TRIGGER;
    }
    Save(); ResetInteraction();
}
bool IsWheelVisible() { EnsureInit(); return g_wheelVisible.load(); }
void ToggleWheelVisible() { EnsureInit(); g_wheelVisible.store(!g_wheelVisible.load()); Save(); }
bool AreHandleHighlightsEnabled() { EnsureInit(); return g_highlights.load(); }
void ToggleHandleHighlights() { EnsureInit(); g_highlights.store(!g_highlights.load()); Save(); }
bool DoBikeHandsFollowTilt() { EnsureInit(); return g_bikeHandsFollowTilt.load(); }
void ToggleBikeHandsFollowTilt() {
    EnsureInit(); g_bikeHandsFollowTilt.store(!g_bikeHandsFollowTilt.load()); Save();
}
bool IsBikeHorizonLocked() { EnsureInit(); return g_bikeHorizonLocked.load(); }
void ToggleBikeHorizonLock() {
    EnsureInit(); g_bikeHorizonLocked.store(!g_bikeHorizonLocked.load()); Save();
}
int GetBikeVisualLeanPercent() {
    EnsureInit();
    return g_bikeVisualLeanPercent.load(std::memory_order_acquire);
}
void AdjustBikeVisualLeanPercent(int direction) {
    EnsureInit(); if (!direction) return;
    g_bikeVisualLeanPercent.store(std::clamp(
        g_bikeVisualLeanPercent.load(std::memory_order_acquire)+
            (direction<0?-5:5),25,100),std::memory_order_release);
    Save();
}
bool KeepRiderOnFlipsEnabled() {
    EnsureInit(); return g_keepRiderOnFlips.load(std::memory_order_acquire);
}
void ToggleKeepRiderOnFlips() {
    EnsureInit();
    g_keepRiderOnFlips.store(!g_keepRiderOnFlips.load(std::memory_order_acquire),
                             std::memory_order_release);
    Save();
}
bool GetActiveBikeVisualBasis(float right[3],float forward[3],float up[3]) {
    if (!right||!forward||!up) return false;
    void* bike=nullptr;
    if (!GetActivePlayerBike(&bike,nullptr)) return false;
    V3 r{},f{},u{},p{};
    if (!ReadScaledBikeLeanFrame(bike,&r,&f,&u,&p)) return false;
    right[0]=r.x; right[1]=r.y; right[2]=r.z;
    forward[0]=f.x; forward[1]=f.y; forward[2]=f.z;
    up[0]=u.x; up[1]=u.y; up[2]=u.z;
    return true;
}
bool IsInteriorGlassHidden() {
    EnsureInit(); return g_hideInteriorGlass.load(std::memory_order_acquire);
}
void ToggleInteriorGlass() {
    EnsureInit();
    g_hideInteriorGlass.store(!g_hideInteriorGlass.load(),
                              std::memory_order_release);
    Save();
}
bool CarCameraTiltEnabled() {
    EnsureInit(); return g_carCameraTilt.load(std::memory_order_acquire);
}
void ToggleCarCameraTilt() {
    EnsureInit();
    g_carCameraTilt.store(!g_carCameraTilt.load(), std::memory_order_release);
    Save();
    LOGI("[driving] car camera -> %s",
         g_carCameraTilt.load() ? "FULL TILT" : "LEVEL");
}
bool BoatCameraTiltEnabled() {
    EnsureInit(); return g_boatCameraTilt.load(std::memory_order_acquire);
}
void ToggleBoatCameraTilt() {
    EnsureInit();
    g_boatCameraTilt.store(!g_boatCameraTilt.load(),
                           std::memory_order_release);
    Save();
    LOGI("[driving] boat camera -> %s",
         g_boatCameraTilt.load() ? "WITH BOAT" : "LEVEL");
}
bool IsDrivebyAimImmersive() {
    EnsureInit();
    return g_drivebyImmersive.load(std::memory_order_acquire);
}
void ToggleDrivebyAimImmersive() {
    EnsureInit();
    g_drivebyImmersive.store(!g_drivebyImmersive.load(),
                             std::memory_order_release);
    Save();
}
bool VehicleWeaponsImmersive() {
    EnsureInit();
    return GetMode()==MODE_IMMERSIVE ||
           g_drivebyImmersive.load(std::memory_order_acquire);
}
bool ShouldUseTrackedHands() {
    EnsureInit();
    if (!HasPlayerVehicle()) return true;
    return ModeForAppearance(GetActiveVehicleAppearance(false))==MODE_IMMERSIVE;
}
void SetMenuPreview(bool active,int calibrationHand) {
    g_menuPreview.store(active, std::memory_order_release);
    g_menuCalibrationHand.store(active?calibrationHand:-1,
                                std::memory_order_release);
}

bool MotionHeading(float* heading) {
    xr::HandPose poses[2]{};
    if (!physicalweapon::GetHandPosesSnapshot(poses)) return false;
    const int hand = std::clamp(g_motionHand.load(), 0, 1);
    if (!poses[hand].valid || !poses[hand].aimValid) return false;
    const float x = poses[hand].aimOri[0];
    const float y = poses[hand].aimOri[1];
    const float z = poses[hand].aimOri[2];
    const float w = poses[hand].aimOri[3];
    const float forwardX = -2.0f * (x * z + w * y);
    const float forwardZ = -(1.0f - 2.0f * (x * x + y * y));
    if (forwardX * forwardX + forwardZ * forwardZ < 0.01f) return false;
    *heading = std::atan2(forwardX, -forwardZ);
    return true;
}

float WrapAngleRad(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

void UpdateMotionSteering(const xr::InputState& input, bool active) {
    const int appearance = GetActiveVehicleAppearance();
    void* vehicle = g.FindPlayerVehicle ? g.FindPlayerVehicle(-1,false)
                                        : nullptr;
    const bool engaged = active && vehicle != nullptr &&
        (appearance == 1 || appearance == 2) &&
        ModeForAppearance(appearance) == MODE_MOTION;
    g_motionEngaged.store(engaged, std::memory_order_relaxed);
    if (!engaged) {
        g_motionVehicle = nullptr;
        g_motionRefValid = false;
        g_motionSteering.store(0.0f, std::memory_order_relaxed);
        return;
    }
    if (g_motionVehicle != vehicle) {
        g_motionVehicle = vehicle;
        g_motionRefValid = false;
        g_motionSteering.store(0.0f, std::memory_order_relaxed);
    }
    float heading = 0.0f;
    if (!MotionHeading(&heading)) {
        g_motionSteering.store(0.0f, std::memory_order_relaxed);
        return;
    }
    if (!g_motionRefValid) {
        g_motionSteering.store(0.0f, std::memory_order_relaxed);
        // The neutral reference is the hand's yaw at the first throttle
        // squeeze - exactly the Vice City behaviour.
        if (input.triggers[1] < 0.15f) return;
        g_motionRefHeading = heading;
        g_motionRefValid = true;
    }
    constexpr float kMaxAngle = 90.0f * 3.14159265f / 180.0f;
    constexpr float kMidAngle = 30.0f * 3.14159265f / 180.0f;
    constexpr float kFineScale = 0.5f;
    float angle = std::clamp(
        -WrapAngleRad(heading - g_motionRefHeading), -kMaxAngle, kMaxAngle);
    if (std::fabs(angle) < 3.0f * 3.14159265f / 180.0f) angle = 0.0f;
    const float absolute = std::fabs(angle);
    float steering;
    if (absolute <= kMidAngle) {
        steering = angle / kMidAngle * kFineScale;
    } else {
        const float sign = angle >= 0.0f ? 1.0f : -1.0f;
        steering = sign * (kFineScale +
            (absolute - kMidAngle) / (kMaxAngle - kMidAngle) *
            (1.0f - kFineScale));
    }
    g_motionSteering.store(
        std::fabs(steering) < 0.01f ? 0.0f
                                    : std::clamp(steering, -1.0f, 1.0f),
        std::memory_order_relaxed);
    static double s_motionLogAt = 0.0;
    timespec logTs{};
    clock_gettime(CLOCK_MONOTONIC, &logTs);
    const double logNow = logTs.tv_sec + logTs.tv_nsec * 1e-9;
    if (logNow - s_motionLogAt > 1.0) {
        s_motionLogAt = logNow;
        LOGI("[driving] motion steer=%.2f angle=%.1fdeg ref=%d",
             g_motionSteering.load(std::memory_order_relaxed),
             angle * 57.2958f, g_motionRefValid ? 1 : 0);
    }
}

bool MotionSteeringActive() {
    return g_motionEngaged.load(std::memory_order_relaxed);
}
float MotionSteeringValue() {
    return g_motionSteering.load(std::memory_order_relaxed);
}
int GetMotionSteeringHand() { EnsureInit(); return g_motionHand.load(); }
void ToggleMotionSteeringHand() {
    EnsureInit();
    g_motionHand.store(1 - g_motionHand.load());
    g_motionRefValid = false;
    Save();
    LOGI("[driving] motion steering hand -> %s",
         g_motionHand.load() ? "RIGHT" : "LEFT");
}

void UpdateInput(const xr::InputState& input,bool gameplay,bool blocked) {
    // Controller-yaw MOTION steering is intentionally unavailable in this
    // release. Stick and physical cockpit modes remain the only driving paths.
    g_motionEngaged.store(false,std::memory_order_relaxed);
    g_motionSteering.store(0.0f,std::memory_order_relaxed);
    EnsureInit(); g_inputBlocked.store(blocked,std::memory_order_release);
    const bool radioAllowed = gameplay && !blocked && HasPlayerVehicle();
    const bool radioDown = radioAllowed && input.x;
    const bool wasRadioDown = g_radioButtonDown.exchange(
        radioDown, std::memory_order_acq_rel);
    if (radioDown && !wasRadioDown) {
        // Plain X = next station; X with the LEFT grip squeezed = previous.
        if (input.grip[0] >= 0.55f)
            g_radioPrevJustPressed.store(true, std::memory_order_release);
        else
            g_radioChangeJustPressed.store(true, std::memory_order_release);
    } else if (!radioAllowed) {
        g_radioChangeJustPressed.store(false, std::memory_order_release);
        g_radioPrevJustPressed.store(false, std::memory_order_release);
    }

    // Visible radio navigation (player request): show the station name in
    // the TIMERS text layer for 3 s whenever the station changes.
    {
        static const char* const kStationNames[] = {
            "RADIO OFF",           // 0 (unused placeholder)
            "PLAYBACK FM",         // 1
            "K-ROSE",              // 2
            "K-DST",               // 3
            "BOUNCE FM",           // 4
            "SF-UR",               // 5
            "RADIO LOS SANTOS",    // 6
            "RADIO X",             // 7
            "CSR 103.9",           // 8
            "K-JAH WEST",          // 9
            "MASTER SOUNDS 98.3",  // 10
            "WCTR",                // 11
            "USER TRACKS",         // 12
        };
        double toastNow = 0.0;
        {
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            toastNow = ts.tv_sec + ts.tv_nsec * 1e-9;
        }
        int station = -999;
        if (radioAllowed && g.AudioEngine &&
            g.CAudioEngine_GetCurrentRadioStationID) {
            station = g.CAudioEngine_GetCurrentRadioStationID(g.AudioEngine);
        }
        if (station != g_radioLastStation) {
            if (g_radioLastStation != -999 && station >= 0)
                g_radioToastUntil = toastNow + 3.0;
            g_radioLastStation = station;
        }
        if (g_radioToastUntil > toastNow && station >= 0) {
            const char* name = (station >= 1 && station <= 12)
                ? kStationNames[station] : "RADIO OFF";
            char line[64];
            std::snprintf(line, sizeof(line), "RADIO: %s", name);
            xr::PublishRadioText(line);
        } else {
            xr::PublishRadioText("");
        }
    }
    const int appearance=GetActiveVehicleAppearance();
    const int activeType=appearance==1?VEHICLE_CAR:
                         appearance==2?VEHICLE_BIKE:
                         (appearance==3||appearance==5)?VEHICLE_PLANE:
                         appearance==4?VEHICLE_BOAT:VEHICLE_NONE;
    int activeModel=-1;
    void* activeVehicle=nullptr;
    if (activeType!=VEHICLE_NONE&&g.FindPlayerVehicle) {
        if (void* vehicle=g.FindPlayerVehicle(-1,false)) {
            activeVehicle=vehicle;
            activeModel=*reinterpret_cast<const std::uint16_t*>(
                reinterpret_cast<const char*>(vehicle)+0x32);
            if (activeType==VEHICLE_BIKE) {
                // Android 2.11 CBike::nBikeFlags is the byte at +0x800 and
                // bit 4 is bOnSideStand (verified against ProcessControl).
                // Keep the ridden bike balanced during stationary calibration
                // instead of letting its stand pose shift all grip anchors.
                auto* bikeFlags=reinterpret_cast<std::uint8_t*>(
                    reinterpret_cast<char*>(vehicle)+0x800);
                *bikeFlags=static_cast<std::uint8_t>(*bikeFlags&~0x10u);
            }
        }
    }
    g_activeVehicleType.store(activeType,std::memory_order_release);
    g_activeVehicleModel.store(activeModel,std::memory_order_release);
    g_activeVehicleAppearance.store(appearance,std::memory_order_release);
    {
        // One frame-coherent X edge for the JumpJustDown touch-query hook.
        static bool prevJumpButton=false;
        const bool jumpButton=locomotion::ActionHeld(
            locomotion::BIND_ACT_JUMP,
            input.a,input.b,input.x,input.y,input.l3,input.r3,true);
        g_jumpEdge.store(gameplay&&!blocked&&activeType==VEHICLE_NONE&&
                             jumpButton&&!prevJumpButton,
                         std::memory_order_release);
        prevJumpButton=jumpButton;
    }
    int activeAccelerator=BIKE_ACCEL_HOLD_TRIGGER;
    if (activeType==VEHICLE_BIKE&&activeModel>=0) {
        std::lock_guard<std::mutex> modelLock(g_modelMutex);
        const auto found=g_modelCalibration.find(activeModel);
        if (found!=g_modelCalibration.end())
            activeAccelerator=found->second.acceleratorMode;
    }
    g_activeBikeAccelerator.store(activeAccelerator,std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_stateMutex);
    const bool wheelVehicleType=activeType==VEHICLE_CAR||
        activeType==VEHICLE_PLANE||activeType==VEHICLE_BOAT;
    if (!wheelVehicleType||!activeVehicle||
        (g_wheelSeatVehicle&&g_wheelSeatVehicle!=activeVehicle)) {
        ClearWheelSeatReferenceLocked();
    }
    const bool preview = blocked && g_menuPreview.load(std::memory_order_acquire);
    const int activeMode=ModeForAppearance(appearance);
    static int loggedAppearance=-1,loggedMode=-1;
    if (appearance!=loggedAppearance||activeMode!=loggedMode) {
        LOGI("[driving] active appearance=%d mode=%s gameplay=%d blocked=%d highlights=%d",
             appearance,activeMode==MODE_IMMERSIVE?"IMMERSIVE":"DEFAULT",
             gameplay,blocked,g_highlights.load());
        loggedAppearance=appearance; loggedMode=activeMode;
    }
    // Cadence was an experimental bicycle input. HANDLEBAR + R2 now maps the
    // held trigger directly in OnGetAccelerate, so keep the old pulse reset.
    UpdateBicycleTriggerCadenceLocked(input.triggers[1],false);
    UpdateInteriorGlassMaterials();
    if (!gameplay||(blocked&&!preview)||activeMode!=MODE_IMMERSIVE) {
        // No immersive interaction — but the dashboard HUD panels still need
        // the wheel/handlebar centre frame in DEFAULT driving (the Vice City
        // dashboard pattern anchors from that central point regardless of
        // mode). Publish geometry only: no active wheel, no grabs, no bar.
        // In the third-person chase view the cockpit is far from the camera:
        // dash panels anchored there would hover over the distant vehicle, so
        // publish nothing and let the HUD fall back to the hand slots.
        WheelVisualState anchor{};
        const bool anchorBuilt = gameplay && !ThirdPersonViewActive() &&
            ((appearance==1||appearance==4||appearance==5)
                           ? BuildWheelTracking(&anchor)
                           : (appearance==2 ? BuildBikeTracking(&anchor)
                                            : false));
        ClearInteractionLocked();
        if (anchorBuilt) {
            anchor.active=false;
            anchor.visible=false;
            anchor.grabbed[0]=anchor.grabbed[1]=false;
            anchor.markerVisible[0]=anchor.markerVisible[1]=false;
            anchor.physicalAngle=0.0f;
            anchor.steering=0.0f;
            anchor.throttleVisual=0.0f;
            g_visual=anchor;
        }
        return;
    }
    const bool bicycleMotion=appearance==2&&IsBicycleModel(activeModel)&&
        g_bicycleMode.load(std::memory_order_acquire)==BICYCLE_MOTION;
    if (bicycleMotion) {
        // MOTION owns neither a virtual bar nor grip sockets. It leaves both
        // tracked hands free, derives pedal cadence from their velocity, and
        // derives steering from HMD yaw just like the PC GTA SA VR mod.
        if (preview) {
            ClearInteractionLocked();
            return;
        }
        if (!g_bicycleGestureActive) ClearGrabStateLocked();
        xr::HandPose poses[2]{}; xr::GetHandPoses(poses);
        UpdateBicycleGestureLocked(poses);
        g_bikeThrottle=0.0f;
        g_bikeLean=0.0f;
        g_visual=WheelVisualState{};
        return;
    }
    g_bicycleGestureActive=false;
    g_bicycleSteering=0.0f;
    g_bicyclePedalLevel=0.0f;
    g_bicyclePedalPulse=0.0f;
    g_bicyclePoseValid=false;
    WheelVisualState visual{};
    const bool poseBuilt=(appearance==1||appearance==4||appearance==5)?
        BuildWheelTracking(&visual):
        (appearance==2?BuildBikeTracking(&visual):false);
    if (!poseBuilt) {
        static int failedAppearance=-1;
        static unsigned int failedFrames=0;
        if (failedAppearance!=appearance||(++failedFrames%300u)==1u) {
            LOGW("[driving] control pose unavailable appearance=%d mode=%s",
                 appearance,activeMode==MODE_IMMERSIVE?"IMMERSIVE":"DEFAULT");
            failedAppearance=appearance;
        }
        ClearInteractionLocked(); return;
    }
    FillHandleVisuals(&visual);
    if (preview) {
        // The driving page is also the cockpit calibration view. Keep the wheel
        // and both sockets visible while all grabs/steering remain blocked.
        ClearGrabStateLocked();
        visual.markerVisible[0]=true;
        visual.markerVisible[1]=true;
        const int calibrationHand=
            g_menuCalibrationHand.load(std::memory_order_acquire);
        if (calibrationHand>=0&&calibrationHand<2)
            visual.grabbed[calibrationHand]=true;
        g_visual=visual;
        return;
    }
    xr::HandPose poses[2]{}; xr::GetHandPoses(poses);
    const V3 center{visual.center[0],visual.center[1],visual.center[2]};
    const V3 right{visual.right[0],visual.right[1],visual.right[2]};
    const V3 up{visual.up[0],visual.up[1],visual.up[2]};
    V3 positions[2]{}; bool poseValid[2]{},justGrabbed[2]{};
    // A hand that already holds the drawn sidearm must never also take the
    // wheel/handlebar — the two grabs used to fire together when the holster
    // and the rim overlap in space. The mask is a lock-free mirror, safe to
    // read inside this state lock.
    const unsigned int weaponHands = physicalweapon::HeldHandMaskRelaxed();
    for (int hand=0;hand<2;++hand) {
        positions[hand]={poses[hand].gripPos[0],poses[hand].gripPos[1],poses[hand].gripPos[2]};
        poseValid[hand]=poses[hand].valid&&std::isfinite(positions[hand].x)&&
            std::isfinite(positions[hand].y)&&std::isfinite(positions[hand].z);
        const V3 neutral{visual.handlePosition[hand][0],
                         visual.handlePosition[hand][1],
                         visual.handlePosition[hand][2]};
        const float distance=poseValid[hand]?Length(positions[hand]-neutral):1000.0f;
        const bool holdsWeapon=(weaponHands&(1u<<hand))!=0;
        if (g_grabbed[hand]&&(!poseValid[hand]||input.grip[hand]<=0.30f||
                              holdsWeapon)) {
            g_grabbed[hand]=false; g_angleValid[hand]=false;
        }
        const float grabDistance=visual.bike?0.17f:0.23f;
        if (!g_grabbed[hand]&&poseValid[hand]&&!holdsWeapon&&
            input.grip[hand]>=0.65f&&
            !g_gripDown[hand]&&distance<=grabDistance) {
            g_grabbed[hand]=true; justGrabbed[hand]=true;
        }
        if (input.grip[hand]<=0.30f) g_gripDown[hand]=false;
        else if (input.grip[hand]>=0.65f) g_gripDown[hand]=true;
    }
    {
        bool hornPressed=false;
        if (!visual.bike) {
            const V3 hornNormal{visual.normal[0],visual.normal[1],
                                visual.normal[2]};
            const float hubRadius=std::min(0.45f*visual.radius,0.09f);
            for (int hand=0;hand<2;++hand) {
                if (!poseValid[hand]||g_grabbed[hand]) continue;
                if ((weaponHands&(1u<<hand))!=0) continue;
                const V3 delta=positions[hand]-center;
                const float dn=Dot(delta,hornNormal);
                const V3 planar=delta-hornNormal*dn;
                if (Length(planar)<=hubRadius&&std::abs(dn)<=0.05f) {
                    hornPressed=true;
                    break;
                }
            }
        }
        g_hornPressed.store(hornPressed,std::memory_order_release);
    }
    const bool left=g_grabbed[0]&&poseValid[0],rightGrabbed=g_grabbed[1]&&poseValid[1];
    const float previousAngle=(g_visual.active&&g_visual.bike==visual.bike)?
        g_visual.physicalAngle:0.0f;
    float steeringAngle=0.0f;
    if (visual.bike) {
        constexpr float bikeMax=35.0f*3.14159265358979323846f/180.0f;
        constexpr float discontinuity=45.0f*3.14159265358979323846f/180.0f;
        const V3 neutral=
            V3{visual.handlePosition[1][0],visual.handlePosition[1][1],
               visual.handlePosition[1][2]}-
            V3{visual.handlePosition[0][0],visual.handlePosition[0][1],
               visual.handlePosition[0][2]};
        const V3 normal{visual.normal[0],visual.normal[1],visual.normal[2]};
        const int mask=(left?1:0)|(rightGrabbed?2:0);
        if (mask==3) {
            g_bikeOneHandValid=false;
            const float raw=WrapAngle(
                PlanarAngle(positions[1]-positions[0],right,up)-
                PlanarAngle(neutral,right,up));
            if (!g_twoHandAngleValid||g_bikeOwnershipMask!=mask||
                justGrabbed[0]||justGrabbed[1]) {
                g_twoHandReferenceAngle=WrapAngle(raw-previousAngle);
                g_twoHandAngleValid=true;
            }
            float desired=WrapAngle(raw-g_twoHandReferenceAngle);
            if (!std::isfinite(desired)||
                std::abs(WrapAngle(desired-previousAngle))>discontinuity) {
                desired=previousAngle;
                g_twoHandReferenceAngle=WrapAngle(raw-desired);
            }
            steeringAngle=std::clamp(desired,-bikeMax,bikeMax);
            if (steeringAngle!=desired)
                g_twoHandReferenceAngle=WrapAngle(raw-steeringAngle);
        } else if (mask!=0) {
            g_twoHandAngleValid=false;
            const int hand=mask==1?0:1;
            if (!g_bikeOneHandValid||g_bikeOwnershipMask!=mask||justGrabbed[hand]) {
                g_bikeReferenceHand=positions[hand];
                g_bikeSeedChord=RotateAroundAxis(neutral,normal,previousAngle);
                g_bikeReferencePhysicalAngle=previousAngle;
                g_bikeOneHandValid=true;
            }
            const float handSign=hand==0?-2.0f:2.0f;
            const V3 actualChord=g_bikeSeedChord+
                (positions[hand]-g_bikeReferenceHand)*handSign;
            float desired=g_bikeReferencePhysicalAngle+WrapAngle(
                PlanarAngle(actualChord,right,up)-
                PlanarAngle(g_bikeSeedChord,right,up));
            bool needsRebase=false;
            if (!std::isfinite(desired)||
                std::abs(WrapAngle(desired-previousAngle))>discontinuity) {
                desired=previousAngle;
                needsRebase=true;
            }
            steeringAngle=std::clamp(desired,-bikeMax,bikeMax);
            if (needsRebase||steeringAngle!=desired) {
                g_bikeReferenceHand=positions[hand];
                g_bikeSeedChord=RotateAroundAxis(neutral,normal,steeringAngle);
                g_bikeReferencePhysicalAngle=steeringAngle;
            }
        } else {
            g_twoHandAngleValid=false;
            g_bikeOneHandValid=false;
            steeringAngle=0.0f;
        }
        g_bikeOwnershipMask=mask;
    } else {
        g_bikeOneHandValid=false;
        g_bikeOwnershipMask=0;
        for (int hand=0;hand<2;++hand) if (justGrabbed[hand]) {
            const float angle=PlanarAngle(positions[hand]-center,right,up);
            g_grabReferenceAngle[hand]=WrapAngle(angle-previousAngle);
            g_continuousAngle[hand]=previousAngle; g_angleValid[hand]=true;
        }
        if (left&&rightGrabbed) {
            const float chordAngle=PlanarAngle(positions[1]-positions[0],right,up);
            if (!g_twoHandAngleValid||justGrabbed[0]||justGrabbed[1]) {
                g_twoHandReferenceAngle=WrapAngle(chordAngle-previousAngle);
                g_twoHandContinuousAngle=previousAngle; g_twoHandAngleValid=true;
            }
            steeringAngle=UnwrapAngle(WrapAngle(chordAngle-g_twoHandReferenceAngle),g_twoHandContinuousAngle);
            g_twoHandContinuousAngle=steeringAngle;
            for (int hand=0;hand<2;++hand) {
                g_grabReferenceAngle[hand]=WrapAngle(PlanarAngle(positions[hand]-center,right,up)-steeringAngle);
                g_continuousAngle[hand]=steeringAngle; g_angleValid[hand]=true;
            }
        } else {
            g_twoHandAngleValid=false;
            for (int hand=0;hand<2;++hand) if (g_grabbed[hand]&&poseValid[hand]) {
                float angle=WrapAngle(PlanarAngle(positions[hand]-center,right,up)-g_grabReferenceAngle[hand]);
                if (g_angleValid[hand]) angle=UnwrapAngle(angle,g_continuousAngle[hand]);
                g_continuousAngle[hand]=angle; g_angleValid[hand]=true; steeringAngle=angle; break;
            }
        }
    }
    const float maxSteering=(visual.bike?35.0f:80.0f)*
        3.14159265358979323846f/180.0f;
    visual.physicalAngle=std::clamp(steeringAngle,-maxSteering,maxSteering);
    float steering=std::clamp(steeringAngle/maxSteering,-1.0f,1.0f);
    const float deadZone=visual.bike?0.035f:0.03f;
    if (std::abs(steering)<=deadZone) steering=0.0f;
    else steering=std::copysign((std::abs(steering)-deadZone)/(1.0f-deadZone),steering);
    visual.steering=steering;
    if (appearance==5) {
        // Yoke pitch: axial displacement of the grabbed hands along the wheel
        // normal (vehicle forward), referenced at grab. Pulling toward the
        // pilot (negative along the normal) is nose up. Three defences
        // against the phantom-pull feedback loop (the yaw-only tracking base
        // does not pitch with the airframe, so a climbing plane sweeps the
        // cockpit past stationary hands and reads as more pull):
        //   1) a slow leak re-zeroes the reference toward the current axial,
        //   2) a real dead zone before any pitch registers,
        //   3) travel scaled by the YOKE SENSITIVITY menu setting.
        static float yokeReferenceAxial=0.0f;
        static bool yokeAxialValid=false;
        const V3 yokeNormal{visual.normal[0],visual.normal[1],
                            visual.normal[2]};
        float axialSum=0.0f; int axialCount=0;
        for (int hand=0;hand<2;++hand) {
            if (!g_grabbed[hand]||!poseValid[hand]) continue;
            axialSum+=Dot(positions[hand]-center,yokeNormal);
            ++axialCount;
        }
        float pullOut=0.0f;
        if (axialCount>0) {
            const float axial=axialSum/static_cast<float>(axialCount);
            if (!yokeAxialValid||justGrabbed[0]||justGrabbed[1]) {
                yokeReferenceAxial=axial;
                yokeAxialValid=true;
            }
            constexpr float kDeadZoneMetres=0.03f;
            const float rawDrift=yokeReferenceAxial-axial;
            // Re-zero tracking drift ONLY near neutral. The old always-on
            // leak also decayed a deliberately HELD pull, which is why the
            // yoke felt heavy even at 200% sensitivity.
            if (std::abs(rawDrift)<kDeadZoneMetres)
                yokeReferenceAxial+=(axial-yokeReferenceAxial)*0.03f;
            const float sensitivity=std::clamp(
                g_yokeSensitivity.load(std::memory_order_acquire),25,200)/
                100.0f;
            const float fullPull=0.18f/sensitivity;
            float raw=yokeReferenceAxial-axial;
            if (std::abs(raw)<=kDeadZoneMetres) raw=0.0f;
            else raw=std::copysign(std::abs(raw)-kDeadZoneMetres,raw);
            float pull=std::clamp(raw/fullPull,-1.0f,1.0f);
            // Gentle response: precise near neutral without the heavy squared
            // falloff of the first cut.
            pull=std::copysign(std::pow(std::abs(pull),1.35f),pull);
            pullOut=pull;
        } else {
            yokeAxialValid=false;
        }
        g_planePitch.store(pullOut,std::memory_order_release);
        // Visual feedback: the whole yoke slides toward/away from the pilot
        // with the applied pitch (12cm at full deflection).
        const float visualSlide=-pullOut*0.12f;
        visual.center[0]+=yokeNormal.x*visualSlide;
        visual.center[1]+=yokeNormal.y*visualSlide;
        visual.center[2]+=yokeNormal.z*visualSlide;
    } else {
        g_planePitch.store(0.0f,std::memory_order_release);
    }
    if (visual.bike) {
        const bool bicycle=IsBicycleModel(activeModel);
        if (bicycle) {
            // HANDLEBAR + R2 steers physically, but has no motorcycle-style
            // twist throttle. Its raw R2 cadence is handled in OnGetAccelerate.
            g_bikeThrottle=0.0f;
            g_bikeThrottleGestureActive=false;
            g_bikeThrottleReferenceValid=false;
        } else if (activeAccelerator==BIKE_ACCEL_PHYSICAL_OR_TAP) {
            UpdateBikeThrottleLocked(input,poses[1],rightGrabbed);
        } else {
            g_bikeThrottle=0.0f;
            g_bikeThrottleGestureActive=false;
            g_bikeThrottleReferenceValid=false;
        }
        UpdateBikeLeanLocked(poses,left,rightGrabbed);
    }
    else {
        g_bikeThrottle=0.0f;
        g_bikeThrottleGestureActive=false;
        g_bikeThrottleReferenceValid=false;
    }
    for (int hand=0;hand<2;++hand) {
        visual.grabbed[hand]=g_grabbed[hand];
        // In IMMERSIVE the player must be able to see where to reach before the
        // controller is already close. Keep both cyan sockets visible whenever
        // handle highlights are enabled; engaged sockets turn green in Xr.
        visual.markerVisible[hand]=g_highlights.load();
    }
    FillHandleVisuals(&visual); g_visual=visual;

    // The bicycle model's front fork follows m_fSteerInput(+0x730) ->
    // m_fSteerAngle(+0x600) (CBike::ProcessControlInputs; CBmx::PreRender
    // rotates the fork node from +0x600 — disasm-verified). That chain begins
    // at the CPad steering query, and a BMX control branch can skip it, which
    // left the visible fork straight while the bar steered. Mirror Quest VC:
    // inject the normalized bar steering into the vehicle directly. When the
    // pad path also runs, it converges to exactly this value.
    if (visual.bike && IsBicycleModel(activeModel)) {
        void* bicycle=nullptr;
        if (GetActivePlayerBike(&bicycle,nullptr) && bicycle) {
            auto* bytes = static_cast<char*>(bicycle);
            const float steer = std::clamp(visual.steering,-1.0f,1.0f);
            *reinterpret_cast<float*>(bytes+0x730) = steer;
            const std::uintptr_t handling =
                *reinterpret_cast<const std::uintptr_t*>(bytes+0x4a8);
            if (handling) {
                const float maxSteerDeg =
                    *reinterpret_cast<const float*>(handling+0xa0);
                if (std::isfinite(maxSteerDeg) &&
                    std::abs(maxSteerDeg) < 90.0f) {
                    *reinterpret_cast<float*>(bytes+0x600) =
                        maxSteerDeg*(3.14159265f/180.0f)*steer;
                }
            }
        }
    }
}

void ResetInteraction() { std::lock_guard<std::mutex> lock(g_stateMutex); ClearInteractionLocked(); }
void RefreshVisualTracking() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (!g_visual.active||!g_visual.bike) return;
    const WheelVisualState previous=g_visual;
    WheelVisualState refreshed{};
    if (!BuildBikeTracking(&refreshed)) return;
    refreshed.physicalAngle=previous.physicalAngle;
    refreshed.steering=previous.steering;
    for (int hand=0;hand<2;++hand) {
        refreshed.grabbed[hand]=previous.grabbed[hand];
        refreshed.markerVisible[hand]=previous.markerVisible[hand];
    }
    FillHandleVisuals(&refreshed);
    g_visual=refreshed;
}
bool GetWheelVisualState(WheelVisualState* out) {
    if (!out) return false; std::lock_guard<std::mutex> lock(g_stateMutex);
    *out=g_visual; return out->active;
}
bool GetGrabbedHandVisual(int hand,float position[3],float right[3],float up[3],float forward[3]) {
    if (hand<0||hand>1||!position||!right||!up||!forward) return false;
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (!g_visual.active||!g_visual.grabbed[hand]) return false;
    std::memcpy(position,g_visual.handlePosition[hand],3*sizeof(float));
    std::memcpy(right,g_visual.handleRight[hand],3*sizeof(float));
    std::memcpy(up,g_visual.handleUp[hand],3*sizeof(float));
    std::memcpy(forward,g_visual.handleForward[hand],3*sizeof(float));
    return true;
}

} // namespace savr::driving
