// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// fork_pack - assemble a project into a pack.
//
// The first shipping step of M8 (docs/PLAN.md): a project directory goes in,
// one pack file comes out, and the runtime mounts the pack with no knowledge
// that its assets were ever loose files. See
// docs/adr/0030-asset-pack-format.md.
//
// This tool is the only place that reads `fork.project.json` for packing. The
// engine's PackWriter takes resolved paths, so the engine stays project-agnostic
// exactly as it is today (docs/adr/0025-project-file.md).
//
// Usage:
//   fork_pack --project DIR --out FILE
//   fork_pack --assets DIR --scene FILE --out FILE [--scene-key PATH]
//
// With --project the asset root, the startup scene, and the scene's key in the
// pack all come from the project file. The explicit form packs a bare tree, for
// a caller that has no project file.
//
// Pruning is the default (ADR-0065): only the startup scene and the assets it
// references are packed. Pass --no-prune to pack the whole asset tree.

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/mesh/GltfLoader.h"
#include "fe/pack/AssetClosure.h"
#include "fe/pack/PackSource.h"
#include "fe/pack/PackWriter.h"
#include "fe/resources/AssetDatabase.h"
#include "fe/resources/ResourceManager.h"
#include "fe/runtime/ComponentRegistration.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/Json.h"
#include "fe/serialization/SceneReferences.h"
#include "fe/serialization/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::string project;   // a project directory or file, when given
    std::string assets;    // asset root, in the explicit form
    std::string scene;     // startup scene path, in the explicit form
    std::string scene_key; // key the scene is stored under in the pack
    std::string out;       // output pack path
    bool prune = true;     // pack only what the startup scene references
    bool verbose = false;
};

// The subset of the project file the packer needs. The engine does not know
// what a project is (docs/adr/0025-project-file.md), so this small reader lives
// with the tool that does. It reads the same document the editor writes.
struct ProjectFile {
    std::string directory;     // the directory the project file lives in
    std::string startup_scene; // resolved against `directory`
};

[[nodiscard]] std::string resolve_against(const std::string& directory, const std::string& path) {
    if (path.empty()) {
        return {};
    }
    if (fe::fs::is_absolute(path)) {
        return fe::fs::normalize(path);
    }
    return fe::fs::normalize(fe::fs::join(directory, path));
}

[[nodiscard]] fe::Result<ProjectFile> read_project(const std::string& path) {
    // A directory (or a path with no extension) names the project; the file name
    // inside it is fixed, matching the editor's default.
    std::string file = path;
    if (fe::fs::extension(path).empty()) {
        file = fe::fs::join(path, "fork.project.json");
    }

    auto text = fe::fs::read_text_file(file);
    if (!text) {
        return text.error();
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(text.value());
    } catch (const nlohmann::json::parse_error& e) {
        return fe::Error("project parse error in '" + file + "': " + e.what());
    }

    const std::string format = document.value("format", std::string{});
    if (format != "fork_project") {
        return fe::Error("not a fork project document (format='" + format + "')");
    }
    const fe::i32 version = document.value("format_version", 0);
    if (version > 1 || version < 1) {
        return fe::Error("project format version " + std::to_string(version) + " is not supported");
    }

    ProjectFile project;
    project.directory = fe::fs::parent_path(fe::fs::normalize(file));
    project.startup_scene = resolve_against(project.directory, document.value("startup_scene", std::string{}));
    return project;
}

// Resolves the startup scene's resource references to root-relative asset paths,
// so a pruned pack carries exactly what the scene names. Reads the scene as text
// and traces it through reflection; it does not load it, so no resource is
// decoded.
//
// A reference resolves if the database has indexed it (its path is current and
// its id is stable) or, failing that, if the file is simply present under the
// pack root - an unimported asset is still packed, by path, under the default
// scheme, exactly as the unpruned writer packs it. A reference that resolves to
// neither is a hard error: shipping a build whose scene names an asset that is
// not in it produces a game that runs until it draws, which is worse than a
// failed pack.
[[nodiscard]] fe::Result<std::vector<std::string>>
scene_asset_paths(const std::string& scene_path, const std::string& pack_root, const fe::AssetDatabase& assets) {
    auto text = fe::fs::read_text_file(scene_path);
    if (!text) {
        return text.error();
    }
    auto references = fe::serialization::scene::collect_resource_references(text.value());
    if (!references) {
        return references.error();
    }
    auto paths = fe::serialization::scene::resolve_scene_asset_paths(references.value(), pack_root, assets);
    if (!paths) {
        return paths.error();
    }
    // A glTF names its own buffers and images, which the scene does not, so the
    // closure is expanded here: without it a pruned pack ships the glTF and
    // drops its data. See docs/adr/0088-transitive-pack-references.md.
    return fe::pack::expand_asset_dependencies(std::move(paths.value()), pack_root,
                                               fe::GltfLoader::dependency_reader());
}

void print_usage() {
    std::puts("usage: fork_pack --project DIR --out FILE [--no-prune]\n"
              "       fork_pack --assets DIR --scene FILE --out FILE [--scene-key PATH] [--no-prune]\n"
              "\n"
              "  --project DIR     a project directory or fork.project.json file\n"
              "  --assets DIR      the asset root to pack (explicit form)\n"
              "  --scene FILE      the startup scene to pack (explicit form)\n"
              "  --scene-key PATH  the key the scene is stored under in the pack\n"
              "                    (default: the scene path relative to the asset root)\n"
              "  --no-prune        pack the whole asset tree instead of only the\n"
              "                    assets the startup scene references. Pruning is\n"
              "                    the default (ADR-0065)\n"
              "  --out FILE        the pack file to write\n"
              "  --verbose         log at debug level");
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--project" && has_value) {
            out.project = argv[++i];
        } else if (arg == "--assets" && has_value) {
            out.assets = argv[++i];
        } else if (arg == "--scene" && has_value) {
            out.scene = argv[++i];
        } else if (arg == "--scene-key" && has_value) {
            out.scene_key = argv[++i];
        } else if (arg == "--out" && has_value) {
            out.out = argv[++i];
        } else if (arg == "--prune") {
            out.prune = true;
        } else if (arg == "--no-prune") {
            out.prune = false;
        } else if (arg == "--verbose") {
            out.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            std::fprintf(stderr, "fork_pack: unknown or incomplete argument '%s'\n", std::string(arg).c_str());
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }
    fe::log::set_min_level(options.verbose ? fe::LogLevel::Debug : fe::LogLevel::Info);
    // Make every component type visible before the scene is traced for
    // pruning: a tool that registered only the 2D builtins would see a 3D
    // scene reference nothing and ship an empty pack. See
    // docs/adr/0076-3d-in-the-browser.md.
    fe::runtime::register_all_component_types();

    if (options.out.empty()) {
        std::fprintf(stderr, "fork_pack: --out is required\n");
        return 2;
    }

    std::string pack_root;
    std::string scene_path;
    std::string scene_key;

    if (!options.project.empty()) {
        auto project = read_project(options.project);
        if (!project) {
            std::fprintf(stderr, "fork_pack: %s\n", project.error().message().c_str());
            return 1;
        }
        // The pack root is the project directory, not the asset root: a scene's
        // asset references are project-relative ("assets/textures/checker.png"),
        // so keying the pack the same way is what makes them resolve in a shipped
        // run. The project's own asset_root stays an editor concern
        // (docs/adr/0025-project-file.md).
        pack_root = project.value().directory;
        scene_path = project.value().startup_scene;
        if (scene_path.empty()) {
            std::fprintf(stderr, "fork_pack: the project names no startup scene\n");
            return 1;
        }
        // The scene's key is its path relative to the project directory, so the
        // runtime asks for the same "scenes/main.fescene" it would on disk.
        scene_key = fe::fs::relative_to(pack_root, scene_path);
    } else {
        if (options.assets.empty() || options.scene.empty()) {
            std::fprintf(stderr, "fork_pack: the explicit form needs --assets and --scene\n");
            return 2;
        }
        pack_root = fe::fs::normalize(options.assets);
        scene_path = fe::fs::normalize(options.scene);
        scene_key = options.scene_key.empty() ? fe::fs::relative_to(pack_root, scene_path) : options.scene_key;
    }

    if (!fe::fs::exists(pack_root)) {
        std::fprintf(stderr, "fork_pack: pack root '%s' does not exist\n", pack_root.c_str());
        return 1;
    }

    // Index the tree (its sidecars) so every imported asset goes in with its
    // scheme and stable id. Every other file under the root is packed too, under
    // the default scheme with no id, so a project that references it by path
    // still runs from the pack; only importing gives a stable reference.
    fe::AssetDatabase assets;
    assets.set_root(pack_root);
    FE_LOG_INFO("fork_pack: indexed %llu asset(s) under '%s'", static_cast<unsigned long long>(assets.asset_count()),
                pack_root.c_str());

    fe::pack::PackOptions pack_options;
    if (options.prune) {
        auto paths = scene_asset_paths(scene_path, pack_root, assets);
        if (!paths) {
            std::fprintf(stderr, "fork_pack: %s\n", paths.error().message().c_str());
            return 1;
        }
        pack_options.prune = true;
        pack_options.include = std::move(paths.value());
        FE_LOG_INFO("fork_pack: pruning to %llu scene-referenced asset(s)",
                    static_cast<unsigned long long>(pack_options.include.size()));
    }

    if (fe::Error error = fe::pack::PackWriter::write(options.out, assets, scene_path, scene_key, pack_options);
        error) {
        std::fprintf(stderr, "fork_pack: %s\n", error.message().c_str());
        return 1;
    }

    // Read the pack back through the runtime's own mount, so a pack that fails
    // to parse is reported here rather than at first load. This is the same
    // check the tests make, run once more on the real output.
    auto source = fe::pack::PackSource::open(options.out);
    if (!source) {
        std::fprintf(stderr, "fork_pack: wrote '%s' but it did not read back: %s\n", options.out.c_str(),
                     source.error().message().c_str());
        return 1;
    }

    // Prove the startup scene is reachable and parses, by loading it through the
    // mounted pack exactly as the runtime would.
    fe::ResourceManager manager;
    manager.mount(source.value());
    fe::Scene scene;
    if (fe::Error error = fe::serialization::scene::load(scene_key, scene, &manager); error) {
        std::fprintf(stderr, "fork_pack: the packed scene did not load: %s\n", error.message().c_str());
        return 1;
    }

    FE_LOG_INFO("fork_pack: wrote '%s' with %llu entries; scene '%s' has %llu node(s)", options.out.c_str(),
                static_cast<unsigned long long>(source.value()->pack().entry_count()), scene_key.c_str(),
                static_cast<unsigned long long>(scene.node_count()));
    return 0;
}
