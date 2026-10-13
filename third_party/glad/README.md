# glad

The OpenGL loader is **vendored** (generated once and committed) so that the
build never needs Python, jinja2, or network access.

- Generated for: **OpenGL 3.3 core**
- Language: C
- License: MIT (see the header of `src/gl.c` and `include/glad/gl.h`)

## Files

- `include/glad/gl.h` — the loader header
- `include/KHR/khrplatform.h` — glad's platform header
- `src/gl.c` — the generated loader

## Regenerating

Regeneration is only needed to change the API version or profile. If you do it:

1. Generate a loader for **OpenGL 3.3 core** with glad, C, and the loader name
   `glad` (`--api gl:core=3.3 --generator c --spec gl`).
2. Replace `include/glad/gl.h`, `include/KHR/khrplatform.h`, and `src/gl.c`.
3. Keep `GLFW_INCLUDE_NONE` set on the engine target so GLFW does not pull in
   its own GL headers (see `engine/CMakeLists.txt`).
4. Rebuild and run the tests.

Note the generated function names: this configuration exports `gladLoadGL` and
the `glad_gl*` function pointers.
