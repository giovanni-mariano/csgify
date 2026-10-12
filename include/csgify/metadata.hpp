#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace csgify {

using StringId = std::uint32_t;
using MetadataId = std::uint32_t;

constexpr StringId kInvalidStringId = 0;
constexpr MetadataId kInvalidMetadataId = 0;

// Per-document UTF-8 string interner. ID 0 is permanently the empty string,
// which makes default-initialized metadata safe to inspect and serialize.
class StringTable {
public:
    StringTable();

    StringId intern(const std::string& value);
    const std::string& lookup(StringId id) const;
    std::size_t size() const { return values_.size(); }
    const std::vector<std::string>& values() const { return values_; }

private:
    std::vector<std::string> values_;
    std::unordered_map<std::string, StringId> ids_;
};

enum class AnnotationOrigin {
    cad,
    user,
    csgify,
    libalea,
    llm,
    imported_recipe,
};

enum class MetadataReviewState {
    informational,
    proposed,
    approved,
    rejected,
    superseded,
};

struct Annotation {
    StringId key = kInvalidStringId;
    StringId value = kInvalidStringId;
    AnnotationOrigin origin = AnnotationOrigin::cad;
    MetadataReviewState review_state = MetadataReviewState::informational;
    bool user_verified = false;
};

struct EntityMetadata {
    MetadataId id = kInvalidMetadataId;
    StringId stable_name = kInvalidStringId;
    StringId display_name = kInvalidStringId;
    StringId description = kInvalidStringId;
    StringId engineering_role = kInvalidStringId;
    StringId subsystem = kInvalidStringId;
    StringId source_cad_label = kInvalidStringId;
    StringId source_component_path = kInvalidStringId;
    std::vector<StringId> tags;
    std::vector<Annotation> annotations;
};

const char* annotation_origin_name(AnnotationOrigin origin);
const char* metadata_review_state_name(MetadataReviewState state);

} // namespace csgify
