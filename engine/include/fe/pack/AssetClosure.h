// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fe::pack {

// Names the files one asset depends on, given the asset's path relative to the
// pack root and its bytes. Returns paths also relative to the pack root, or an
// empty list for an asset that is self-contained or that this reader does not
// understand.
//
// Supplied by the caller so this module stays independent of any asset format:
// that a glTF's vertex data lives in an external `.bin` and its maps in image
// files is a `mesh` concern, not a container concern.
using AssetDependencyReader =
    std::function<std::vector<std::string>(std::string_view relative_path, std::string_view bytes)>;

// Expands a set of root-relative asset paths with the files those assets name
// internally, so a pruned pack carries an asset's data and not just the asset.
//
// Reachability pruning keeps the assets a scene names (docs/adr/0065). That is
// complete for a self-contained file, but a glTF is a *container*: the scene
// names the glTF and nothing else, while the glTF names its `.bin` and its
// images. Packing the glTF alone ships a mesh that loads and then fails to
// decode, and a material whose maps never resolve - a wrong render, or a
// silently missing asset, rather than a failed pack.
//
// This walks `paths`, reads each file under `pack_root`, asks `reader` what it
// names, and appends whatever is new, repeatedly until nothing new appears, so a
// dependency that is itself a container is followed too. A path that cannot be
// read contributes no further dependencies; a dependency the tree does not
// contain is still listed, and the writer - which packs only files that exist
// under the root - ignores it, so a missing dependency degrades to today's
// behaviour rather than failing the pack.
[[nodiscard]] std::vector<std::string> expand_asset_dependencies(std::vector<std::string> paths,
                                                                 std::string_view pack_root,
                                                                 const AssetDependencyReader& reader);

} // namespace fe::pack
