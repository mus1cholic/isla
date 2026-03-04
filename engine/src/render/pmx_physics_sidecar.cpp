#include "engine/src/render/include/pmx_physics_sidecar.hpp"

#include "absl/log/log.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace isla::client::pmx_physics_sidecar {

namespace {

struct JsonValue {
    enum class Type {
        Null = 0,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Type type = Type::Null;
    bool bool_value = false;
    double number_value = 0.0;
    std::string string_value;
    std::vector<JsonValue> array_value;
    std::vector<std::pair<std::string, JsonValue>> object_value;
};

class JsonParser {
  public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    [[nodiscard]] bool parse(JsonValue& out_value, std::string& out_error) {
        skip_ws();
        if (!parse_value(out_value, out_error)) {
            return false;
        }
        skip_ws();
        if (cursor_ != input_.size()) {
            out_error = "unexpected trailing JSON tokens";
            return false;
        }
        return true;
    }

  private:
    [[nodiscard]] bool parse_value(JsonValue& out_value, std::string& out_error) {
        if (cursor_ >= input_.size()) {
            out_error = "unexpected end of JSON input";
            return false;
        }
        const char c = input_[cursor_];
        if (c == '{') {
            return parse_object(out_value, out_error);
        }
        if (c == '[') {
            return parse_array(out_value, out_error);
        }
        if (c == '"') {
            out_value.type = JsonValue::Type::String;
            return parse_string(out_value.string_value, out_error);
        }
        if (c == 't' || c == 'f') {
            return parse_bool(out_value, out_error);
        }
        if (c == 'n') {
            return parse_null(out_value, out_error);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number(out_value, out_error);
        }
        out_error = "unexpected JSON token";
        return false;
    }

    [[nodiscard]] bool parse_object(JsonValue& out_value, std::string& out_error) {
        out_value = JsonValue{};
        out_value.type = JsonValue::Type::Object;
        ++cursor_; // '{'
        skip_ws();
        if (cursor_ < input_.size() && input_[cursor_] == '}') {
            ++cursor_;
            return true;
        }

        while (cursor_ < input_.size()) {
            skip_ws();
            if (cursor_ >= input_.size() || input_[cursor_] != '"') {
                out_error = "JSON object key must be a string";
                return false;
            }
            std::string key;
            if (!parse_string(key, out_error)) {
                return false;
            }
            skip_ws();
            if (cursor_ >= input_.size() || input_[cursor_] != ':') {
                out_error = "JSON object expected ':' after key";
                return false;
            }
            ++cursor_;
            skip_ws();

            JsonValue value;
            if (!parse_value(value, out_error)) {
                return false;
            }
            out_value.object_value.emplace_back(std::move(key), std::move(value));

            skip_ws();
            if (cursor_ >= input_.size()) {
                out_error = "unexpected end of JSON object";
                return false;
            }
            if (input_[cursor_] == '}') {
                ++cursor_;
                return true;
            }
            if (input_[cursor_] != ',') {
                out_error = "JSON object expected ',' between members";
                return false;
            }
            ++cursor_;
        }
        out_error = "unexpected end of JSON object";
        return false;
    }

    [[nodiscard]] bool parse_array(JsonValue& out_value, std::string& out_error) {
        out_value = JsonValue{};
        out_value.type = JsonValue::Type::Array;
        ++cursor_; // '['
        skip_ws();
        if (cursor_ < input_.size() && input_[cursor_] == ']') {
            ++cursor_;
            return true;
        }

        while (cursor_ < input_.size()) {
            JsonValue item;
            if (!parse_value(item, out_error)) {
                return false;
            }
            out_value.array_value.push_back(std::move(item));

            skip_ws();
            if (cursor_ >= input_.size()) {
                out_error = "unexpected end of JSON array";
                return false;
            }
            if (input_[cursor_] == ']') {
                ++cursor_;
                return true;
            }
            if (input_[cursor_] != ',') {
                out_error = "JSON array expected ',' between elements";
                return false;
            }
            ++cursor_;
            skip_ws();
        }
        out_error = "unexpected end of JSON array";
        return false;
    }

    [[nodiscard]] bool parse_string(std::string& out_string, std::string& out_error) {
        if (cursor_ >= input_.size() || input_[cursor_] != '"') {
            out_error = "JSON string expected opening quote";
            return false;
        }
        ++cursor_; // opening quote
        out_string.clear();
        while (cursor_ < input_.size()) {
            const char c = input_[cursor_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (cursor_ >= input_.size()) {
                    out_error = "unterminated JSON string escape";
                    return false;
                }
                const char e = input_[cursor_++];
                switch (e) {
                case '"':
                case '\\':
                case '/':
                    out_string.push_back(e);
                    break;
                case 'b':
                    out_string.push_back('\b');
                    break;
                case 'f':
                    out_string.push_back('\f');
                    break;
                case 'n':
                    out_string.push_back('\n');
                    break;
                case 'r':
                    out_string.push_back('\r');
                    break;
                case 't':
                    out_string.push_back('\t');
                    break;
                case 'u':
                    out_error = "JSON unicode escape sequences are not supported";
                    return false;
                default:
                    out_error = "invalid JSON string escape sequence";
                    return false;
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                out_error = "invalid control character in JSON string";
                return false;
            }
            out_string.push_back(c);
        }
        out_error = "unterminated JSON string";
        return false;
    }

    [[nodiscard]] bool parse_bool(JsonValue& out_value, std::string& out_error) {
        if (match_keyword("true")) {
            out_value = JsonValue{};
            out_value.type = JsonValue::Type::Bool;
            out_value.bool_value = true;
            return true;
        }
        if (match_keyword("false")) {
            out_value = JsonValue{};
            out_value.type = JsonValue::Type::Bool;
            out_value.bool_value = false;
            return true;
        }
        out_error = "invalid JSON boolean literal";
        return false;
    }

    [[nodiscard]] bool parse_null(JsonValue& out_value, std::string& out_error) {
        if (!match_keyword("null")) {
            out_error = "invalid JSON null literal";
            return false;
        }
        out_value = JsonValue{};
        out_value.type = JsonValue::Type::Null;
        return true;
    }

    [[nodiscard]] bool parse_number(JsonValue& out_value, std::string& out_error) {
        const std::size_t start = cursor_;
        if (input_[cursor_] == '-') {
            ++cursor_;
        }
        if (cursor_ >= input_.size()) {
            out_error = "invalid JSON number";
            return false;
        }
        if (input_[cursor_] == '0') {
            ++cursor_;
        } else if (input_[cursor_] >= '1' && input_[cursor_] <= '9') {
            while (cursor_ < input_.size() && input_[cursor_] >= '0' && input_[cursor_] <= '9') {
                ++cursor_;
            }
        } else {
            out_error = "invalid JSON number";
            return false;
        }
        if (cursor_ < input_.size() && input_[cursor_] == '.') {
            ++cursor_;
            if (cursor_ >= input_.size() || input_[cursor_] < '0' || input_[cursor_] > '9') {
                out_error = "invalid JSON number fractional component";
                return false;
            }
            while (cursor_ < input_.size() && input_[cursor_] >= '0' && input_[cursor_] <= '9') {
                ++cursor_;
            }
        }
        if (cursor_ < input_.size() && (input_[cursor_] == 'e' || input_[cursor_] == 'E')) {
            ++cursor_;
            if (cursor_ < input_.size() && (input_[cursor_] == '+' || input_[cursor_] == '-')) {
                ++cursor_;
            }
            if (cursor_ >= input_.size() || input_[cursor_] < '0' || input_[cursor_] > '9') {
                out_error = "invalid JSON number exponent component";
                return false;
            }
            while (cursor_ < input_.size() && input_[cursor_] >= '0' && input_[cursor_] <= '9') {
                ++cursor_;
            }
        }

        const std::string token(input_.substr(start, cursor_ - start));
        char* end_ptr = nullptr;
        const double parsed_value = std::strtod(token.c_str(), &end_ptr);
        if (end_ptr == nullptr || *end_ptr != '\0' || !std::isfinite(parsed_value)) {
            out_error = "failed parsing JSON number";
            return false;
        }

        out_value = JsonValue{};
        out_value.type = JsonValue::Type::Number;
        out_value.number_value = parsed_value;
        return true;
    }

    void skip_ws() {
        while (cursor_ < input_.size()) {
            const char c = input_[cursor_];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                ++cursor_;
                continue;
            }
            return;
        }
    }

    [[nodiscard]] bool match_keyword(std::string_view keyword) {
        if (input_.substr(cursor_, keyword.size()) != keyword) {
            return false;
        }
        cursor_ += keyword.size();
        return true;
    }

    std::string_view input_;
    std::size_t cursor_ = 0U;
};

const JsonValue* object_find(const JsonValue& object, std::string_view key) {
    if (object.type != JsonValue::Type::Object) {
        return nullptr;
    }
    for (const auto& [entry_key, entry_value] : object.object_value) {
        if (entry_key == key) {
            return &entry_value;
        }
    }
    return nullptr;
}

std::optional<std::string> read_required_string(const JsonValue& object, std::string_view key) {
    const JsonValue* value = object_find(object, key);
    if (value == nullptr || value->type != JsonValue::Type::String || value->string_value.empty()) {
        return std::nullopt;
    }
    return value->string_value;
}

std::optional<double> read_required_number(const JsonValue& object, std::string_view key) {
    const JsonValue* value = object_find(object, key);
    if (value == nullptr || value->type != JsonValue::Type::Number) {
        return std::nullopt;
    }
    if (!std::isfinite(value->number_value)) {
        return std::nullopt;
    }
    return value->number_value;
}

std::optional<bool> read_required_bool(const JsonValue& object, std::string_view key) {
    const JsonValue* value = object_find(object, key);
    if (value == nullptr || value->type != JsonValue::Type::Bool) {
        return std::nullopt;
    }
    return value->bool_value;
}

std::optional<std::uint32_t> read_required_u32(const JsonValue& object, std::string_view key) {
    const std::optional<double> number = read_required_number(object, key);
    if (!number.has_value()) {
        return std::nullopt;
    }
    if (*number < 0.0 || *number > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return std::nullopt;
    }
    const double rounded = std::round(*number);
    if (std::abs(rounded - *number) > 1.0e-6) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(rounded);
}

std::optional<Vec3> read_required_vec3(const JsonValue& object, std::string_view key) {
    const JsonValue* value = object_find(object, key);
    if (value == nullptr || value->type != JsonValue::Type::Array ||
        value->array_value.size() != 3U) {
        return std::nullopt;
    }
    Vec3 out{};
    if (value->array_value[0].type != JsonValue::Type::Number ||
        value->array_value[1].type != JsonValue::Type::Number ||
        value->array_value[2].type != JsonValue::Type::Number) {
        return std::nullopt;
    }
    const double x = value->array_value[0].number_value;
    const double y = value->array_value[1].number_value;
    const double z = value->array_value[2].number_value;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return std::nullopt;
    }
    out.x = static_cast<float>(x);
    out.y = static_cast<float>(y);
    out.z = static_cast<float>(z);
    return out;
}

bool parse_collision_layers(const JsonValue& root, SidecarData& out_data,
                            std::vector<std::string>& warnings) {
    const JsonValue* layers_value = object_find(root, "collision_layers");
    if (layers_value == nullptr || layers_value->type != JsonValue::Type::Array) {
        warnings.push_back("physics sidecar missing valid collision_layers array");
        return false;
    }
    for (std::size_t i = 0U; i < layers_value->array_value.size(); ++i) {
        const JsonValue& layer = layers_value->array_value[i];
        if (layer.type != JsonValue::Type::Object) {
            warnings.push_back("physics sidecar collision_layers entry is not an object");
            continue;
        }
        const std::optional<std::uint32_t> index = read_required_u32(layer, "index");
        const std::optional<std::string> name = read_required_string(layer, "name");
        if (!index.has_value() || !name.has_value()) {
            warnings.push_back("physics sidecar collision_layers entry missing index/name");
            continue;
        }
        if (*index > 31U) {
            warnings.push_back("physics sidecar collision_layers index must be in [0,31]");
            continue;
        }
        out_data.collision_layers.push_back(CollisionLayer{
            .index = *index,
            .name = *name,
        });
    }
    return true;
}

bool parse_constraints(const JsonValue& root, SidecarData& out_data,
                       std::vector<std::string>& warnings) {
    const JsonValue* constraints_value = object_find(root, "constraints");
    if (constraints_value == nullptr || constraints_value->type != JsonValue::Type::Array) {
        warnings.push_back("physics sidecar missing valid constraints array");
        return false;
    }
    std::size_t skipped_unsupported = 0U;
    for (const JsonValue& entry : constraints_value->array_value) {
        if (entry.type != JsonValue::Type::Object) {
            warnings.push_back("physics sidecar constraint entry is not an object");
            continue;
        }
        const std::optional<std::string> id = read_required_string(entry, "id");
        const std::optional<std::string> bone_a_name = read_required_string(entry, "bone_a_name");
        const std::optional<std::string> bone_b_name = read_required_string(entry, "bone_b_name");
        const std::optional<std::string> type = read_required_string(entry, "type");
        if (!id.has_value() || !bone_a_name.has_value() || !bone_b_name.has_value() ||
            !type.has_value()) {
            warnings.push_back("physics sidecar constraint missing required fields");
            continue;
        }

        ConstraintType parsed_type = ConstraintType::Fixed;
        if (*type == "fixed") {
            parsed_type = ConstraintType::Fixed;
        } else if (*type == "hinge") {
            parsed_type = ConstraintType::Hinge;
        } else if (*type == "cone_twist") {
            parsed_type = ConstraintType::ConeTwist;
        } else {
            warnings.push_back(
                "physics sidecar constraint type is unsupported and will be ignored");
            ++skipped_unsupported;
            continue;
        }

        out_data.constraints.push_back(Constraint{
            .id = *id,
            .bone_a_name = *bone_a_name,
            .bone_b_name = *bone_b_name,
            .type = parsed_type,
        });
    }
    if (skipped_unsupported > 0U) {
        VLOG(1) << "PmxPhysicsSidecar: constraints skipped due to unsupported type count="
                << skipped_unsupported;
    }
    return true;
}

bool parse_colliders(const JsonValue& root, std::span<const std::string> joint_names,
                     SidecarData& out_data, std::vector<std::string>& warnings) {
    const JsonValue* colliders_value = object_find(root, "colliders");
    if (colliders_value == nullptr || colliders_value->type != JsonValue::Type::Array) {
        warnings.push_back("physics sidecar missing valid colliders array");
        return false;
    }

    std::unordered_set<std::string> known_joint_names;
    known_joint_names.reserve(joint_names.size());
    for (const std::string& joint_name : joint_names) {
        if (!joint_name.empty()) {
            known_joint_names.insert(joint_name);
        }
    }

    for (const JsonValue& entry : colliders_value->array_value) {
        if (entry.type != JsonValue::Type::Object) {
            warnings.push_back("physics sidecar collider entry is not an object");
            continue;
        }
        const std::optional<std::string> id = read_required_string(entry, "id");
        const std::optional<std::string> bone_name = read_required_string(entry, "bone_name");
        const std::optional<std::string> shape = read_required_string(entry, "shape");
        const std::optional<Vec3> offset = read_required_vec3(entry, "offset");
        const std::optional<Vec3> rotation = read_required_vec3(entry, "rotation_euler_deg");
        const std::optional<bool> is_trigger = read_required_bool(entry, "is_trigger");
        const std::optional<std::uint32_t> layer = read_required_u32(entry, "layer");
        const std::optional<std::uint32_t> mask = read_required_u32(entry, "mask");
        if (!id.has_value() || !bone_name.has_value() || !shape.has_value() ||
            !offset.has_value() || !rotation.has_value() || !is_trigger.has_value() ||
            !layer.has_value() || !mask.has_value()) {
            warnings.push_back("physics sidecar collider missing required fields");
            continue;
        }
        if (!known_joint_names.empty() && !known_joint_names.contains(*bone_name)) {
            warnings.push_back("physics sidecar collider references unknown bone_name");
            continue;
        }

        Collider parsed{};
        parsed.id = *id;
        parsed.bone_name = *bone_name;
        parsed.offset = *offset;
        parsed.rotation_euler_deg = *rotation;
        parsed.is_trigger = *is_trigger;
        parsed.layer = *layer;
        parsed.mask = *mask;

        if (*shape == "sphere") {
            parsed.shape = ColliderShape::Sphere;
            const std::optional<double> radius = read_required_number(entry, "radius");
            if (!radius.has_value() || *radius <= 0.0 || !std::isfinite(*radius)) {
                warnings.push_back("physics sidecar sphere collider missing valid radius");
                continue;
            }
            parsed.radius = static_cast<float>(*radius);
        } else if (*shape == "capsule") {
            parsed.shape = ColliderShape::Capsule;
            const std::optional<double> radius = read_required_number(entry, "radius");
            const std::optional<double> height = read_required_number(entry, "height");
            if (!radius.has_value() || !height.has_value() || *radius <= 0.0 || *height <= 0.0 ||
                !std::isfinite(*radius) || !std::isfinite(*height)) {
                warnings.push_back("physics sidecar capsule collider missing valid radius/height");
                continue;
            }
            parsed.radius = static_cast<float>(*radius);
            parsed.height = static_cast<float>(*height);
        } else if (*shape == "box") {
            parsed.shape = ColliderShape::Box;
            const std::optional<Vec3> size = read_required_vec3(entry, "size");
            if (!size.has_value() || size->x <= 0.0F || size->y <= 0.0F || size->z <= 0.0F) {
                warnings.push_back("physics sidecar box collider missing valid size");
                continue;
            }
            parsed.size = *size;
        } else {
            warnings.push_back("physics sidecar collider shape is unsupported");
            continue;
        }

        out_data.colliders.push_back(std::move(parsed));
    }
    return true;
}

} // namespace

SidecarLoadResult load_from_file(std::string_view sidecar_path,
                                 std::span<const std::string> joint_names) {
    SidecarLoadResult result{};
    std::ifstream stream(std::string(sidecar_path), std::ios::binary);
    if (!stream.is_open()) {
        result.error_message =
            "failed to open physics sidecar file: '" + std::string(sidecar_path) + "'";
        return result;
    }

    std::string json_text((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
    JsonParser parser(json_text);
    JsonValue root;
    std::string parse_error;
    if (!parser.parse(root, parse_error)) {
        result.error_message = "failed to parse physics sidecar JSON: " + parse_error;
        return result;
    }
    if (root.type != JsonValue::Type::Object) {
        result.error_message = "physics sidecar top-level JSON value must be an object";
        return result;
    }

    const std::optional<std::string> schema_version = read_required_string(root, "schema_version");
    if (!schema_version.has_value()) {
        result.error_message = "physics sidecar missing schema_version";
        return result;
    }
    if (*schema_version != kExpectedSchemaVersion) {
        result.error_message = "physics sidecar schema_version is unsupported: got '" +
                               *schema_version + "', expected '" + kExpectedSchemaVersion + "'";
        return result;
    }

    if (object_find(root, "converter") == nullptr ||
        object_find(root, "converter")->type != JsonValue::Type::Object) {
        result.error_message = "physics sidecar converter section missing or invalid";
        return result;
    }

    SidecarData sidecar;
    const bool layers_ok = parse_collision_layers(root, sidecar, result.warnings);
    const bool colliders_ok = parse_colliders(root, joint_names, sidecar, result.warnings);
    const bool constraints_ok = parse_constraints(root, sidecar, result.warnings);
    if (!layers_ok || !colliders_ok || !constraints_ok) {
        result.error_message = "physics sidecar missing required top-level arrays";
        return result;
    }

    result.ok = true;
    result.sidecar = std::move(sidecar);
    VLOG(1) << "PmxPhysicsSidecar: loaded '" << sidecar_path
            << "' colliders=" << result.sidecar.colliders.size()
            << " constraints=" << result.sidecar.constraints.size()
            << " collision_layers=" << result.sidecar.collision_layers.size()
            << " warnings=" << result.warnings.size();
    return result;
}

} // namespace isla::client::pmx_physics_sidecar
