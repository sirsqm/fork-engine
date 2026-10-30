// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/resources/Resource.h"
#include "fe/scene/Transform.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe::anim {

// A skeleton: a tree of joints plus the bind-pose data skinning needs.
//
// Why this is a resource of its own rather than part of a mesh. A mesh is
// geometry and is shared: one "character.glb" mesh can be drawn by a crowd of
// instances. A skeleton is the *rig* that drives that geometry, and it is a
// separate authored thing with its own identity. glTF agrees and models `skins`
// as a top-level array beside `meshes`, with a mesh's primitives naming the skin
// they are bound to (or none). Folding the skeleton into the mesh would make it
// impossible to share one mesh across two rigs, and would put animation state
// inside a resource the resource manager treats as immutable.
//
// Why the hierarchy is an index array rather than pointers. The skinning palette
// is computed by one forward walk over a flat array, and glTF's skeleton is
// already a tree of indices (`nodes[].children`). An index is also what survives
// serialization and what a clip's track names, so nothing here needs a stable
// address.
//
// The bind pose. Every joint carries the local transform it had when the mesh was
// bound (`rest`) and the inverse of its world matrix at that moment
// (`inverse_bind`). Skinning transforms a vertex out of mesh space and into a
// joint's space with `inverse_bind`, then back into the current pose's space with
// the joint's animated world matrix; the pair is what makes geometry follow a
// pose. glTF stores the inverse bind matrices directly, so they are read rather
// than derived. A skeleton assembled in code supplies only `rest` and calls
// `compute_inverse_bind_matrices()`.
//
// Ordering. The palette walk needs a parent resolved before its child, and glTF
// does *not* promise that `nodes` is topologically ordered. The constructor
// therefore computes `order()`: the joint indices sorted so every parent precedes
// its children. Everything downstream walks that order and is correct whatever
// order the file happened to write.
class Skeleton final : public Resource {
public:
    static constexpr const char* kTypeName = "Skeleton";

    // The parent index of a root joint.
    static constexpr i32 kNoParent = -1;

    // One joint of the rig.
    struct Joint {
        // The joint's authored name. Kept because it is the only thing that
        // survives a round trip through a format that names joints, and because
        // a tool showing a pose needs to say "hand_l" rather than "7".
        std::string name;

        // The parent joint's index, or kNoParent for a root.
        i32 parent = kNoParent;

        // The joint's local transform in the bind pose. A clip's track replaces
        // this when it animates the joint; a pose starts here for a joint the
        // clip does not touch, which is what makes a clip that animates one arm
        // leave the rest of the body in its rest pose rather than collapsing it
        // to the origin.
        Transform rest{};

        // The inverse of the joint's world matrix in the bind pose. Read from the
        // file when it provides one (glTF does), derived from `rest` otherwise.
        Mat4 inverse_bind = Mat4::identity();
    };

    Skeleton(std::string path, std::vector<Joint> joints);

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    [[nodiscard]] const std::vector<Joint>& joints() const noexcept { return joints_; }
    [[nodiscard]] usize joint_count() const noexcept { return joints_.size(); }
    [[nodiscard]] bool empty() const noexcept { return joints_.empty(); }

    [[nodiscard]] const Joint& joint(usize index) const noexcept { return joints_[index]; }

    // The joint index with this name, or kNoParent when there is none. A clip
    // written against a name (which is how a human authors one) resolves through
    // here once at load rather than per frame.
    [[nodiscard]] i32 find_joint(std::string_view name) const noexcept;

    // The joint indices sorted so every parent precedes its children, which is
    // the order a pose walk must use. Computed once by the constructor.
    [[nodiscard]] const std::vector<i32>& order() const noexcept { return order_; }

    // True when every joint's parent index is in range and the parent links form
    // a forest with no cycle, so `order()` covers every joint. A skeleton that
    // fails this cannot be posed, and the loaders reject one rather than
    // producing a pose that silently drops joints.
    [[nodiscard]] bool has_valid_hierarchy() const noexcept { return order_.size() == joints_.size(); }

    // The number of roots (joints with no parent).
    [[nodiscard]] usize root_count() const noexcept;

    // Fills `inverse_bind` for every joint from the rest pose, by composing the
    // rest world matrices and inverting each. For a skeleton built in code, or a
    // format that supplies a rest pose but not the inverse matrices.
    void compute_inverse_bind_matrices();

    // The bind-pose world matrix of each joint, composed from `rest`. The
    // reference a computed `inverse_bind` must agree with, and what a test
    // asserts a bind-pose palette is the identity against.
    void compute_rest_world_matrices(std::vector<Mat4>& out) const;

    [[nodiscard]] usize memory_usage() const noexcept override;

private:
    void compute_order();

    std::vector<Joint> joints_;
    std::vector<i32> order_;
};

// One joint's resolved local pose: translation, rotation, and scale.
//
// A quaternion rather than the Euler triple `Transform` stores. A pose is
// produced by interpolating between keys, and interpolating Euler angles is
// wrong in two ways an animator sees immediately: the axes do not compose
// independently (so a blended rotation takes a path the author never intended),
// and a rotation that crosses the +/-pi wrap sweeps the long way round. A
// quaternion has neither problem. `Transform` keeps its Euler API because that is
// what a scene file and the inspector author; the conversion happens here, at the
// boundary, exactly as the physics solver does it.
struct JointPose {
    Vec3 translation{};
    Quat rotation = Quat::identity();
    Vec3 scale{1.0f, 1.0f, 1.0f};

    // The local matrix, composed as translation * rotation * scale: the same
    // composition `Transform::local_matrix()` uses, so a rest pose and a pose
    // reset from it agree bit for bit.
    [[nodiscard]] Mat4 local_matrix() const noexcept;

    [[nodiscard]] static JointPose identity() noexcept { return JointPose{}; }

    // The rest pose a `Transform` describes.
    [[nodiscard]] static JointPose from_transform(const Transform& transform) noexcept;

    // Back to Euler form, for writing a pose into a `Transform` (root motion, or
    // a tool that shows a pose as numbers).
    [[nodiscard]] Transform to_transform() const noexcept;

    [[nodiscard]] constexpr bool operator==(const JointPose&) const noexcept = default;
};

// A whole skeleton's local pose, one `JointPose` per joint.
//
// Deliberately a plain value type with no reference to the skeleton it belongs
// to: a pose is what a clip produces, and a test asserts on one with no scene,
// no resource, and no GPU. The skeleton is passed in to the two functions that
// need it (the world walk and the palette), which is what lets a pose be sampled
// before the rig that will drive it is known.
class SkeletonPose {
public:
    // Grows or shrinks to `count` joints. A new joint starts at the identity, so
    // call `reset_to_rest` after resizing to start from a bind pose.
    void resize(usize count);
    [[nodiscard]] usize joint_count() const noexcept { return joints_.size(); }
    [[nodiscard]] bool empty() const noexcept { return joints_.empty(); }

    [[nodiscard]] const std::vector<JointPose>& joints() const noexcept { return joints_; }
    [[nodiscard]] std::vector<JointPose>& joints() noexcept { return joints_; }

    [[nodiscard]] const JointPose& at(usize index) const noexcept { return joints_[index]; }
    [[nodiscard]] JointPose& at(usize index) noexcept { return joints_[index]; }

    // Every joint back to the skeleton's rest pose. The starting point a clip
    // samples over, so a joint the clip does not animate holds its authored
    // position rather than snapping to the origin.
    void reset_to_rest(const Skeleton& skeleton);

    // Composes each joint's world matrix from its local pose and its parent's
    // world matrix, walking the skeleton's parent-first order. `out` is resized
    // to the joint count. A joint whose index is past the end of the pose is
    // left as the identity rather than reading past the end, so a clip authored
    // against a different rig degrades instead of crashing.
    void world_matrices(const Skeleton& skeleton, std::vector<Mat4>& out) const;

    // The skinning palette: `world * inverse_bind` per joint, the matrices the
    // vertex shader (or the CPU skinner) weights a vertex by. This is linear
    // blend skinning's whole formulation; everything above it exists to produce
    // these matrices.
    //
    // In the bind pose the palette is the identity for every joint, because
    // `world` then *is* the inverse of `inverse_bind`. That identity is the
    // property a test asserts to prove the walk and the inverse matrices agree,
    // and it is why a skinned mesh with no animation renders exactly where its
    // static twin would.
    void skinning_matrices(const Skeleton& skeleton, std::vector<Mat4>& out) const;

private:
    std::vector<JointPose> joints_;
};

} // namespace fe::anim
