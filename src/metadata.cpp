#include "csgify/metadata.hpp"
#include "csgify/material.hpp"

#include <limits>
#include <stdexcept>

namespace csgify {

StringTable::StringTable() : values_(1), ids_{{std::string{}, kInvalidStringId}} {}

StringId StringTable::intern(const std::string& value) {
    const auto found = ids_.find(value);
    if (found != ids_.end()) {
        return found->second;
    }
    if (values_.size() > static_cast<std::size_t>(
            std::numeric_limits<StringId>::max())) {
        throw std::overflow_error("csgify string table exhausted StringId range");
    }
    const StringId id = static_cast<StringId>(values_.size());
    values_.push_back(value);
    ids_.emplace(values_.back(), id);
    return id;
}

const std::string& StringTable::lookup(StringId id) const {
    if (id >= values_.size()) {
        return values_.front();
    }
    return values_[id];
}

const char* annotation_origin_name(AnnotationOrigin origin) {
    switch (origin) {
    case AnnotationOrigin::cad: return "cad";
    case AnnotationOrigin::user: return "user";
    case AnnotationOrigin::csgify: return "csgify";
    case AnnotationOrigin::libalea: return "libalea";
    case AnnotationOrigin::llm: return "llm";
    case AnnotationOrigin::imported_recipe: return "imported_recipe";
    }
    return "cad";
}

const char* metadata_review_state_name(MetadataReviewState state) {
    switch (state) {
    case MetadataReviewState::informational: return "informational";
    case MetadataReviewState::proposed: return "proposed";
    case MetadataReviewState::approved: return "approved";
    case MetadataReviewState::rejected: return "rejected";
    case MetadataReviewState::superseded: return "superseded";
    }
    return "informational";
}

const char* material_phase_name(MaterialPhase phase) {
    switch (phase) {
    case MaterialPhase::solid: return "solid";
    case MaterialPhase::liquid: return "liquid";
    case MaterialPhase::gas: return "gas";
    case MaterialPhase::vacuum: return "vacuum";
    case MaterialPhase::unknown: return "unknown";
    }
    return "unknown";
}

} // namespace csgify
