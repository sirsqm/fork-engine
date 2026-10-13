// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the mesh module: the built-in primitive generators and the OBJ
// parser. These are pure CPU tests; no graphics context is needed.

#include <doctest/doctest.h>

#include "fe/mesh/Mesh.h"
#include "fe/mesh/ObjLoader.h"

#include <memory>

using namespace fe;

TEST_CASE("make_cube produces 24 vertices and 36 indices") {
    const std::shared_ptr<Mesh> cube = Mesh::make_cube();

    // Six faces, four unique vertices each, two triangles per face.
    CHECK(cube->vertex_count() == 24);
    CHECK(cube->index_count() == 36);
    CHECK(cube->triangle_count() == 12);
}

TEST_CASE("make_cube has a unit bounds box centred on the origin") {
    const std::shared_ptr<Mesh> cube = Mesh::make_cube();
    CHECK(cube->bounds_min().x == doctest::Approx(-0.5f));
    CHECK(cube->bounds_min().y == doctest::Approx(-0.5f));
    CHECK(cube->bounds_min().z == doctest::Approx(-0.5f));
    CHECK(cube->bounds_max().x == doctest::Approx(0.5f));
    CHECK(cube->bounds_max().y == doctest::Approx(0.5f));
    CHECK(cube->bounds_max().z == doctest::Approx(0.5f));
}

TEST_CASE("make_cube face normals are unit length and axis aligned") {
    const std::shared_ptr<Mesh> cube = Mesh::make_cube();
    for (const Vertex3D& vertex : cube->vertices()) {
        CHECK(vertex.normal.length_squared() == doctest::Approx(1.0f));
        // Exactly one component is +/-1 and the other two are 0.
        const f32 abs_sum = std::abs(vertex.normal.x) + std::abs(vertex.normal.y) + std::abs(vertex.normal.z);
        CHECK(abs_sum == doctest::Approx(1.0f));
    }
}

TEST_CASE("make_plane spans the requested half extents and faces up") {
    const std::shared_ptr<Mesh> plane = Mesh::make_plane(2.0f, 3.0f);

    CHECK(plane->vertex_count() == 4);
    CHECK(plane->index_count() == 6);
    CHECK(plane->bounds_min().x == doctest::Approx(-2.0f));
    CHECK(plane->bounds_max().x == doctest::Approx(2.0f));
    CHECK(plane->bounds_min().z == doctest::Approx(-3.0f));
    CHECK(plane->bounds_max().z == doctest::Approx(3.0f));
    // Flat in Y.
    CHECK(plane->bounds_min().y == doctest::Approx(0.0f));
    CHECK(plane->bounds_max().y == doctest::Approx(0.0f));
    for (const Vertex3D& vertex : plane->vertices()) {
        CHECK(vertex.normal.y == doctest::Approx(1.0f));
    }
}

TEST_CASE("an empty mesh reports zero-size bounds") {
    const Mesh mesh("empty", {}, {});
    CHECK(mesh.vertex_count() == 0);
    CHECK(mesh.index_count() == 0);
    CHECK(mesh.bounds_min().x == doctest::Approx(0.0f));
    CHECK(mesh.bounds_max().x == doctest::Approx(0.0f));
}

TEST_CASE("ObjLoader::reference_key adds the obj scheme once") {
    CHECK(ObjLoader::reference_key("assets/crate.obj") == "obj:assets/crate.obj");
    // Already-schemed paths are returned unchanged.
    CHECK(ObjLoader::reference_key("obj:assets/crate.obj") == "obj:assets/crate.obj");
}

TEST_CASE("ObjLoader parses a triangle with positions and normals") {
    const char* obj = R"(# a single triangle
v 0.0 0.0 0.0
v 1.0 0.0 0.0
v 0.0 1.0 0.0
vn 0.0 0.0 1.0
f 1//1 2//1 3//1
)";

    auto parsed = ObjLoader::parse("tri.obj", obj);
    REQUIRE(parsed);
    const auto* mesh = dynamic_cast<const Mesh*>(parsed.value().get());
    REQUIRE(mesh != nullptr);
    CHECK(mesh->vertex_count() == 3);
    CHECK(mesh->index_count() == 3);
    CHECK(mesh->triangle_count() == 1);

    // The authored normal survives.
    CHECK(mesh->vertices()[0].normal.z == doctest::Approx(1.0f));
    CHECK(mesh->bounds_max().x == doctest::Approx(1.0f));
    CHECK(mesh->bounds_max().y == doctest::Approx(1.0f));
}

TEST_CASE("ObjLoader triangulates a quad as a fan") {
    const char* obj = R"(v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
f 1 2 3 4
)";

    auto parsed = ObjLoader::parse("quad.obj", obj);
    REQUIRE(parsed);
    const auto* mesh = dynamic_cast<const Mesh*>(parsed.value().get());
    REQUIRE(mesh != nullptr);
    // A quad becomes two triangles.
    CHECK(mesh->triangle_count() == 2);
    CHECK(mesh->index_count() == 6);
}

TEST_CASE("ObjLoader computes a flat normal when faces omit them") {
    const char* obj = R"(v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 3
)";

    auto parsed = ObjLoader::parse("flat.obj", obj);
    REQUIRE(parsed);
    const auto* mesh = dynamic_cast<const Mesh*>(parsed.value().get());
    REQUIRE(mesh != nullptr);
    // The triangle lies in the XY plane, so its face normal is +/-Z.
    for (const Vertex3D& vertex : mesh->vertices()) {
        CHECK(std::abs(vertex.normal.z) == doctest::Approx(1.0f));
    }
}

TEST_CASE("ObjLoader resolves negative (relative) indices") {
    const char* obj = R"(v 0 0 0
v 1 0 0
v 0 1 0
f -3 -2 -1
)";

    auto parsed = ObjLoader::parse("relative.obj", obj);
    REQUIRE(parsed);
    const auto* mesh = dynamic_cast<const Mesh*>(parsed.value().get());
    REQUIRE(mesh != nullptr);
    CHECK(mesh->triangle_count() == 1);
    CHECK(mesh->vertices()[0].position.x == doctest::Approx(0.0f));
    CHECK(mesh->vertices()[1].position.x == doctest::Approx(1.0f));
}

TEST_CASE("ObjLoader reads texture coordinates") {
    const char* obj = R"(v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
f 1/1 2/2 3/3
)";

    auto parsed = ObjLoader::parse("uv.obj", obj);
    REQUIRE(parsed);
    const auto* mesh = dynamic_cast<const Mesh*>(parsed.value().get());
    REQUIRE(mesh != nullptr);
    CHECK(mesh->vertices()[1].uv.x == doctest::Approx(1.0f));
    CHECK(mesh->vertices()[2].uv.y == doctest::Approx(1.0f));
}

TEST_CASE("ObjLoader rejects an out-of-range index") {
    const char* obj = R"(v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 9
)";
    auto parsed = ObjLoader::parse("bad.obj", obj);
    CHECK_FALSE(parsed);
}

TEST_CASE("ObjLoader rejects a file with no faces") {
    const char* obj = R"(v 0 0 0
v 1 0 0
)";
    auto parsed = ObjLoader::parse("points.obj", obj);
    CHECK_FALSE(parsed);
}

TEST_CASE("ObjLoader ignores group and material statements") {
    const char* obj = R"(mtllib crate.mtl
o Crate
g Crate
usemtl Wood
s off
v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 3
)";
    auto parsed = ObjLoader::parse("crate.obj", obj);
    REQUIRE(parsed);
    const auto* mesh = dynamic_cast<const Mesh*>(parsed.value().get());
    REQUIRE(mesh != nullptr);
    CHECK(mesh->triangle_count() == 1);
}
