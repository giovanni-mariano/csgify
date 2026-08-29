# Vendored Dependencies

This directory contains source dependencies that csgify can build without
depending on system package versions.

## Lua

Lua is expected at:

```text
vendor/lua-5.5.0/
```

The CMake option `CSGIFY_ENABLE_LUA` builds the batch Lua runner from this
vendored tree. It does not use `find_package(Lua)` and does not require Lua to
be installed on the system.

Useful configure command:

```sh
cmake --preset occt8 -DCSGIFY_ENABLE_LUA=ON
```

The Lua interpreter and compiler frontends (`lua.c`, `luac.c`) are not built.
csgify links only the Lua library sources needed by the embedded runner.

## Future Alea

Alea can follow the same rule later: put the source under `vendor/`, add a
small CMake target, and link csgify against that target instead of relying on
a sibling checkout or system install.

