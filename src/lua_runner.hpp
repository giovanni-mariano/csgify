#pragma once

#include <string>

namespace csgify {

int run_lua_script(const std::string& script_path, int argc = 0, char** argv = nullptr);

} // namespace csgify
