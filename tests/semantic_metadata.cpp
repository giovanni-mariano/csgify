#include "csgify/metadata.hpp"
#include "csgify/material.hpp"

#include <cassert>
#include <string>

int main() {
    csgify::StringTable strings;
    assert(strings.size() == 1);
    assert(strings.lookup(csgify::kInvalidStringId).empty());

    const csgify::StringId pipe = strings.intern("Cooling pipe A");
    const csgify::StringId same_pipe = strings.intern("Cooling pipe A");
    const csgify::StringId unicode = strings.intern("Acqua – circuito primario");
    assert(pipe != csgify::kInvalidStringId);
    assert(pipe == same_pipe);
    assert(strings.lookup(pipe) == "Cooling pipe A");
    assert(strings.lookup(unicode) == "Acqua – circuito primario");
    assert(strings.lookup(999999).empty());

    csgify::EntityMetadata metadata;
    metadata.id = 1;
    metadata.display_name = pipe;
    metadata.annotations.push_back({
        strings.intern("user_comment"),
        strings.intern("Do not simplify this boundary"),
        csgify::AnnotationOrigin::user,
        csgify::MetadataReviewState::approved,
        true,
    });
    metadata.annotations.push_back({
        strings.intern("llm_suggestion"),
        strings.intern("Treat as cooling water host"),
        csgify::AnnotationOrigin::llm,
        csgify::MetadataReviewState::proposed,
        false,
    });

    assert(metadata.annotations.size() == 2);
    assert(metadata.annotations[0].origin == csgify::AnnotationOrigin::user);
    assert(metadata.annotations[0].user_verified);
    assert(metadata.annotations[1].review_state
           == csgify::MetadataReviewState::proposed);
    assert(std::string(csgify::annotation_origin_name(
               csgify::AnnotationOrigin::imported_recipe))
           == "imported_recipe");
    assert(std::string(csgify::material_phase_name(csgify::MaterialPhase::liquid))
           == "liquid");
    return 0;
}
