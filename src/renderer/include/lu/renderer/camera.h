#pragma once

#include "lu/renderer/math.h"

namespace lu::renderer {

enum class CameraMode {
    Orbit,
    Fly
};

class OrbitCamera {
public:
    void setMode(CameraMode mode);
    CameraMode mode() const { return mode_; }
    void setTarget(Vec3 target) { target_ = target; }
    void setDistance(float distance) { distance_ = std::max(0.1f, distance); }
    void orbit(float dx, float dy);
    void pan(float dx, float dy);
    void zoom(float amount);
    void flyLook(float dx, float dy);
    void flyMove(Vec3 local_delta);
    void syncFlyToOrbit();

    Vec3 position() const;
    Mat4 viewMatrix() const;

    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    float distance() const { return distance_; }
    Vec3 target() const;
    Vec3 flyPosition() const { return fly_position_; }
    Vec3 forward() const;
    Vec3 right() const;
    Vec3 up() const;

private:
    CameraMode mode_ = CameraMode::Orbit;
    Vec3 target_ = {0.0f, 0.0f, 0.0f};
    Vec3 fly_position_ = {0.0f, 0.0f, 10.0f};
    float distance_ = 10.0f;
    float yaw_ = 0.75f;
    float pitch_ = 0.45f;
};

} // namespace lu::renderer
