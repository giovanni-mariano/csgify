#pragma once

#include <NCollection_List.hxx>
#include <TopoDS_Shape.hxx>

namespace csgify::detail {

// Configure a binary Boolean before its only execution. OCCT's two-shape
// convenience constructors execute immediately, before policy can be set.
template <class Operation>
void build_binary_boolean(Operation& operation,
                          const TopoDS_Shape& argument,
                          const TopoDS_Shape& tool,
                          double fuzzy_value = -1.0) {
    NCollection_List<TopoDS_Shape> arguments;
    NCollection_List<TopoDS_Shape> tools;
    arguments.Append(argument);
    tools.Append(tool);
    operation.SetArguments(arguments);
    operation.SetTools(tools);
    operation.SetNonDestructive(true);
    if (fuzzy_value >= 0.0) operation.SetFuzzyValue(fuzzy_value);
    operation.Build();
}

} // namespace csgify::detail
