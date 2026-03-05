#include "client_app.hpp"

#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "client_app_test_hooks.hpp"

namespace isla::client {
namespace {

class ScopedEnvVar {
  public:
    ScopedEnvVar(const char* name, const char* value) : name_(name) {
        const char* existing = std::getenv(name_);
        if (existing != nullptr) {
            had_original_ = true;
            original_ = existing;
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (had_original_) {
            set(original_.c_str());
        } else {
#if defined(_WIN32)
            _putenv_s(name_, "");
#else
            unsetenv(name_);
#endif
        }
    }

  private:
    void set(const char* value) {
#if defined(_WIN32)
        _putenv_s(name_, value != nullptr ? value : "");
#else
        if (value == nullptr || value[0] == '\0') {
            unsetenv(name_);
        } else {
            setenv(name_, value, 1);
        }
#endif
    }

    const char* name_ = nullptr;
    bool had_original_ = false;
    std::string original_;
};

class ScopedCurrentPath {
  public:
    explicit ScopedCurrentPath(const std::filesystem::path& path) {
        std::error_code ec;
        original_ = std::filesystem::current_path(ec);
        if (ec) {
            return;
        }
        std::filesystem::current_path(path, ec);
        if (ec) {
            original_.clear();
            return;
        }
        armed_ = true;
    }

    ~ScopedCurrentPath() {
        if (!armed_) {
            return;
        }
        std::error_code ec;
        std::filesystem::current_path(original_, ec);
    }

    [[nodiscard]] bool is_armed() const {
        return armed_;
    }

  private:
    std::filesystem::path original_;
    bool armed_ = false;
};

class ScopedTempDir {
  public:
    static ScopedTempDir create(std::string_view prefix) {
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return {};
        }

        std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<std::uint64_t> distribution;
        for (int i = 0; i < 100; ++i) {
            const auto candidate =
                base / (std::string(prefix) + "_" + std::to_string(distribution(rng)));
            if (std::filesystem::create_directories(candidate, ec) && !ec) {
                return ScopedTempDir(candidate);
            }
            ec.clear();
        }
        return {};
    }

    ScopedTempDir() = default;
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    ScopedTempDir(ScopedTempDir&&) = default;
    ScopedTempDir& operator=(ScopedTempDir&&) = default;

    ~ScopedTempDir() {
        if (path_.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] bool is_valid() const {
        return !path_.empty();
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

  private:
    explicit ScopedTempDir(std::filesystem::path path) : path_(std::move(path)) {}

    std::filesystem::path path_{};
};

std::vector<std::uint8_t> make_minimal_triangle_glb() {
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":36}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"max\":[1,1,0],\"min\":[0,0,0]}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}]}";

    std::vector<std::uint8_t> json_chunk(json.begin(), json.end());
    while ((json_chunk.size() % 4U) != 0U) {
        json_chunk.push_back(static_cast<std::uint8_t>(' '));
    }

    std::vector<std::uint8_t> bin_chunk;
    bin_chunk.reserve(36U);
    const float vertices[9] = {
        0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
    };
    for (float value : vertices) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        bin_chunk.insert(bin_chunk.end(), bytes, bytes + sizeof(float));
    }
    while ((bin_chunk.size() % 4U) != 0U) {
        bin_chunk.push_back(0U);
    }

    auto append_u32_le = [](std::vector<std::uint8_t>& out, std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    };

    const auto json_chunk_length = static_cast<std::uint32_t>(json_chunk.size());
    const auto bin_chunk_length = static_cast<std::uint32_t>(bin_chunk.size());
    const std::uint32_t total_length = 12U + 8U + json_chunk_length + 8U + bin_chunk_length;

    std::vector<std::uint8_t> glb;
    glb.reserve(total_length);

    // GLB header.
    glb.push_back(static_cast<std::uint8_t>('g'));
    glb.push_back(static_cast<std::uint8_t>('l'));
    glb.push_back(static_cast<std::uint8_t>('T'));
    glb.push_back(static_cast<std::uint8_t>('F'));
    append_u32_le(glb, 2U);
    append_u32_le(glb, total_length);

    // JSON chunk.
    append_u32_le(glb, json_chunk_length);
    glb.push_back(static_cast<std::uint8_t>('J'));
    glb.push_back(static_cast<std::uint8_t>('S'));
    glb.push_back(static_cast<std::uint8_t>('O'));
    glb.push_back(static_cast<std::uint8_t>('N'));
    glb.insert(glb.end(), json_chunk.begin(), json_chunk.end());

    // BIN chunk.
    append_u32_le(glb, bin_chunk_length);
    glb.push_back(static_cast<std::uint8_t>('B'));
    glb.push_back(static_cast<std::uint8_t>('I'));
    glb.push_back(static_cast<std::uint8_t>('N'));
    glb.push_back(0U);
    glb.insert(glb.end(), bin_chunk.begin(), bin_chunk.end());

    return glb;
}

void append_u16_le(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void append_f32_le(std::vector<std::uint8_t>& out, float value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(float));
}

std::filesystem::path write_skinned_no_animation_gltf_fixture(const std::filesystem::path& dir) {
    const std::filesystem::path gltf_path = dir / "skinned_no_clips.gltf";
    const std::filesystem::path bin_path = dir / "skinned_no_clips.bin";
    const std::filesystem::path texture_path = dir / "albedo.png";

    std::vector<std::uint8_t> buffer;
    buffer.reserve(256U);
    const std::size_t positions_offset = buffer.size();
    const float positions[9] = {
        0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
    };
    for (float value : positions) {
        append_f32_le(buffer, value);
    }
    const std::size_t positions_length = buffer.size() - positions_offset;

    const std::size_t normals_offset = buffer.size();
    const float normals[9] = {
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F,
    };
    for (float value : normals) {
        append_f32_le(buffer, value);
    }
    const std::size_t normals_length = buffer.size() - normals_offset;

    const std::size_t joints_offset = buffer.size();
    for (int i = 0; i < 3; ++i) {
        append_u16_le(buffer, 0U);
        append_u16_le(buffer, 0U);
        append_u16_le(buffer, 0U);
        append_u16_le(buffer, 0U);
    }
    const std::size_t joints_length = buffer.size() - joints_offset;

    const std::size_t weights_offset = buffer.size();
    for (int i = 0; i < 3; ++i) {
        append_f32_le(buffer, 1.0F);
        append_f32_le(buffer, 0.0F);
        append_f32_le(buffer, 0.0F);
        append_f32_le(buffer, 0.0F);
    }
    const std::size_t weights_length = buffer.size() - weights_offset;

    const std::size_t indices_offset = buffer.size();
    append_u16_le(buffer, 0U);
    append_u16_le(buffer, 1U);
    append_u16_le(buffer, 2U);
    const std::size_t indices_length = buffer.size() - indices_offset;
    while ((buffer.size() % 4U) != 0U) {
        buffer.push_back(0U);
    }

    const std::size_t ibm_offset = buffer.size();
    const float identity_mat4[16] = {
        1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,
    };
    for (float value : identity_mat4) {
        append_f32_le(buffer, value);
    }
    const std::size_t ibm_length = buffer.size() - ibm_offset;

    {
        std::ofstream bin_out(bin_path, std::ios::binary);
        if (!bin_out.is_open()) {
            ADD_FAILURE() << "failed to open skinned fixture BIN output path: " << bin_path;
            return {};
        }
        bin_out.write(reinterpret_cast<const char*>(buffer.data()),
                      static_cast<std::streamsize>(buffer.size()));
    }
    {
        std::ofstream texture_out(texture_path, std::ios::binary);
        if (!texture_out.is_open()) {
            ADD_FAILURE() << "failed to open skinned fixture texture output path: " << texture_path;
            return {};
        }
        texture_out << "fake_png";
    }

    std::ofstream gltf_out(gltf_path, std::ios::binary);
    if (!gltf_out.is_open()) {
        ADD_FAILURE() << "failed to open skinned fixture glTF output path: " << gltf_path;
        return {};
    }
    gltf_out << "{\n"
             << "  \"asset\": {\"version\": \"2.0\"},\n"
             << R"(  "buffers": [{"uri": ")" << bin_path.filename().string()
             << R"(", "byteLength": )" << buffer.size() << "}],\n"
             << "  \"bufferViews\": [\n"
             << R"(    {"buffer": 0, "byteOffset": )" << positions_offset
             << ", \"byteLength\": " << positions_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << normals_offset
             << ", \"byteLength\": " << normals_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << joints_offset
             << ", \"byteLength\": " << joints_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << weights_offset
             << ", \"byteLength\": " << weights_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << indices_offset
             << ", \"byteLength\": " << indices_length << ", \"target\": 34963},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << ibm_offset
             << ", \"byteLength\": " << ibm_length << "}\n"
             << "  ],\n"
             << "  \"accessors\": [\n"
             << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": "
                "\"VEC3\"},\n"
             << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 3, \"type\": "
                "\"VEC3\"},\n"
             << "    {\"bufferView\": 2, \"componentType\": 5123, \"count\": 3, \"type\": "
                "\"VEC4\"},\n"
             << "    {\"bufferView\": 3, \"componentType\": 5126, \"count\": 3, \"type\": "
                "\"VEC4\"},\n"
             << "    {\"bufferView\": 4, \"componentType\": 5123, \"count\": 3, \"type\": "
                "\"SCALAR\"},\n"
             << "    {\"bufferView\": 5, \"componentType\": 5126, \"count\": 1, \"type\": "
                "\"MAT4\"}\n"
             << "  ],\n"
             << R"(  "images": [{"uri": ")" << texture_path.filename().string() << "\"}],\n"
             << "  \"textures\": [{\"source\": 0}],\n"
             << "  \"materials\": [{\n"
             << "    \"pbrMetallicRoughness\": {\n"
             << "      \"baseColorFactor\": [0.3, 0.5, 0.7, 0.4],\n"
             << "      \"baseColorTexture\": {\"index\": 0}\n"
             << "    },\n"
             << "    \"doubleSided\": true,\n"
             << "    \"alphaMode\": \"MASK\",\n"
             << "    \"alphaCutoff\": 0.5\n"
             << "  }],\n"
             << "  \"meshes\": [{\"primitives\": [{\n"
             << "    \"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, \"JOINTS_0\": 2, "
                "\"WEIGHTS_0\": 3},\n"
             << "    \"indices\": 4,\n"
             << "    \"material\": 0\n"
             << "  }]}],\n"
             << "  \"nodes\": [{\"name\": \"joint0\"}, {\"mesh\": 0, \"skin\": 0}],\n"
             << "  \"skins\": [{\"joints\": [0], \"inverseBindMatrices\": 5}],\n"
             << "  \"scenes\": [{\"nodes\": [1]}],\n"
             << "  \"scene\": 0\n"
             << "}\n";
    if (!gltf_out.good()) {
        ADD_FAILURE() << "failed to write skinned fixture glTF output path: " << gltf_path;
        return {};
    }
    return gltf_path;
}

std::filesystem::path write_skinned_with_animation_gltf_fixture(const std::filesystem::path& dir) {
    const std::filesystem::path gltf_path = dir / "skinned_with_clip.gltf";
    const std::filesystem::path bin_path = dir / "skinned_with_clip.bin";

    std::vector<std::uint8_t> buffer;
    buffer.reserve(320U);
    const std::size_t positions_offset = buffer.size();
    const float positions[9] = {
        0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
    };
    for (float value : positions) {
        append_f32_le(buffer, value);
    }
    const std::size_t positions_length = buffer.size() - positions_offset;

    const std::size_t normals_offset = buffer.size();
    const float normals[9] = {
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F,
    };
    for (float value : normals) {
        append_f32_le(buffer, value);
    }
    const std::size_t normals_length = buffer.size() - normals_offset;

    const std::size_t joints_offset = buffer.size();
    for (int i = 0; i < 3; ++i) {
        append_u16_le(buffer, 0U);
        append_u16_le(buffer, 0U);
        append_u16_le(buffer, 0U);
        append_u16_le(buffer, 0U);
    }
    const std::size_t joints_length = buffer.size() - joints_offset;

    const std::size_t weights_offset = buffer.size();
    for (int i = 0; i < 3; ++i) {
        append_f32_le(buffer, 1.0F);
        append_f32_le(buffer, 0.0F);
        append_f32_le(buffer, 0.0F);
        append_f32_le(buffer, 0.0F);
    }
    const std::size_t weights_length = buffer.size() - weights_offset;

    const std::size_t indices_offset = buffer.size();
    append_u16_le(buffer, 0U);
    append_u16_le(buffer, 1U);
    append_u16_le(buffer, 2U);
    const std::size_t indices_length = buffer.size() - indices_offset;
    while ((buffer.size() % 4U) != 0U) {
        buffer.push_back(0U);
    }

    const std::size_t ibm_offset = buffer.size();
    const float identity_mat4[16] = {
        1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,
    };
    for (float value : identity_mat4) {
        append_f32_le(buffer, value);
    }
    const std::size_t ibm_length = buffer.size() - ibm_offset;

    const std::size_t times_offset = buffer.size();
    append_f32_le(buffer, 0.0F);
    append_f32_le(buffer, 1.0F);
    const std::size_t times_length = buffer.size() - times_offset;

    const std::size_t translations_offset = buffer.size();
    append_f32_le(buffer, 0.0F);
    append_f32_le(buffer, 0.0F);
    append_f32_le(buffer, 0.0F);
    append_f32_le(buffer, 2.0F);
    append_f32_le(buffer, 0.0F);
    append_f32_le(buffer, 0.0F);
    const std::size_t translations_length = buffer.size() - translations_offset;

    {
        std::ofstream bin_out(bin_path, std::ios::binary);
        if (!bin_out.is_open()) {
            ADD_FAILURE() << "failed to open animated fixture BIN output path: " << bin_path;
            return {};
        }
        bin_out.write(reinterpret_cast<const char*>(buffer.data()),
                      static_cast<std::streamsize>(buffer.size()));
    }

    std::ofstream gltf_out(gltf_path, std::ios::binary);
    if (!gltf_out.is_open()) {
        ADD_FAILURE() << "failed to open animated fixture glTF output path: " << gltf_path;
        return {};
    }
    gltf_out << "{\n"
             << "  \"asset\": {\"version\": \"2.0\"},\n"
             << R"(  "buffers": [{"uri": ")" << bin_path.filename().string()
             << R"(", "byteLength": )" << buffer.size() << "}],\n"
             << "  \"bufferViews\": [\n"
             << R"(    {"buffer": 0, "byteOffset": )" << positions_offset
             << ", \"byteLength\": " << positions_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << normals_offset
             << ", \"byteLength\": " << normals_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << joints_offset
             << ", \"byteLength\": " << joints_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << weights_offset
             << ", \"byteLength\": " << weights_length << ", \"target\": 34962},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << indices_offset
             << ", \"byteLength\": " << indices_length << ", \"target\": 34963},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << ibm_offset
             << ", \"byteLength\": " << ibm_length << "},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << times_offset
             << ", \"byteLength\": " << times_length << "},\n"
             << R"(    {"buffer": 0, "byteOffset": )" << translations_offset
             << ", \"byteLength\": " << translations_length << "}\n"
             << "  ],\n"
             << "  \"accessors\": [\n"
             << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": "
                "\"VEC3\"},\n"
             << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 3, \"type\": "
                "\"VEC3\"},\n"
             << "    {\"bufferView\": 2, \"componentType\": 5123, \"count\": 3, \"type\": "
                "\"VEC4\"},\n"
             << "    {\"bufferView\": 3, \"componentType\": 5126, \"count\": 3, \"type\": "
                "\"VEC4\"},\n"
             << "    {\"bufferView\": 4, \"componentType\": 5123, \"count\": 3, \"type\": "
                "\"SCALAR\"},\n"
             << "    {\"bufferView\": 5, \"componentType\": 5126, \"count\": 1, \"type\": "
                "\"MAT4\"},\n"
             << "    {\"bufferView\": 6, \"componentType\": 5126, \"count\": 2, \"type\": "
                "\"SCALAR\"},\n"
             << "    {\"bufferView\": 7, \"componentType\": 5126, \"count\": 2, \"type\": "
                "\"VEC3\"}\n"
             << "  ],\n"
             << "  \"meshes\": [{\"primitives\": [{\n"
             << "    \"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, \"JOINTS_0\": 2, "
                "\"WEIGHTS_0\": 3},\n"
             << "    \"indices\": 4\n"
             << "  }]}],\n"
             << "  \"nodes\": [{\"name\": \"joint0\"}, {\"mesh\": 0, \"skin\": 0}],\n"
             << "  \"skins\": [{\"joints\": [0], \"inverseBindMatrices\": 5}],\n"
             << "  \"animations\": [{\n"
             << "    \"name\": \"idle\",\n"
             << "    \"samplers\": [{\"input\": 6, \"output\": 7, \"interpolation\": "
                "\"LINEAR\"}],\n"
             << "    \"channels\": [{\"sampler\": 0, \"target\": {\"node\": 0, \"path\": "
                "\"translation\"}}]\n"
             << "  }],\n"
             << "  \"scenes\": [{\"nodes\": [1]}],\n"
             << "  \"scene\": 0\n"
             << "}\n";
    if (!gltf_out.good()) {
        ADD_FAILURE() << "failed to write animated fixture glTF output path: " << gltf_path;
        return {};
    }
    return gltf_path;
}

std::filesystem::path write_multi_primitive_static_gltf_fixture(const std::filesystem::path& dir) {
    const std::filesystem::path gltf_path = dir / "multi_primitive_static.gltf";
    const std::filesystem::path texture_a_path = dir / "albedo_a.png";
    const std::filesystem::path texture_b_path = dir / "albedo_b.png";
    {
        std::ofstream texture_a(texture_a_path, std::ios::binary);
        if (!texture_a.is_open()) {
            ADD_FAILURE() << "failed to open multi-primitive fixture texture A path: "
                          << texture_a_path;
            return {};
        }
        texture_a << "fake_png_a";
    }
    {
        std::ofstream texture_b(texture_b_path, std::ios::binary);
        if (!texture_b.is_open()) {
            ADD_FAILURE() << "failed to open multi-primitive fixture texture B path: "
                          << texture_b_path;
            return {};
        }
        texture_b << "fake_png_b";
    }

    constexpr char kMultiPrimitiveStaticGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAAAAAAIA/"
        "AAAAAAAAgD8AAIA/"
        "\",\"byteLength\":72}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":72}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":0,\"byteOffset\":36,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"images\":[{\"uri\":\"albedo_a.png\"},{\"uri\":\"albedo_b.png\"}],"
        "\"textures\":[{\"source\":0},{\"source\":1}],"
        "\"materials\":["
        "{"
        "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.2,0.4,0.6,0.5],\"baseColorTexture\":"
        "{\"index\":0}},"
        "\"alphaMode\":\"BLEND\","
        "\"doubleSided\":true"
        "},"
        "{"
        "\"pbrMetallicRoughness\":{\"baseColorFactor\":[1.0,0.0,0.0,1.0],\"baseColorTexture\":"
        "{\"index\":1}},"
        "\"alphaMode\":\"MASK\","
        "\"alphaCutoff\":0.33"
        "}"
        "],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0},\"material\":0,\"mode\":4},"
        "{\"attributes\":{\"POSITION\":1},\"material\":1,\"mode\":4}"
        "]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    std::ofstream gltf_out(gltf_path, std::ios::binary);
    if (!gltf_out.is_open()) {
        ADD_FAILURE() << "failed to open multi-primitive fixture glTF output path: " << gltf_path;
        return {};
    }
    gltf_out << kMultiPrimitiveStaticGltf;
    if (!gltf_out.good()) {
        ADD_FAILURE() << "failed to write multi-primitive fixture glTF output path: " << gltf_path;
        return {};
    }
    return gltf_path;
}

std::filesystem::path
write_mixed_non_triangle_and_triangle_static_gltf_fixture(const std::filesystem::path& dir) {
    const std::filesystem::path gltf_path = dir / "mixed_line_and_triangle.gltf";

    constexpr char kMixedLineAndTriangleGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAAAAAAIA/"
        "AAAAAAAAgD8AAIA/"
        "\",\"byteLength\":72}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":72}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":0,\"byteOffset\":36,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"materials\":["
        "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.3,0.3,0.3,1.0]}},"
        "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.1,0.1,0.7]},\"alphaMode\":"
        "\"BLEND\"}"
        "],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0},\"material\":0,\"mode\":1},"
        "{\"attributes\":{\"POSITION\":1},\"material\":1,\"mode\":4}"
        "]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    std::ofstream gltf_out(gltf_path, std::ios::binary);
    if (!gltf_out.is_open()) {
        ADD_FAILURE() << "failed to open mixed-primitive fixture glTF output path: " << gltf_path;
        return {};
    }
    gltf_out << kMixedLineAndTriangleGltf;
    if (!gltf_out.good()) {
        ADD_FAILURE() << "failed to write mixed-primitive fixture glTF output path: " << gltf_path;
        return {};
    }
    return gltf_path;
}

std::filesystem::path write_texturemap_static_gltf_fixture(const std::filesystem::path& dir) {
    const std::filesystem::path gltf_path = dir / "texturemap_static.gltf";
    const std::filesystem::path body_texture_path = dir / "body.png";
    const std::filesystem::path head_override_path = dir / "head_override.png";
    {
        std::ofstream body_texture(body_texture_path, std::ios::binary);
        if (!body_texture.is_open()) {
            ADD_FAILURE() << "failed to open body texture fixture path: " << body_texture_path;
            return {};
        }
        body_texture << "fake_png_body";
    }
    {
        std::ofstream head_texture(head_override_path, std::ios::binary);
        if (!head_texture.is_open()) {
            ADD_FAILURE() << "failed to open head override fixture path: " << head_override_path;
            return {};
        }
        head_texture << "fake_png_head";
    }

    constexpr char kTexturemapStaticGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAAAAAAIA/"
        "AAAAAAAAgD8AAIA/"
        "\",\"byteLength\":72}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":72}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":0,\"byteOffset\":36,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"images\":[{\"uri\":\"body.png\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":["
        "{\"name\":\"Head\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1.0,1.0,1.0,1.0]}},"
        "{\"name\":\"Body\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.9,0.8,0.7,1.0],"
        "\"baseColorTexture\":{\"index\":0}}}"
        "],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0},\"material\":0,\"mode\":4},"
        "{\"attributes\":{\"POSITION\":1},\"material\":1,\"mode\":4}"
        "]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    std::ofstream gltf_out(gltf_path, std::ios::binary);
    if (!gltf_out.is_open()) {
        ADD_FAILURE() << "failed to open texturemap fixture glTF output path: " << gltf_path;
        return {};
    }
    gltf_out << kTexturemapStaticGltf;
    if (!gltf_out.good()) {
        ADD_FAILURE() << "failed to write texturemap fixture glTF output path: " << gltf_path;
        return {};
    }
    return gltf_path;
}

std::filesystem::path
write_texturemap_ambiguous_material_fixture(const std::filesystem::path& dir) {
    const std::filesystem::path gltf_path = dir / "texturemap_ambiguous.gltf";
    constexpr char kTexturemapAmbiguousStaticGltf[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAAAAAAIA/"
        "AAAAAAAAgD8AAIA/"
        "\",\"byteLength\":72}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":72}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":0,\"byteOffset\":36,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"materials\":["
        "{\"name\":\"Shared\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1.0,1.0,1.0,1.0]}},"
        "{\"name\":\"Shared\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.6,0.6,0.6,1.0]}}"
        "],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0},\"material\":0,\"mode\":4},"
        "{\"attributes\":{\"POSITION\":1},\"material\":1,\"mode\":4}"
        "]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    std::ofstream gltf_out(gltf_path, std::ios::binary);
    if (!gltf_out.is_open()) {
        ADD_FAILURE() << "failed to open ambiguous texturemap fixture glTF output path: "
                      << gltf_path;
        return {};
    }
    gltf_out << kTexturemapAmbiguousStaticGltf;
    if (!gltf_out.good()) {
        ADD_FAILURE() << "failed to write ambiguous texturemap fixture glTF output path: "
                      << gltf_path;
        return {};
    }
    return gltf_path;
}

class FakeSdlRuntime final : public ISdlRuntime {
  public:
    std::uint64_t now_ticks_ns = 0U;
    int pixel_width = 1280;
    int pixel_height = 720;
    mutable std::vector<SDL_Event> queued_events;

    [[nodiscard]] std::uint64_t get_ticks_ns() const override {
        return now_ticks_ns;
    }
    [[nodiscard]] bool init_video() const override {
        return true;
    }
    void quit() const override {}
    [[nodiscard]] bool has_primary_display() const override {
        return true;
    }
    [[nodiscard]] SDL_Window* create_window(const char* title, int width, int height,
                                            std::uint64_t flags) const override {
        (void)title;
        (void)width;
        (void)height;
        (void)flags;
        return reinterpret_cast<SDL_Window*>(0x1);
    }
    [[nodiscard]] SDL_Renderer* create_renderer(SDL_Window* window) const override {
        (void)window;
        return nullptr;
    }
    void destroy_renderer(SDL_Renderer* renderer) const override {
        (void)renderer;
    }
    void destroy_window(SDL_Window* window) const override {
        (void)window;
    }
    void maximize_window(SDL_Window* window) const override {
        (void)window;
    }
    [[nodiscard]] bool poll_event(SDL_Event* event) const override {
        if (queued_events.empty()) {
            return false;
        }
        *event = queued_events.front();
        queued_events.erase(queued_events.begin());
        return true;
    }
    [[nodiscard]] const bool* get_keyboard_state(int* key_count) const override {
        static const std::array<bool, SDL_SCANCODE_COUNT> keys{};
        if (key_count != nullptr) {
            *key_count = SDL_SCANCODE_COUNT;
        }
        return keys.data();
    }
    [[nodiscard]] bool get_window_size_in_pixels(SDL_Window* window, int* width,
                                                 int* height) const override {
        (void)window;
        if (width != nullptr) {
            *width = pixel_width;
        }
        if (height != nullptr) {
            *height = pixel_height;
        }
        return true;
    }
    [[nodiscard]] bool get_window_size(SDL_Window* window, int* width, int* height) const override {
        return get_window_size_in_pixels(window, width, height);
    }
    void set_window_bordered(SDL_Window* window, bool bordered) const override {
        (void)window;
        (void)bordered;
    }
    [[nodiscard]] bool set_window_relative_mouse_mode(SDL_Window* window,
                                                      bool enabled) const override {
        (void)window;
        (void)enabled;
        return true;
    }
};

animated_gltf::AnimatedGltfAsset make_test_asset_with_two_clips() {
    animated_gltf::AnimatedGltfAsset asset;
    asset.skeleton.joints.resize(1U);
    asset.bind_local_transforms.resize(1U);
    asset.bind_prefix_matrices = { Mat4::identity() };

    animated_gltf::SkinnedPrimitive primitive;
    primitive.vertices = {
        animated_gltf::SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
            .uv = Vec2{ .x = 0.0F, .y = 0.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
        animated_gltf::SkinnedVertex{
            .position = Vec3{ .x = 1.0F, .y = 0.0F, .z = 0.0F },
            .uv = Vec2{ .x = 1.0F, .y = 0.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
        animated_gltf::SkinnedVertex{
            .position = Vec3{ .x = 0.0F, .y = 1.0F, .z = 0.0F },
            .uv = Vec2{ .x = 0.0F, .y = 1.0F },
            .joints = { 0U, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        },
    };
    primitive.indices = { 0U, 1U, 2U };
    asset.primitives.push_back(std::move(primitive));

    animated_gltf::AnimationClip idle;
    idle.name = "idle";
    idle.duration_seconds = 1.0F;
    idle.joint_tracks.resize(1U);
    idle.joint_tracks[0].translations = {
        animated_gltf::Vec3Keyframe{ .time_seconds = 0.0F,
                                     .value = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } },
        animated_gltf::Vec3Keyframe{ .time_seconds = 1.0F,
                                     .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } },
    };

    animated_gltf::AnimationClip walk;
    walk.name = "walk";
    walk.duration_seconds = 1.0F;
    walk.joint_tracks.resize(1U);
    walk.joint_tracks[0].translations = {
        animated_gltf::Vec3Keyframe{ .time_seconds = 0.0F,
                                     .value = Vec3{ .x = 10.0F, .y = 0.0F, .z = 0.0F } },
        animated_gltf::Vec3Keyframe{ .time_seconds = 1.0F,
                                     .value = Vec3{ .x = 12.0F, .y = 0.0F, .z = 0.0F } },
    };

    asset.clips.push_back(std::move(idle));
    asset.clips.push_back(std::move(walk));
    return asset;
}

animated_gltf::AnimatedGltfAsset make_large_joint_test_asset() {
    animated_gltf::AnimatedGltfAsset asset;
    asset.skeleton.joints.resize(66U);
    asset.bind_local_transforms.resize(66U);
    asset.bind_prefix_matrices.assign(66U, Mat4::identity());

    animated_gltf::SkinnedPrimitive primitive;
    primitive.vertices.reserve(66U);
    primitive.indices.reserve(66U);
    for (std::uint16_t joint = 0U; joint < 66U; ++joint) {
        primitive.vertices.push_back(animated_gltf::SkinnedVertex{
            .position = Vec3{ .x = static_cast<float>(joint), .y = 0.0F, .z = 0.0F },
            .uv = Vec2{ .x = 0.0F, .y = 0.0F },
            .joints = { joint, 0U, 0U, 0U },
            .weights = { 1.0F, 0.0F, 0.0F, 0.0F },
        });
        primitive.indices.push_back(static_cast<std::uint32_t>(joint));
    }
    asset.primitives.push_back(std::move(primitive));

    animated_gltf::AnimationClip idle;
    idle.name = "idle";
    idle.duration_seconds = 1.0F;
    idle.joint_tracks.resize(66U);
    idle.joint_tracks[0].translations = {
        animated_gltf::Vec3Keyframe{ .time_seconds = 0.0F,
                                     .value = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F } },
        animated_gltf::Vec3Keyframe{ .time_seconds = 1.0F,
                                     .value = Vec3{ .x = 2.0F, .y = 0.0F, .z = 0.0F } },
    };
    animated_gltf::AnimationClip walk = idle;
    walk.name = "walk";
    animated_gltf::AnimationClip action = idle;
    action.name = "action";
    asset.clips.push_back(std::move(idle));
    asset.clips.push_back(std::move(walk));
    asset.clips.push_back(std::move(action));
    return asset;
}

class ClientAppAnimationTestFixture : public ::testing::Test {
  protected:
    void load_startup_mesh_with_env(const std::string& animated_asset_path,
                                    const std::string& mesh_asset_path) {
        ScopedEnvVar animated_env("ISLA_ANIMATED_GLTF_ASSET", animated_asset_path.c_str());
        ScopedEnvVar mesh_env("ISLA_MESH_ASSET", mesh_asset_path.c_str());
        internal::ClientAppTestHooks::load_startup_mesh(app_);
    }

    [[nodiscard]] const RenderWorld& world() const {
        return internal::ClientAppTestHooks::world(app_);
    }

    FakeSdlRuntime runtime_;
    ClientApp app_{ runtime_ };
};

struct TextureRemapExpectation {
    std::optional<std::string> material0_texture_file;
    std::optional<std::string> material1_texture_file;
    bool expect_material0_texture_empty = false;
    bool expect_material1_texture_empty = false;
    std::optional<float> material0_alpha_cutoff;
    std::optional<std::size_t> objects_size;
};

struct TextureRemapCase {
    std::string test_name;
    std::string temp_dir_prefix;
    bool use_ambiguous_fixture = false;
    std::string texturemap_file_name;
    std::vector<std::pair<std::string, std::string>> files_to_create;
    std::string texturemap_json;
    TextureRemapExpectation expectation;
};

class ClientAppTextureRemapParamFixture : public ClientAppAnimationTestFixture,
                                          public ::testing::WithParamInterface<TextureRemapCase> {
  protected:
    std::filesystem::path create_fixture_asset(const TextureRemapCase& test_case,
                                               const std::filesystem::path& dir) {
        return test_case.use_ambiguous_fixture ? write_texturemap_ambiguous_material_fixture(dir)
                                               : write_texturemap_static_gltf_fixture(dir);
    }

    void write_file(const std::filesystem::path& path, std::string_view contents) {
        std::ofstream out(path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << contents;
        ASSERT_TRUE(out.good());
    }
};

TEST_F(ClientAppAnimationTestFixture, NonMonotonicClockClampsTickDeltaToZero) {
    internal::ClientAppTestHooks::set_last_tick_ns(app_, 100U);
    runtime_.now_ticks_ns = 50U;

    internal::ClientAppTestHooks::tick(app_);
    EXPECT_NEAR(internal::ClientAppTestHooks::world(app_).sim_time_seconds(), 0.0F, 1.0e-6F);
}

TEST_F(ClientAppAnimationTestFixture, TickAdvancesAnimationAndMeshTriangles) {
    internal::ClientAppTestHooks::set_animated_asset(app_, make_test_asset_with_two_clips());
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app_);
    internal::ClientAppTestHooks::set_last_tick_ns(app_, 0U);
    runtime_.now_ticks_ns = 500000000ULL;

    internal::ClientAppTestHooks::tick(app_);

    const RenderWorld& world = internal::ClientAppTestHooks::world(app_);
    ASSERT_FALSE(world.meshes().empty());
    ASSERT_FALSE(world.meshes()[0].triangles().empty());
    EXPECT_NEAR(world.sim_time_seconds(), 0.5F, 1.0e-4F);
    EXPECT_NEAR(world.meshes()[0].triangles()[0].a.x, 1.0F, 1.0e-4F);
}

TEST_F(ClientAppAnimationTestFixture, AnimatedTickUpdatesMeshInPlaceWithoutTriangleReallocation) {
    internal::ClientAppTestHooks::set_animated_asset(app_, make_test_asset_with_two_clips());
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app_);
    internal::ClientAppTestHooks::set_last_tick_ns(app_, 0U);
    runtime_.now_ticks_ns = 500000000ULL;

    internal::ClientAppTestHooks::tick(app_);

    RenderWorld& world = internal::ClientAppTestHooks::mutable_world(app_);
    ASSERT_FALSE(world.meshes().empty());
    const Triangle* triangles_ptr_after_first_tick = world.meshes()[0].triangles().data();
    const std::size_t triangles_capacity_after_first_tick =
        world.meshes()[0].triangles().capacity();

    runtime_.now_ticks_ns = 1000000000ULL;
    internal::ClientAppTestHooks::tick(app_);

    ASSERT_FALSE(world.meshes()[0].triangles().empty());
    EXPECT_EQ(world.meshes()[0].triangles().data(), triangles_ptr_after_first_tick);
    EXPECT_EQ(world.meshes()[0].triangles().capacity(), triangles_capacity_after_first_tick);
}

TEST_F(ClientAppAnimationTestFixture, AnimatedTickKeepsTriangleStorageStableAcrossManyFrames) {
    internal::ClientAppTestHooks::set_animated_asset(app_, make_test_asset_with_two_clips());
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app_);
    internal::ClientAppTestHooks::set_last_tick_ns(app_, 0U);
    runtime_.now_ticks_ns = 500000000ULL;

    internal::ClientAppTestHooks::tick(app_);

    RenderWorld& world = internal::ClientAppTestHooks::mutable_world(app_);
    ASSERT_FALSE(world.meshes().empty());
    const Triangle* stable_ptr = world.meshes()[0].triangles().data();
    const std::size_t stable_capacity = world.meshes()[0].triangles().capacity();

    for (int frame = 0; frame < 120; ++frame) {
        runtime_.now_ticks_ns += 16666666ULL;
        internal::ClientAppTestHooks::tick(app_);
        ASSERT_FALSE(world.meshes()[0].triangles().empty());
        EXPECT_EQ(world.meshes()[0].triangles().data(), stable_ptr);
        EXPECT_EQ(world.meshes()[0].triangles().capacity(), stable_capacity);
    }
}

TEST_F(ClientAppAnimationTestFixture, AnimatedTickDefersBoundsRecomputeUntilIntervalBoundary) {
    internal::ClientAppTestHooks::set_animated_asset(app_, make_test_asset_with_two_clips());
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app_);
    internal::ClientAppTestHooks::set_last_tick_ns(app_, 0U);

    RenderWorld& world = internal::ClientAppTestHooks::mutable_world(app_);
    ASSERT_FALSE(world.meshes().empty());
    const BoundingSphere initial_bounds = world.meshes()[0].local_bounds();

    for (int frame = 0; frame < 29; ++frame) {
        runtime_.now_ticks_ns += 16666666ULL;
        internal::ClientAppTestHooks::tick(app_);
    }

    const BoundingSphere deferred_bounds = world.meshes()[0].local_bounds();
    EXPECT_FLOAT_EQ(deferred_bounds.center.x, initial_bounds.center.x);
    EXPECT_FLOAT_EQ(deferred_bounds.center.y, initial_bounds.center.y);
    EXPECT_FLOAT_EQ(deferred_bounds.center.z, initial_bounds.center.z);
    EXPECT_FLOAT_EQ(deferred_bounds.radius, initial_bounds.radius);

    runtime_.now_ticks_ns += 16666666ULL;
    internal::ClientAppTestHooks::tick(app_);
    const BoundingSphere refreshed_bounds = world.meshes()[0].local_bounds();
    EXPECT_GT(refreshed_bounds.center.x, initial_bounds.center.x + 0.1F);
}

TEST_F(ClientAppAnimationTestFixture, GpuAuthoritativeAnimationUpdatesPaletteWithoutGeometryChurn) {
    internal::ClientAppTestHooks::set_animated_asset(app_, make_test_asset_with_two_clips());
    internal::ClientAppTestHooks::set_gpu_skinning_authoritative(app_, true);
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app_);
    internal::ClientAppTestHooks::set_last_tick_ns(app_, 0U);

    RenderWorld& world = internal::ClientAppTestHooks::mutable_world(app_);
    ASSERT_FALSE(world.meshes().empty());
    ASSERT_TRUE(world.meshes()[0].has_skinned_geometry());
    ASSERT_FALSE(world.meshes()[0].skin_palette().empty());
    const std::uint64_t stable_geometry_revision = world.meshes()[0].geometry_revision();

    runtime_.now_ticks_ns = 500000000ULL;
    internal::ClientAppTestHooks::tick(app_);
    ASSERT_FALSE(world.meshes()[0].skin_palette().empty());
    const float tx_after_first_tick = world.meshes()[0].skin_palette()[0].elements[12];
    EXPECT_NEAR(tx_after_first_tick, 1.0F, 1.0e-4F);
    EXPECT_EQ(world.meshes()[0].geometry_revision(), stable_geometry_revision);

    runtime_.now_ticks_ns = 1000000000ULL;
    internal::ClientAppTestHooks::tick(app_);
    ASSERT_FALSE(world.meshes()[0].skin_palette().empty());
    const float tx_after_second_tick = world.meshes()[0].skin_palette()[0].elements[12];
    EXPECT_NEAR(tx_after_second_tick, 0.0F, 1.0e-4F);
    EXPECT_EQ(world.meshes()[0].geometry_revision(), stable_geometry_revision);
}

TEST_F(ClientAppAnimationTestFixture,
       GpuAuthoritativeLargeSkeletonIsPartitionedToLocalPaletteBudget) {
    internal::ClientAppTestHooks::set_animated_asset(app_, make_large_joint_test_asset());
    internal::ClientAppTestHooks::set_gpu_skinning_authoritative(app_, true);
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app_);

    const RenderWorld& world = internal::ClientAppTestHooks::world(app_);
    ASSERT_GE(world.meshes().size(), 2U);
    for (const MeshData& mesh : world.meshes()) {
        ASSERT_TRUE(mesh.has_skinned_geometry());
        ASSERT_LE(mesh.skin_palette().size(), 64U);
        for (const SkinnedMeshVertex& vertex : mesh.skinned_vertices()) {
            EXPECT_LT(vertex.joints[0], 64U);
        }
    }
}

TEST_F(ClientAppAnimationTestFixture, GpuAuthoritativePartitioningIsStableAcrossRepeatedPopulate) {
    internal::ClientAppTestHooks::set_animated_asset(app_, make_large_joint_test_asset());
    internal::ClientAppTestHooks::set_gpu_skinning_authoritative(app_, true);

    internal::ClientAppTestHooks::populate_world_from_animated_asset(app_);
    const RenderWorld& first_world = internal::ClientAppTestHooks::world(app_);
    ASSERT_FALSE(first_world.meshes().empty());
    std::vector<std::size_t> first_palette_sizes;
    first_palette_sizes.reserve(first_world.meshes().size());
    for (const MeshData& mesh : first_world.meshes()) {
        first_palette_sizes.push_back(mesh.skin_palette().size());
    }
    const std::size_t first_mesh_count = first_world.meshes().size();
    const std::size_t first_object_count = first_world.objects().size();

    internal::ClientAppTestHooks::populate_world_from_animated_asset(app_);
    const RenderWorld& second_world = internal::ClientAppTestHooks::world(app_);
    ASSERT_EQ(second_world.meshes().size(), first_mesh_count);
    ASSERT_EQ(second_world.objects().size(), first_object_count);
    ASSERT_EQ(second_world.meshes().size(), first_palette_sizes.size());
    for (std::size_t i = 0U; i < second_world.meshes().size(); ++i) {
        EXPECT_EQ(second_world.meshes()[i].skin_palette().size(), first_palette_sizes[i]);
    }
}

TEST_F(ClientAppAnimationTestFixture, PhysicsColliderProxyFollowsAnimatedJointPose) {
    animated_gltf::AnimatedGltfAsset asset = make_test_asset_with_two_clips();
    asset.skeleton.joints[0].name = "root";
    internal::ClientAppTestHooks::set_animated_asset(app_, std::move(asset));

    pmx_physics_sidecar::SidecarData sidecar;
    sidecar.colliders.push_back(pmx_physics_sidecar::Collider{
        .id = "head_col",
        .bone_name = "root",
        .shape = pmx_physics_sidecar::ColliderShape::Sphere,
        .offset = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
        .rotation_euler_deg = Vec3{ .x = 0.0F, .y = 0.0F, .z = 0.0F },
        .is_trigger = false,
        .layer = 1U,
        .mask = 1U,
        .radius = 0.5F,
    });
    internal::ClientAppTestHooks::set_physics_sidecar(app_, std::move(sidecar));

    internal::ClientAppTestHooks::populate_world_from_animated_asset(app_);
    ASSERT_EQ(internal::ClientAppTestHooks::physics_collider_binding_count(app_), 1U);

    const RenderWorld& world_at_bind = internal::ClientAppTestHooks::world(app_);
    ASSERT_GE(world_at_bind.meshes().size(), 2U);
    ASSERT_GE(world_at_bind.objects().size(), 2U);
    ASSERT_GE(world_at_bind.materials().size(), 1U);
    const std::size_t proxy_mesh_id = world_at_bind.objects().back().mesh_id;
    ASSERT_LT(proxy_mesh_id, world_at_bind.meshes().size());
    ASSERT_FALSE(world_at_bind.meshes()[proxy_mesh_id].triangles().empty());
    const float bind_x = world_at_bind.meshes()[proxy_mesh_id].triangles()[0].a.x;
    EXPECT_NEAR(bind_x, 0.0F, 1.0e-4F);

    internal::ClientAppTestHooks::set_last_tick_ns(app_, 0U);
    runtime_.now_ticks_ns = 500000000ULL;
    internal::ClientAppTestHooks::tick(app_);

    const RenderWorld& world_after_tick = internal::ClientAppTestHooks::world(app_);
    ASSERT_LT(proxy_mesh_id, world_after_tick.meshes().size());
    ASSERT_FALSE(world_after_tick.meshes()[proxy_mesh_id].triangles().empty());
    const float proxy_x = world_after_tick.meshes()[proxy_mesh_id].triangles()[0].a.x;
    EXPECT_NEAR(proxy_x, 1.0F, 1.0e-4F);
}

TEST_F(ClientAppAnimationTestFixture, LoadStartupMeshAnimatedSidecarCreatesColliderProxyBindings) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_client_app_test_anim_sidecar");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path =
        write_skinned_with_animation_gltf_fixture(temp_dir.path());
    ASSERT_FALSE(gltf_path.empty());
    const std::filesystem::path sidecar_path =
        gltf_path.parent_path() / "skinned_with_clip.physics.json";
    {
        std::ofstream out(sidecar_path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << "{";
        out << R"("schema_version":"1.0.0",)";
        out << "\"converter\":{\"name\":\"conv\",\"version\":\"1\",\"command\":\"x\","
               "\"timestamp_utc\":\"2026-03-01T00:00:00Z\"},";
        out << R"("collision_layers":[{"index":0,"name":"default"}],)";
        out << "\"colliders\":[{\"id\":\"c0\",\"bone_name\":\"joint0\",\"shape\":\"sphere\","
               "\"offset\":[0,0,0],\"rotation_euler_deg\":[0,0,0],\"is_trigger\":false,"
               "\"layer\":1,\"mask\":1,\"radius\":0.2}],";
        out << "\"constraints\":[]";
        out << "}";
    }
    load_startup_mesh_with_env(gltf_path.string(), "");

    EXPECT_TRUE(internal::ClientAppTestHooks::has_animated_asset(app_));
    EXPECT_EQ(internal::ClientAppTestHooks::physics_collider_binding_count(app_), 1U);
    const RenderWorld& world = this->world();
    EXPECT_GE(world.meshes().size(), 2U);
    EXPECT_GE(world.objects().size(), 2U);
}

TEST_F(ClientAppAnimationTestFixture,
       LoadStartupMeshAnimatedMissingSidecarKeepsPlaybackWithoutPhysics) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_client_app_test_anim_no_sidecar");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path =
        write_skinned_with_animation_gltf_fixture(temp_dir.path());
    ASSERT_FALSE(gltf_path.empty());
    load_startup_mesh_with_env(gltf_path.string(), "");

    EXPECT_TRUE(internal::ClientAppTestHooks::has_animated_asset(app_));
    EXPECT_EQ(internal::ClientAppTestHooks::physics_collider_binding_count(app_), 0U);
}

TEST_F(ClientAppAnimationTestFixture,
       LoadStartupMeshAnimatedInvalidSidecarDoesNotBlockAnimationLoad) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_client_app_test_anim_bad_sidecar");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path =
        write_skinned_with_animation_gltf_fixture(temp_dir.path());
    ASSERT_FALSE(gltf_path.empty());
    const std::filesystem::path sidecar_path =
        gltf_path.parent_path() / "skinned_with_clip.physics.json";
    {
        std::ofstream out(sidecar_path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << "{";
        out << R"("schema_version":"1.0.1",)";
        out << "\"converter\":{\"name\":\"conv\",\"version\":\"1\",\"command\":\"x\","
               "\"timestamp_utc\":\"2026-03-01T00:00:00Z\"},";
        out << R"("collision_layers":[],"colliders":[],"constraints":[])";
        out << "}";
    }
    load_startup_mesh_with_env(gltf_path.string(), "");

    EXPECT_TRUE(internal::ClientAppTestHooks::has_animated_asset(app_));
    EXPECT_EQ(internal::ClientAppTestHooks::physics_collider_binding_count(app_), 0U);
}

TEST_F(ClientAppAnimationTestFixture, PhysicsProxyTriangleStorageStaysStableAcrossManyTicks) {
    animated_gltf::AnimatedGltfAsset asset = make_test_asset_with_two_clips();
    asset.skeleton.joints[0].name = "root";
    internal::ClientAppTestHooks::set_animated_asset(app_, std::move(asset));

    pmx_physics_sidecar::SidecarData sidecar;
    sidecar.colliders.push_back(pmx_physics_sidecar::Collider{
        .id = "proxy",
        .bone_name = "root",
        .shape = pmx_physics_sidecar::ColliderShape::Sphere,
        .offset = Vec3{},
        .rotation_euler_deg = Vec3{},
        .is_trigger = false,
        .layer = 1U,
        .mask = 1U,
        .radius = 0.5F,
    });
    internal::ClientAppTestHooks::set_physics_sidecar(app_, std::move(sidecar));
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app_);
    internal::ClientAppTestHooks::set_last_tick_ns(app_, 0U);

    const RenderWorld& world = internal::ClientAppTestHooks::world(app_);
    ASSERT_GE(world.objects().size(), 2U);
    const std::size_t proxy_mesh_id = world.objects().back().mesh_id;
    ASSERT_LT(proxy_mesh_id, world.meshes().size());
    const Triangle* stable_ptr = world.meshes()[proxy_mesh_id].triangles().data();
    const std::size_t stable_capacity = world.meshes()[proxy_mesh_id].triangles().capacity();

    for (int frame = 0; frame < 120; ++frame) {
        runtime_.now_ticks_ns += 16666666ULL;
        internal::ClientAppTestHooks::tick(app_);
        const RenderWorld& tick_world = internal::ClientAppTestHooks::world(app_);
        ASSERT_LT(proxy_mesh_id, tick_world.meshes().size());
        EXPECT_EQ(tick_world.meshes()[proxy_mesh_id].triangles().data(), stable_ptr);
        EXPECT_EQ(tick_world.meshes()[proxy_mesh_id].triangles().capacity(), stable_capacity);
    }
}

TEST_F(ClientAppAnimationTestFixture,
       GpuAuthoritativeTickUpdatesSkinPaletteAndPhysicsProxyTogether) {
    animated_gltf::AnimatedGltfAsset asset = make_test_asset_with_two_clips();
    asset.skeleton.joints[0].name = "root";
    internal::ClientAppTestHooks::set_animated_asset(app_, std::move(asset));
    internal::ClientAppTestHooks::set_gpu_skinning_authoritative(app_, true);

    pmx_physics_sidecar::SidecarData sidecar;
    sidecar.colliders.push_back(pmx_physics_sidecar::Collider{
        .id = "proxy",
        .bone_name = "root",
        .shape = pmx_physics_sidecar::ColliderShape::Sphere,
        .offset = Vec3{},
        .rotation_euler_deg = Vec3{},
        .is_trigger = false,
        .layer = 1U,
        .mask = 1U,
        .radius = 0.5F,
    });
    internal::ClientAppTestHooks::set_physics_sidecar(app_, std::move(sidecar));
    internal::ClientAppTestHooks::populate_world_from_animated_asset(app_);
    internal::ClientAppTestHooks::set_last_tick_ns(app_, 0U);

    const RenderWorld& bind_world = internal::ClientAppTestHooks::world(app_);
    ASSERT_GE(bind_world.objects().size(), 2U);
    const std::size_t skinned_mesh_id = bind_world.objects().front().mesh_id;
    const std::size_t proxy_mesh_id = bind_world.objects().back().mesh_id;
    ASSERT_LT(skinned_mesh_id, bind_world.meshes().size());
    ASSERT_LT(proxy_mesh_id, bind_world.meshes().size());
    const std::uint64_t stable_geometry_revision =
        bind_world.meshes()[skinned_mesh_id].geometry_revision();
    const float bind_proxy_x = bind_world.meshes()[proxy_mesh_id].triangles()[0].a.x;
    EXPECT_NEAR(bind_proxy_x, 0.0F, 1.0e-4F);

    runtime_.now_ticks_ns = 500000000ULL;
    internal::ClientAppTestHooks::tick(app_);

    const RenderWorld& tick_world = internal::ClientAppTestHooks::world(app_);
    ASSERT_LT(skinned_mesh_id, tick_world.meshes().size());
    ASSERT_LT(proxy_mesh_id, tick_world.meshes().size());
    ASSERT_FALSE(tick_world.meshes()[skinned_mesh_id].skin_palette().empty());
    EXPECT_NEAR(tick_world.meshes()[skinned_mesh_id].skin_palette()[0].elements[12], 1.0F, 1.0e-4F);
    EXPECT_EQ(tick_world.meshes()[skinned_mesh_id].geometry_revision(), stable_geometry_revision);
    EXPECT_NEAR(tick_world.meshes()[proxy_mesh_id].triangles()[0].a.x, 1.0F, 1.0e-4F);
}

TEST_F(ClientAppAnimationTestFixture,
       LoadStartupMeshResetsGpuAuthoritativeFlagWhenRendererUnsupported) {
    internal::ClientAppTestHooks::set_gpu_skinning_authoritative(app_, true);

    load_startup_mesh_with_env("", "");

    EXPECT_FALSE(internal::ClientAppTestHooks::gpu_skinning_authoritative(app_));
}

TEST_F(ClientAppAnimationTestFixture, EnvironmentConfigSelectsClipAndClampMode) {
    internal::ClientAppTestHooks::set_animated_asset(app_, make_test_asset_with_two_clips());

    ScopedEnvVar clip_env("ISLA_ANIM_CLIP", "walk");
    ScopedEnvVar mode_env("ISLA_ANIM_PLAYBACK_MODE", "clamp");

    internal::ClientAppTestHooks::configure_animation_playback_from_environment(app_);

    const auto& state = internal::ClientAppTestHooks::animation_playback(app_).state();
    EXPECT_EQ(state.clip_index, 1U);
    EXPECT_EQ(state.playback_mode, animated_gltf::ClipPlaybackMode::Clamp);
}

TEST_F(ClientAppAnimationTestFixture, FallbackWhenAnimatedAssetFailsToLoad) {
    load_startup_mesh_with_env("missing_file.gltf", "");

    EXPECT_FALSE(internal::ClientAppTestHooks::has_animated_asset(app_));
    EXPECT_TRUE(internal::ClientAppTestHooks::world(app_).meshes().empty());
}

TEST_F(ClientAppAnimationTestFixture, LoadStartupMeshUsesWorkspaceDefaultModelPathWhenUnset) {
    ScopedTempDir sandbox_dir = ScopedTempDir::create("isla_client_app_test_sandbox");
    ScopedTempDir workspace_dir = ScopedTempDir::create("isla_client_app_test_workspace");
    ASSERT_TRUE(sandbox_dir.is_valid());
    ASSERT_TRUE(workspace_dir.is_valid());
    ASSERT_TRUE(std::filesystem::create_directories(workspace_dir.path() / "models"));

    const std::filesystem::path default_glb_path = workspace_dir.path() / "models" / "model.glb";
    const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
    {
        std::ofstream out(default_glb_path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out.write(reinterpret_cast<const char*>(glb.data()),
                  static_cast<std::streamsize>(glb.size()));
    }

    ScopedCurrentPath cwd_guard(sandbox_dir.path());
    ASSERT_TRUE(cwd_guard.is_armed());
    ScopedEnvVar animated_env("ISLA_ANIMATED_GLTF_ASSET", "");
    ScopedEnvVar mesh_env("ISLA_MESH_ASSET", "");
    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace_dir.path().string().c_str());

    internal::ClientAppTestHooks::load_startup_mesh(app_);

    const RenderWorld& world = this->world();
    ASSERT_EQ(world.meshes().size(), 1U);
    ASSERT_EQ(world.objects().size(), 1U);
    EXPECT_FALSE(world.meshes()[0].triangles().empty());
}

TEST_F(ClientAppAnimationTestFixture, StaticFallbackAppliesVisibleAutoFitTransform) {

    ScopedTempDir temp_dir = ScopedTempDir::create("isla_client_app_test_obj");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path obj_path = temp_dir.path() / "triangle.obj";
    {
        std::ofstream out(obj_path);
        ASSERT_TRUE(out.is_open());
        out << "v 0 0 0\n";
        out << "v 1 0 0\n";
        out << "v 0 1 0\n";
        out << "f 1 2 3\n";
    }

    load_startup_mesh_with_env("", obj_path.string());

    const RenderWorld& world = internal::ClientAppTestHooks::world(app_);
    ASSERT_EQ(world.objects().size(), 1U);
    const Transform& transform = world.objects()[0].transform;
    EXPECT_GT(transform.scale.x, 1.0F);
    EXPECT_GT(transform.scale.y, 1.0F);
    EXPECT_GT(transform.scale.z, 1.0F);
    EXPECT_NE(transform.position.x, 0.0F);
    EXPECT_NE(transform.position.y, 0.0F);
}

TEST_F(ClientAppAnimationTestFixture, StaticLoadPreservesPerPrimitiveMaterialsAndSharedTransform) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_client_app_test_multi_primitive_static");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path =
        write_multi_primitive_static_gltf_fixture(temp_dir.path());
    ASSERT_FALSE(gltf_path.empty());
    ASSERT_TRUE(std::filesystem::exists(gltf_path));
    load_startup_mesh_with_env("", gltf_path.string());

    const RenderWorld& world = internal::ClientAppTestHooks::world(app_);
    ASSERT_EQ(world.meshes().size(), 2U);
    ASSERT_EQ(world.materials().size(), 2U);
    ASSERT_EQ(world.objects().size(), 2U);
    EXPECT_EQ(world.objects()[0].mesh_id, 0U);
    EXPECT_EQ(world.objects()[0].material_id, 0U);
    EXPECT_EQ(world.objects()[1].mesh_id, 1U);
    EXPECT_EQ(world.objects()[1].material_id, 1U);
    EXPECT_NEAR(world.materials()[0].base_color.r, 0.2F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[0].base_color.g, 0.4F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[0].base_color.b, 0.6F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[0].base_alpha, 0.5F, 1.0e-6F);
    EXPECT_EQ(world.materials()[0].blend_mode, MaterialBlendMode::AlphaBlend);
    EXPECT_EQ(world.materials()[0].cull_mode, MaterialCullMode::Disabled);
    EXPECT_EQ(world.materials()[0].albedo_texture_path,
              (temp_dir.path() / "albedo_a.png").lexically_normal().string());
    EXPECT_NEAR(world.materials()[1].base_color.r, 1.0F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[1].base_color.g, 0.0F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[1].base_color.b, 0.0F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[1].alpha_cutoff, 0.33F, 1.0e-6F);
    EXPECT_EQ(world.materials()[1].blend_mode, MaterialBlendMode::Opaque);
    EXPECT_EQ(world.materials()[1].cull_mode, MaterialCullMode::CounterClockwise);
    EXPECT_EQ(world.materials()[1].albedo_texture_path,
              (temp_dir.path() / "albedo_b.png").lexically_normal().string());
    EXPECT_FLOAT_EQ(world.objects()[0].transform.scale.x, world.objects()[1].transform.scale.x);
    EXPECT_FLOAT_EQ(world.objects()[0].transform.scale.y, world.objects()[1].transform.scale.y);
    EXPECT_FLOAT_EQ(world.objects()[0].transform.scale.z, world.objects()[1].transform.scale.z);
    EXPECT_FLOAT_EQ(world.objects()[0].transform.position.x,
                    world.objects()[1].transform.position.x);
    EXPECT_FLOAT_EQ(world.objects()[0].transform.position.y,
                    world.objects()[1].transform.position.y);
    EXPECT_FLOAT_EQ(world.objects()[0].transform.position.z,
                    world.objects()[1].transform.position.z);
}

TEST_F(ClientAppAnimationTestFixture, StaticLoadFailsWhenTextureRemapSidecarSchemaIsInvalid) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_client_app_test_texturemap_invalid");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path = write_texturemap_static_gltf_fixture(temp_dir.path());
    ASSERT_FALSE(gltf_path.empty());
    ASSERT_TRUE(std::filesystem::exists(gltf_path));

    const std::filesystem::path texturemap_path =
        temp_dir.path() / "texturemap_static.texturemap.json";
    {
        std::ofstream out(texturemap_path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << "{"
            << "\"schema_version\":\"1.0.1\","
            << "\"policy\":{\"override_mode\":\"if_missing\",\"path_scope\":\"asset_relative_"
               "only\"},"
            << "\"mappings\":[]"
            << "}";
        ASSERT_TRUE(out.good());
    }
    load_startup_mesh_with_env("", gltf_path.string());

    const RenderWorld& world = internal::ClientAppTestHooks::world(app_);
    EXPECT_TRUE(world.meshes().empty());
    EXPECT_TRUE(world.objects().empty());
}

TEST_P(ClientAppTextureRemapParamFixture, StaticLoadTextureRemapCaseMatrix) {
    const TextureRemapCase& test_case = GetParam();
    ScopedTempDir temp_dir = ScopedTempDir::create(test_case.temp_dir_prefix);
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path = create_fixture_asset(test_case, temp_dir.path());
    ASSERT_FALSE(gltf_path.empty());
    ASSERT_TRUE(std::filesystem::exists(gltf_path));

    for (const auto& [filename, contents] : test_case.files_to_create) {
        write_file(temp_dir.path() / filename, contents);
    }
    write_file(temp_dir.path() / test_case.texturemap_file_name, test_case.texturemap_json);
    load_startup_mesh_with_env("", gltf_path.string());

    const RenderWorld& loaded_world = world();
    ASSERT_EQ(loaded_world.materials().size(), 2U);
    if (test_case.expectation.objects_size.has_value()) {
        EXPECT_EQ(loaded_world.objects().size(), test_case.expectation.objects_size.value());
    }
    if (test_case.expectation.material0_texture_file.has_value()) {
        EXPECT_EQ(loaded_world.materials()[0].albedo_texture_path,
                  (temp_dir.path() / test_case.expectation.material0_texture_file.value())
                      .lexically_normal()
                      .string());
    }
    if (test_case.expectation.material1_texture_file.has_value()) {
        EXPECT_EQ(loaded_world.materials()[1].albedo_texture_path,
                  (temp_dir.path() / test_case.expectation.material1_texture_file.value())
                      .lexically_normal()
                      .string());
    }
    if (test_case.expectation.expect_material0_texture_empty) {
        EXPECT_TRUE(loaded_world.materials()[0].albedo_texture_path.empty());
    }
    if (test_case.expectation.expect_material1_texture_empty) {
        EXPECT_TRUE(loaded_world.materials()[1].albedo_texture_path.empty());
    }
    if (test_case.expectation.material0_alpha_cutoff.has_value()) {
        EXPECT_NEAR(loaded_world.materials()[0].alpha_cutoff,
                    test_case.expectation.material0_alpha_cutoff.value(), 1.0e-6F);
    }
}

INSTANTIATE_TEST_SUITE_P(
    StaticTextureRemapCases, ClientAppTextureRemapParamFixture,
    ::testing::Values(
        [] {
            TextureRemapCase value;
            value.test_name = "ByMaterialNameWhenMissing";
            value.temp_dir_prefix = "isla_client_app_test_texturemap";
            value.texturemap_file_name = "texturemap_static.texturemap.json";
            value.texturemap_json =
                "{"
                "\"schema_version\":\"1.0.0\","
                "\"policy\":{\"override_mode\":\"if_missing\",\"path_scope\":\"asset_relative_"
                "only\"},"
                "\"mappings\":["
                "{"
                "\"id\":\"head_by_name\","
                "\"target\":{\"material_name\":\"Head\"},"
                "\"albedo_texture\":\"head_override.png\","
                "\"alpha_cutoff\":0.5"
                "}"
                "]"
                "}";
            value.expectation.material0_texture_file = "head_override.png";
            value.expectation.material1_texture_file = "body.png";
            value.expectation.material0_alpha_cutoff = 0.5F;
            value.expectation.objects_size = 2U;
            return value;
        }(),
        [] {
            TextureRemapCase value;
            value.test_name = "AlwaysOverridesExistingGltfTexture";
            value.temp_dir_prefix = "isla_client_app_test_texturemap_always";
            value.texturemap_file_name = "texturemap_static.texturemap.json";
            value.files_to_create = { { "body_override.png", "fake_png_body_override" } };
            value.texturemap_json =
                "{"
                "\"schema_version\":\"1.0.0\","
                "\"policy\":{\"override_mode\":\"always\",\"path_scope\":\"asset_relative_only\"},"
                "\"mappings\":[{"
                "\"id\":\"body_override\","
                "\"target\":{\"material_name\":\"Body\"},"
                "\"albedo_texture\":\"body_override.png\""
                "}]"
                "}";
            value.expectation.material1_texture_file = "body_override.png";
            return value;
        }(),
        [] {
            TextureRemapCase value;
            value.test_name = "AppliesByMeshPrimitiveTuple";
            value.temp_dir_prefix = "isla_client_app_test_texturemap_tuple";
            value.texturemap_file_name = "texturemap_static.texturemap.json";
            value.files_to_create = { { "body_tuple_override.png",
                                        "fake_png_body_tuple_override" } };
            value.texturemap_json =
                "{"
                "\"schema_version\":\"1.0.0\","
                "\"policy\":{\"override_mode\":\"always\",\"path_scope\":\"asset_relative_only\"},"
                "\"mappings\":[{"
                "\"id\":\"body_by_tuple\","
                "\"target\":{\"mesh_index\":0,\"primitive_index\":1},"
                "\"albedo_texture\":\"body_tuple_override.png\""
                "}]"
                "}";
            value.expectation.material1_texture_file = "body_tuple_override.png";
            return value;
        }(),
        [] {
            TextureRemapCase value;
            value.test_name = "DuplicateKeyCollisionKeepsFirstMappingOnly";
            value.temp_dir_prefix = "isla_client_app_test_texturemap_duplicate";
            value.texturemap_file_name = "texturemap_static.texturemap.json";
            value.files_to_create = {
                { "head_first.png", "fake_png_head_first" },
                { "head_second.png", "fake_png_head_second" },
            };
            value.texturemap_json =
                "{"
                "\"schema_version\":\"1.0.0\","
                "\"policy\":{\"override_mode\":\"if_missing\",\"path_scope\":\"asset_relative_"
                "only\"},"
                "\"mappings\":["
                "{"
                "\"id\":\"head_first\","
                "\"target\":{\"material_name\":\"Head\"},"
                "\"albedo_texture\":\"head_first.png\""
                "},"
                "{"
                "\"id\":\"head_second_duplicate\","
                "\"target\":{\"material_name\":\"Head\"},"
                "\"albedo_texture\":\"head_second.png\""
                "}"
                "]"
                "}";
            value.expectation.material0_texture_file = "head_first.png";
            return value;
        }(),
        [] {
            TextureRemapCase value;
            value.test_name = "AmbiguousMaterialNameSkipsOverride";
            value.temp_dir_prefix = "isla_client_app_test_texturemap_ambiguous";
            value.use_ambiguous_fixture = true;
            value.texturemap_file_name = "texturemap_ambiguous.texturemap.json";
            value.files_to_create = { { "shared_override.png", "fake_png_shared_override" } };
            value.texturemap_json =
                "{"
                "\"schema_version\":\"1.0.0\","
                "\"policy\":{\"override_mode\":\"always\",\"path_scope\":\"asset_relative_only\"},"
                "\"mappings\":[{"
                "\"id\":\"shared_by_name\","
                "\"target\":{\"material_name\":\"Shared\"},"
                "\"albedo_texture\":\"shared_override.png\""
                "}]"
                "}";
            value.expectation.expect_material0_texture_empty = true;
            value.expectation.expect_material1_texture_empty = true;
            return value;
        }(),
        [] {
            TextureRemapCase value;
            value.test_name = "MissingTextureFileSkipsMapping";
            value.temp_dir_prefix = "isla_client_app_test_texturemap_missing_file";
            value.texturemap_file_name = "texturemap_static.texturemap.json";
            value.texturemap_json =
                "{"
                "\"schema_version\":\"1.0.0\","
                "\"policy\":{\"override_mode\":\"if_missing\",\"path_scope\":\"asset_relative_"
                "only\"},"
                "\"mappings\":[{"
                "\"id\":\"head_missing_file\","
                "\"target\":{\"material_name\":\"Head\"},"
                "\"albedo_texture\":\"missing_head.png\""
                "}]"
                "}";
            value.expectation.expect_material0_texture_empty = true;
            value.expectation.material1_texture_file = "body.png";
            return value;
        }()),
    [](const ::testing::TestParamInfo<TextureRemapCase>& info) { return info.param.test_name; });

TEST_F(ClientAppAnimationTestFixture,
       AnimatedEnvFallbackToStaticPreservesPerPrimitiveMaterialParity) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_client_app_test_multi_primitive_parity");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path =
        write_multi_primitive_static_gltf_fixture(temp_dir.path());
    ASSERT_FALSE(gltf_path.empty());
    ASSERT_TRUE(std::filesystem::exists(gltf_path));

    FakeSdlRuntime static_runtime;
    ClientApp static_app(static_runtime);
    {
        ScopedEnvVar animated_env("ISLA_ANIMATED_GLTF_ASSET", "");
        ScopedEnvVar mesh_env("ISLA_MESH_ASSET", gltf_path.string().c_str());
        internal::ClientAppTestHooks::load_startup_mesh(static_app);
    }
    const RenderWorld& static_world = internal::ClientAppTestHooks::world(static_app);
    ASSERT_EQ(static_world.materials().size(), 2U);
    ASSERT_EQ(static_world.meshes().size(), 2U);
    ASSERT_EQ(static_world.objects().size(), 2U);

    FakeSdlRuntime fallback_runtime;
    ClientApp fallback_app(fallback_runtime);
    {
        ScopedEnvVar animated_env("ISLA_ANIMATED_GLTF_ASSET", gltf_path.string().c_str());
        ScopedEnvVar mesh_env("ISLA_MESH_ASSET", "");
        internal::ClientAppTestHooks::load_startup_mesh(fallback_app);
    }
    const RenderWorld& fallback_world = internal::ClientAppTestHooks::world(fallback_app);
    ASSERT_EQ(fallback_world.materials().size(), static_world.materials().size());
    ASSERT_EQ(fallback_world.meshes().size(), static_world.meshes().size());
    ASSERT_EQ(fallback_world.objects().size(), static_world.objects().size());
    for (std::size_t i = 0U; i < static_world.materials().size(); ++i) {
        EXPECT_FLOAT_EQ(fallback_world.materials()[i].base_color.r,
                        static_world.materials()[i].base_color.r);
        EXPECT_FLOAT_EQ(fallback_world.materials()[i].base_color.g,
                        static_world.materials()[i].base_color.g);
        EXPECT_FLOAT_EQ(fallback_world.materials()[i].base_color.b,
                        static_world.materials()[i].base_color.b);
        EXPECT_FLOAT_EQ(fallback_world.materials()[i].base_alpha,
                        static_world.materials()[i].base_alpha);
        EXPECT_FLOAT_EQ(fallback_world.materials()[i].alpha_cutoff,
                        static_world.materials()[i].alpha_cutoff);
        EXPECT_EQ(fallback_world.materials()[i].blend_mode, static_world.materials()[i].blend_mode);
        EXPECT_EQ(fallback_world.materials()[i].cull_mode, static_world.materials()[i].cull_mode);
        EXPECT_EQ(fallback_world.materials()[i].albedo_texture_path,
                  static_world.materials()[i].albedo_texture_path);
    }
    for (std::size_t i = 0U; i < static_world.objects().size(); ++i) {
        EXPECT_EQ(fallback_world.objects()[i].mesh_id, static_world.objects()[i].mesh_id);
        EXPECT_EQ(fallback_world.objects()[i].material_id, static_world.objects()[i].material_id);
        EXPECT_FLOAT_EQ(fallback_world.objects()[i].transform.position.x,
                        static_world.objects()[i].transform.position.x);
        EXPECT_FLOAT_EQ(fallback_world.objects()[i].transform.position.y,
                        static_world.objects()[i].transform.position.y);
        EXPECT_FLOAT_EQ(fallback_world.objects()[i].transform.position.z,
                        static_world.objects()[i].transform.position.z);
        EXPECT_FLOAT_EQ(fallback_world.objects()[i].transform.scale.x,
                        static_world.objects()[i].transform.scale.x);
        EXPECT_FLOAT_EQ(fallback_world.objects()[i].transform.scale.y,
                        static_world.objects()[i].transform.scale.y);
        EXPECT_FLOAT_EQ(fallback_world.objects()[i].transform.scale.z,
                        static_world.objects()[i].transform.scale.z);
    }
}

TEST_F(ClientAppAnimationTestFixture,
       StaticLoadSkipsNonTrianglePrimitivesAndLoadsRenderableChunks) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_client_app_test_mixed_primitives");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path =
        write_mixed_non_triangle_and_triangle_static_gltf_fixture(temp_dir.path());
    ASSERT_FALSE(gltf_path.empty());
    ASSERT_TRUE(std::filesystem::exists(gltf_path));
    load_startup_mesh_with_env("", gltf_path.string());

    const RenderWorld& world = internal::ClientAppTestHooks::world(app_);
    ASSERT_EQ(world.meshes().size(), 1U);
    ASSERT_EQ(world.materials().size(), 1U);
    ASSERT_EQ(world.objects().size(), 1U);
    EXPECT_EQ(world.objects()[0].mesh_id, 0U);
    EXPECT_EQ(world.objects()[0].material_id, 0U);
    ASSERT_FALSE(world.meshes()[0].triangles().empty());
    EXPECT_NEAR(world.materials()[0].base_color.r, 0.8F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[0].base_color.g, 0.1F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[0].base_color.b, 0.1F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[0].base_alpha, 0.7F, 1.0e-6F);
    EXPECT_EQ(world.materials()[0].blend_mode, MaterialBlendMode::AlphaBlend);
}

TEST_F(ClientAppAnimationTestFixture,
       StaticLoadAggregateTransformIsDeterministicAcrossRepeatedLoads) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_client_app_test_transform_determinism");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path =
        write_multi_primitive_static_gltf_fixture(temp_dir.path());
    ASSERT_FALSE(gltf_path.empty());
    ASSERT_TRUE(std::filesystem::exists(gltf_path));

    const auto load_once = [&]() -> std::vector<Transform> {
        load_startup_mesh_with_env("", gltf_path.string());
        const RenderWorld& world = internal::ClientAppTestHooks::world(app_);
        std::vector<Transform> transforms;
        transforms.reserve(world.objects().size());
        for (const RenderObject& object : world.objects()) {
            transforms.push_back(object.transform);
        }
        return transforms;
    };

    const std::vector<Transform> first = load_once();
    const std::vector<Transform> second = load_once();
    ASSERT_EQ(first.size(), 2U);
    ASSERT_EQ(second.size(), first.size());
    for (std::size_t i = 0U; i < first.size(); ++i) {
        EXPECT_FLOAT_EQ(first[i].position.x, second[i].position.x);
        EXPECT_FLOAT_EQ(first[i].position.y, second[i].position.y);
        EXPECT_FLOAT_EQ(first[i].position.z, second[i].position.z);
        EXPECT_FLOAT_EQ(first[i].scale.x, second[i].scale.x);
        EXPECT_FLOAT_EQ(first[i].scale.y, second[i].scale.y);
        EXPECT_FLOAT_EQ(first[i].scale.z, second[i].scale.z);
    }
}

TEST_F(ClientAppAnimationTestFixture, StaticLoadPreservesPerObjectMaterialStateMappingContract) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_client_app_test_material_mapping");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path =
        write_multi_primitive_static_gltf_fixture(temp_dir.path());
    ASSERT_FALSE(gltf_path.empty());
    ASSERT_TRUE(std::filesystem::exists(gltf_path));
    load_startup_mesh_with_env("", gltf_path.string());

    const RenderWorld& world = internal::ClientAppTestHooks::world(app_);
    ASSERT_EQ(world.objects().size(), 2U);
    ASSERT_EQ(world.materials().size(), 2U);

    const Material& object0_material = world.materials().at(world.objects()[0].material_id);
    const Material& object1_material = world.materials().at(world.objects()[1].material_id);
    EXPECT_EQ(object0_material.blend_mode, MaterialBlendMode::AlphaBlend);
    EXPECT_EQ(object0_material.cull_mode, MaterialCullMode::Disabled);
    EXPECT_LT(object0_material.alpha_cutoff, 0.0F);
    EXPECT_EQ(object1_material.blend_mode, MaterialBlendMode::Opaque);
    EXPECT_EQ(object1_material.cull_mode, MaterialCullMode::CounterClockwise);
    EXPECT_NEAR(object1_material.alpha_cutoff, 0.33F, 1.0e-6F);
}

TEST_F(ClientAppAnimationTestFixture, AnimatedLoadNoClipsFallsBackToStaticWithFidelityInputs) {
    ScopedTempDir temp_dir = ScopedTempDir::create("isla_client_app_test_skinned_no_clips");
    ASSERT_TRUE(temp_dir.is_valid());
    const std::filesystem::path gltf_path =
        write_skinned_no_animation_gltf_fixture(temp_dir.path());
    ASSERT_FALSE(gltf_path.empty());
    ASSERT_TRUE(std::filesystem::exists(gltf_path));

    const animated_gltf::AnimatedGltfLoadResult animated_loaded =
        animated_gltf::load_from_file(gltf_path.string());
    ASSERT_TRUE(animated_loaded.ok) << animated_loaded.error_message;
    ASSERT_TRUE(animated_loaded.asset.clips.empty());
    load_startup_mesh_with_env(gltf_path.string(), "");

    EXPECT_FALSE(internal::ClientAppTestHooks::has_animated_asset(app_));
    const RenderWorld& world = internal::ClientAppTestHooks::world(app_);
    ASSERT_EQ(world.materials().size(), 1U);
    ASSERT_EQ(world.meshes().size(), 1U);
    ASSERT_EQ(world.objects().size(), 1U);
    EXPECT_NEAR(world.materials()[0].base_color.r, 0.3F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[0].base_color.g, 0.5F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[0].base_color.b, 0.7F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[0].base_alpha, 0.4F, 1.0e-6F);
    EXPECT_NEAR(world.materials()[0].alpha_cutoff, 0.5F, 1.0e-6F);
    EXPECT_EQ(world.materials()[0].blend_mode, MaterialBlendMode::Opaque);
    EXPECT_EQ(world.materials()[0].cull_mode, MaterialCullMode::Disabled);
    EXPECT_EQ(world.materials()[0].albedo_texture_path,
              (temp_dir.path() / "albedo.png").lexically_normal().string());
    ASSERT_FALSE(world.meshes()[0].triangles().empty());
    EXPECT_TRUE(world.meshes()[0].triangles()[0].has_vertex_normals);
}

TEST_F(ClientAppAnimationTestFixture, StaticLoadMaterialBaselineMatchesEnvAndDefaultPathFlows) {
    FakeSdlRuntime env_runtime;
    ClientApp env_app(env_runtime);
    ScopedTempDir env_workspace_dir = ScopedTempDir::create("isla_client_app_test_default_flow");
    ASSERT_TRUE(env_workspace_dir.is_valid());
    ASSERT_TRUE(std::filesystem::create_directories(env_workspace_dir.path() / "models"));
    const std::filesystem::path model_path = env_workspace_dir.path() / "models" / "model.glb";
    const std::vector<std::uint8_t> glb = make_minimal_triangle_glb();
    {
        std::ofstream out(model_path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out.write(reinterpret_cast<const char*>(glb.data()),
                  static_cast<std::streamsize>(glb.size()));
    }

    {
        ScopedEnvVar animated_env("ISLA_ANIMATED_GLTF_ASSET", "");
        ScopedEnvVar mesh_env("ISLA_MESH_ASSET", model_path.string().c_str());
        internal::ClientAppTestHooks::load_startup_mesh(env_app);
    }
    const RenderWorld& env_world = internal::ClientAppTestHooks::world(env_app);
    ASSERT_EQ(env_world.materials().size(), 1U);

    FakeSdlRuntime default_runtime;
    ClientApp default_app(default_runtime);
    ScopedTempDir sandbox_dir = ScopedTempDir::create("isla_client_app_test_default_flow_sandbox");
    ASSERT_TRUE(sandbox_dir.is_valid());
    ScopedCurrentPath cwd_guard(sandbox_dir.path());
    ASSERT_TRUE(cwd_guard.is_armed());
    {
        ScopedEnvVar animated_env("ISLA_ANIMATED_GLTF_ASSET", "");
        ScopedEnvVar mesh_env("ISLA_MESH_ASSET", "");
        ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY",
                                   env_workspace_dir.path().string().c_str());
        internal::ClientAppTestHooks::load_startup_mesh(default_app);
    }
    const RenderWorld& default_world = internal::ClientAppTestHooks::world(default_app);
    ASSERT_EQ(default_world.materials().size(), 1U);

    EXPECT_FLOAT_EQ(env_world.materials()[0].base_color.r,
                    default_world.materials()[0].base_color.r);
    EXPECT_FLOAT_EQ(env_world.materials()[0].base_color.g,
                    default_world.materials()[0].base_color.g);
    EXPECT_FLOAT_EQ(env_world.materials()[0].base_color.b,
                    default_world.materials()[0].base_color.b);
    EXPECT_FLOAT_EQ(env_world.materials()[0].base_alpha, default_world.materials()[0].base_alpha);
    EXPECT_EQ(env_world.materials()[0].blend_mode, default_world.materials()[0].blend_mode);
    EXPECT_EQ(env_world.materials()[0].cull_mode, default_world.materials()[0].cull_mode);
}

} // namespace
} // namespace isla::client
