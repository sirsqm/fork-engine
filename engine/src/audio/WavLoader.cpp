// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/audio/WavLoader.h"

#include "WavCodec.h"

#include <string>

namespace fe::audio {

bool WavLoader::supports(std::string_view extension) const noexcept {
    return extension == ".wav";
}

Result<DecodedAudio> WavLoader::parse(const u8* data, usize size) {
    // The header parse and the sample decode both live in WavCodec.h, shared
    // with the incremental stream, so a loaded clip and a streamed one cannot
    // decode differently.
    auto header = wavcodec::parse_wav_header(data, size);
    if (!header) {
        return header.error();
    }
    auto samples = wavcodec::decode_data(data, header.value());
    if (!samples) {
        return samples.error();
    }

    DecodedAudio decoded;
    decoded.format = header.value().format;
    decoded.samples = std::move(samples.value());
    return decoded;
}

Result<std::shared_ptr<Resource>> WavLoader::load(std::string_view path) {
    auto bytes = read_bytes(path);
    if (!bytes) {
        return bytes.error();
    }

    auto decoded = parse(bytes.value().data(), bytes.value().size());
    if (!decoded) {
        return Error(std::string(decoded.error().message()) + " ('" + std::string(path) + "')");
    }

    auto clip =
        std::make_shared<AudioClip>(std::string(path), decoded.value().format, std::move(decoded.value().samples));
    return std::static_pointer_cast<Resource>(clip);
}

std::string WavLoader::reference_key(std::string_view file_path) {
    // An already-schemed path is returned unchanged so the helper is safe to
    // apply to either a bare file path or an existing reference. A colon at
    // index <= 1 is a Windows drive letter ("C:/..."), not a scheme, matching
    // the split ResourceManager::load performs.
    const std::size_t colon = file_path.find(':');
    if (colon != std::string_view::npos && colon > 1) {
        return std::string(file_path);
    }
    return "wav:" + std::string(file_path);
}

} // namespace fe::audio
