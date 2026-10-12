#include "csgify/project_store.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path()
        / ("csgify-artifact-store-test-" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    csgify::ArtifactStore store(root);
    std::string error;
    assert(store.initialize(&error));

    csgify::ArtifactRecord first;
    assert(store.publish_bytes("report", ".json", "{\"ok\":true}\n",
                               &first, &error));
    assert(!first.digest.empty());
    assert(first.size == 12);
    assert(!first.deduplicated);
    assert(store.validate(first, &error));
    assert(std::filesystem::exists(store.absolute_path(first)));

    csgify::ArtifactRecord duplicate;
    assert(store.publish_bytes("report", "json", "{\"ok\":true}\n",
                               &duplicate, &error));
    assert(duplicate.digest == first.digest);
    assert(duplicate.relative_path == first.relative_path);
    assert(duplicate.deduplicated);

    const std::filesystem::path source = root / "source.brep";
    {
        std::ofstream out(source, std::ios::binary);
        out << "BREP fixture payload";
    }
    csgify::ArtifactRecord copied;
    assert(store.publish_file("brep", "brep", source, &copied, &error));
    assert(store.validate(copied, &error));

    {
        std::ofstream corrupt(store.absolute_path(copied),
                              std::ios::binary | std::ios::app);
        corrupt << "corruption";
    }
    assert(!store.validate(copied, &error));

    // A corrupt object at the content-addressed destination must make a new
    // publication fail closed and must not leave its staging copy behind.
    error.clear();
    csgify::ArtifactRecord rejected;
    assert(!store.publish_file("brep", "brep", source, &rejected, &error));
    assert(error.find("existing content-addressed artifact is corrupt")
           != std::string::npos);
    assert(std::filesystem::is_empty(root / "tmp"));

    csgify::ArtifactRecord unsafe = first;
    unsafe.relative_path = "../outside";
    assert(store.absolute_path(unsafe).empty());

    std::filesystem::remove_all(root, ec);
    return 0;
}
