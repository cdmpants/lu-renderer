#include "lu/renderer/camera.h"

#include <algorithm>
#include <cmath>

namespace lu::renderer {

void OrbitCamera::setMode(CameraMode mode) {
    if (mode_ == mode) return;
    if (mode == CameraMode::Fly) {
        syncFlyToOrbit();
    }
    mode_ = mode;
}

void OrbitCamera::orbit(float dx, float dy) {
    yaw_ += dx * 0.008f;
    pitch_ += dy * 0.008f;
    pitch_ = std::clamp(pitch_, -1.45f, 1.45f);
}

void OrbitCamera::pan(float dx, float dy) {
    Vec3 eye = position();
    Vec3 forward = normalize(target_ - eye);
    Vec3 right = normalize(cross({0.0f, 1.0f, 0.0f}, forward));
    Vec3 up = normalize(cross(forward, right));
    float scale = distance_ * 0.0015f;
    target_ = target_ + right * (-dx * scale) + up * (-dy * scale);
}

void OrbitCamera::zoom(float amount) {
    distance_ *= std::pow(0.88f, amount);
    distance_ = std::clamp(distance_, 0.25f, 100000.0f);
}

void OrbitCamera::flyLook(float dx, float dy) {
    yaw_ += dx * 0.004f;
    pitch_ += dy * 0.004f;
    pitch_ = std::clamp(pitch_, -1.45f, 1.45f);
}

void OrbitCamera::flyMove(Vec3 local_delta) {
    fly_position_ = fly_position_
        + right() * local_delta.x
        + up() * local_delta.y
        + forward() * local_delta.z;
}

void OrbitCamera::syncFlyToOrbit() {
    fly_position_ = position();
}

Vec3 OrbitCamera::position() const {
    if (mode_ == CameraMode::Fly) return fly_position_;

    float cp = std::cos(pitch_);
    return {
        target_.x + std::sin(yaw_) * cp * distance_,
        target_.y + std::sin(pitch_) * distance_,
        target_.z + std::cos(yaw_) * cp * distance_
    };
}

Vec3 OrbitCamera::target() const {
    if (mode_ == CameraMode::Fly) return fly_position_ + forward();
    return target_;
}

Vec3 OrbitCamera::forward() const {
    float cp = std::cos(pitch_);
    return normalize({
        -std::sin(yaw_) * cp,
        -std::sin(pitch_),
        -std::cos(yaw_) * cp
    });
}

Vec3 OrbitCamera::right() const {
    return normalize(cross({0.0f, 1.0f, 0.0f}, forward()));
}

Vec3 OrbitCamera::up() const {
    return normalize(cross(forward(), right()));
}

Mat4 OrbitCamera::viewMatrix() const {
    return look_at(position(), target(), {0.0f, 1.0f, 0.0f});
}

} // namespace lu::renderer
