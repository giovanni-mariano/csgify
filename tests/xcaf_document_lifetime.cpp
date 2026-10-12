#include "engine_model.hpp"
#include "step_export.hpp"

#include <XCAFApp_Application.hxx>

#include <cassert>
#include <filesystem>

int main(int argc, char** argv) {
    assert(argc == 3);

    const Handle(XCAFApp_Application) application =
        XCAFApp_Application::GetApplication();
    const int documents_before = application->NbDocuments();

    csgify::CadModel model = csgify::load_step_model(argv[1]);
    assert(!model.root.IsNull());
    assert(application->NbDocuments() == documents_before);

    assert(csgify::export_step(model, argv[2]) == 0);
    assert(std::filesystem::exists(argv[2]));
    assert(application->NbDocuments() == documents_before);
    return 0;
}
