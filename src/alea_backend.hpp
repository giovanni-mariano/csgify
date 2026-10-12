#pragma once

#include <string>

#include <alea.h>

namespace csgify::detail {

// Finalize a freshly-built Alea system before export. Keeping this operation
// behind one adapter prevents Alea API changes from leaking through the CAD
// conversion code and gives callers one checked failure boundary.
bool finalize_alea_system(alea_system_t* system,
                          bool split_union_cells,
                          std::string* error_out);

} // namespace csgify::detail
