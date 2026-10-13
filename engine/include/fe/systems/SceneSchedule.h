// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/systems/FrameContext.h"
#include "fe/systems/System.h"

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace fe {

class Scene;

// An ordered list of systems run once per frame.
//
// This is the smallest thing that makes the hybrid execution model real: a
// sequence of systems, run in registration order, each handed the scene. It is
// intentionally not a job graph. There is no parallelism, no declared
// read/write set, and no dependency solver, because adding those before a
// second and third system demonstrate a need would be speculative framework.
// When a real need appears, it can be met by extending this type without
// changing any system.
//
// The schedule lives with the scene, not in Engine. Engine owns no scene, so it
// cannot own the schedule either. Application creates a scene and a schedule
// and pairs them; the editor does the same, which is what makes play-in-editor
// run the user's systems through the identical path.
//
// Ordering: systems run in the order they were added. A caller that needs a
// particular order (for example animation before physics) adds them in that
// order. This is explicit and greppable; the moment ordering becomes implicit,
// debugging a frame becomes guesswork.
class SceneSchedule {
public:
    SceneSchedule() = default;

    SceneSchedule(const SceneSchedule&) = delete;
    SceneSchedule& operator=(const SceneSchedule&) = delete;

    // Adds a system of concrete type T, constructed in place. Returns a borrowed
    // pointer; ownership stays with the schedule.
    template <typename T, typename... Args>
    T* add_system(Args&&... args) {
        static_assert(std::is_base_of_v<System, T>, "T must derive from fe::System");
        auto system = std::unique_ptr<T>(new T(std::forward<Args>(args)...));
        T* raw = system.get();
        systems_.push_back(std::move(system));
        return raw;
    }

    // Adds an already-constructed system. Used when the concrete type is chosen
    // at runtime (for example a plugin supplying a system).
    System* add_system(std::unique_ptr<System> system) {
        System* raw = system.get();
        systems_.push_back(std::move(system));
        return raw;
    }

    // Runs every system once, in order.
    void update(const FrameContext& context, Scene& scene);

    [[nodiscard]] usize system_count() const noexcept { return systems_.size(); }

    [[nodiscard]] System* system_at(usize index) noexcept { return systems_[index].get(); }
    [[nodiscard]] const System* system_at(usize index) const noexcept { return systems_[index].get(); }

    // Finds the first system whose name matches, or nullptr.
    [[nodiscard]] System* find(std::string_view name) noexcept;

    // Removes every system. The schedule is reusable afterwards.
    void clear() noexcept { systems_.clear(); }

private:
    std::vector<std::unique_ptr<System>> systems_;
};

} // namespace fe
