// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/mesh/ObjLoader.h"

#include "fe/core/FileSystem.h"
#include "fe/mesh/Mesh.h"

#include <charconv>
#include <sstream>
#include <string>
#include <vector>

namespace fe {
namespace {

[[nodiscard]] bool is_obj_extension(std::string_view extension) noexcept {
    return extension == ".obj";
}

// One `v/vt/vn` reference from a face statement. OBJ indices are 1-based;
// negative values are relative to the current end of the list.
struct FaceRef {
    i32 position = 0;
    i32 texcoord = 0;
    i32 normal = 0;
};

[[nodiscard]] i32 resolve_index(i32 raw, usize count) noexcept {
    if (raw > 0) {
        return raw - 1;
    }
    if (raw < 0) {
        return static_cast<i32>(count) + raw;
    }
    return -1; // Absent.
}

[[nodiscard]] bool parse_int(std::string_view text, i32& out) noexcept {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{};
}

// Splits "v/vt/vn" (any of vt/vn optional) into raw indices.
[[nodiscard]] FaceRef parse_face_ref(std::string_view token) noexcept {
    FaceRef ref;
    i32 fields[3] = {0, 0, 0};
    usize field = 0;

    usize start = 0;
    while (start <= token.size() && field < 3) {
        const usize slash = token.find('/', start);
        const usize end = slash == std::string_view::npos ? token.size() : slash;
        if (end > start) {
            (void)parse_int(token.substr(start, end - start), fields[field]);
        }
        ++field;
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }

    ref.position = fields[0];
    ref.texcoord = fields[1];
    ref.normal = fields[2];
    return ref;
}

} // namespace

bool ObjLoader::supports(std::string_view extension) const noexcept {
    return is_obj_extension(extension);
}

Result<std::shared_ptr<Resource>> ObjLoader::load(std::string_view path) {
    auto text = fs::read_text_file(path);
    if (!text) {
        return text.error();
    }
    return parse(path, text.value());
}

std::string ObjLoader::reference_key(std::string_view file_path) {
    if (file_path.find(':') != std::string_view::npos) {
        return std::string(file_path);
    }
    return "obj:" + std::string(file_path);
}

Result<std::shared_ptr<Resource>> ObjLoader::parse(std::string_view path, std::string_view text) {
    std::vector<Vec3> positions;
    std::vector<Vec2> texcoords;
    std::vector<Vec3> normals;

    std::vector<Vertex3D> vertices;
    std::vector<u32> indices;

    std::istringstream stream{std::string(text)};
    std::string line;
    usize line_number = 0;

    while (std::getline(stream, line)) {
        ++line_number;

        // Strip a trailing carriage return so CRLF files parse the same way.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const usize first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }

        std::istringstream fields{line.substr(first)};
        std::string keyword;
        fields >> keyword;

        if (keyword == "v") {
            f32 x = 0.0f;
            f32 y = 0.0f;
            f32 z = 0.0f;
            if (!(fields >> x >> y >> z)) {
                return Error("OBJ parse error at line " + std::to_string(line_number) + ": malformed vertex");
            }
            positions.push_back(Vec3{x, y, z});
        } else if (keyword == "vt") {
            f32 u = 0.0f;
            f32 v = 0.0f;
            if (!(fields >> u >> v)) {
                return Error("OBJ parse error at line " + std::to_string(line_number) + ": malformed texcoord");
            }
            texcoords.push_back(Vec2{u, v});
        } else if (keyword == "vn") {
            f32 x = 0.0f;
            f32 y = 0.0f;
            f32 z = 0.0f;
            if (!(fields >> x >> y >> z)) {
                return Error("OBJ parse error at line " + std::to_string(line_number) + ": malformed normal");
            }
            normals.push_back(Vec3{x, y, z});
        } else if (keyword == "f") {
            // Collect the face's corner references, then triangulate as a fan.
            std::vector<FaceRef> face;
            std::string token;
            while (fields >> token) {
                face.push_back(parse_face_ref(token));
            }
            if (face.size() < 3) {
                continue; // Degenerate; skip rather than fail the whole mesh.
            }

            const auto resolve = [&](const FaceRef& ref) -> Result<u32> {
                const i32 p = resolve_index(ref.position, positions.size());
                if (p < 0 || static_cast<usize>(p) >= positions.size()) {
                    return Error("OBJ parse error at line " + std::to_string(line_number) +
                                 ": vertex index out of range");
                }
                const i32 n = resolve_index(ref.normal, normals.size());
                const i32 t = resolve_index(ref.texcoord, texcoords.size());

                Vertex3D vertex{};
                vertex.position = positions[static_cast<usize>(p)];
                if (n >= 0 && static_cast<usize>(n) < normals.size()) {
                    vertex.normal = normals[static_cast<usize>(n)];
                }
                if (t >= 0 && static_cast<usize>(t) < texcoords.size()) {
                    vertex.uv = texcoords[static_cast<usize>(t)];
                }
                vertices.push_back(vertex);
                return static_cast<u32>(vertices.size() - 1);
            };

            // Resolve the first corner once; then fan (0, i, i+1).
            std::vector<u32> corner_indices;
            corner_indices.reserve(face.size());
            for (const FaceRef& ref : face) {
                auto resolved = resolve(ref);
                if (!resolved) {
                    return resolved.error();
                }
                corner_indices.push_back(resolved.value());
            }
            for (usize i = 1; i + 1 < corner_indices.size(); ++i) {
                indices.push_back(corner_indices[0]);
                indices.push_back(corner_indices[i]);
                indices.push_back(corner_indices[i + 1]);
            }
        }
        // Any other keyword (o, g, s, usemtl, mtllib, ...) is ignored.
    }

    // Compute a flat normal for every triangle that arrived without one, so an
    // unlit-looking OBJ still shades. We only overwrite vertices whose normal is
    // exactly zero, which is the "absent" sentinel the loader leaves behind.
    for (usize i = 0; i + 2 < indices.size(); i += 3) {
        const u32 a = indices[i];
        const u32 b = indices[i + 1];
        const u32 c = indices[i + 2];
        if (vertices[a].normal.length_squared() > 0.0f && vertices[b].normal.length_squared() > 0.0f &&
            vertices[c].normal.length_squared() > 0.0f) {
            continue;
        }
        const Vec3 edge1 = vertices[b].position - vertices[a].position;
        const Vec3 edge2 = vertices[c].position - vertices[a].position;
        const Vec3 normal = edge1.cross(edge2).normalized();
        for (const u32 index : {a, b, c}) {
            if (vertices[index].normal.length_squared() == 0.0f) {
                vertices[index].normal = normal;
            }
        }
    }

    if (vertices.empty()) {
        return Error("OBJ '" + std::string(path) + "' contains no faces");
    }

    auto mesh = std::make_shared<Mesh>(std::string(path), std::move(vertices), std::move(indices));
    return std::static_pointer_cast<Resource>(mesh);
}

} // namespace fe
