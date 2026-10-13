# Third-party code

This directory contains code that is vendored into the repository rather than
fetched at configure time. Vendored code is checked in so that a clean checkout
builds without network access or extra tooling.

| Directory | Library | License | Why it is vendored |
| --- | --- | --- | --- |
| `glad/` | [glad](https://github.com/Dav1dde/glad) OpenGL 3.3 core loader | MIT | Generated once and committed, so the build never needs Python, jinja2, or network access. |
| `stb/` | [stb_image](https://github.com/nothings/stb) | Public domain / MIT | Single header, no build system to integrate. |

Libraries fetched at configure time (GLFW, nlohmann/json, doctest) are pinned in
`cmake/FEDependencies.cmake` and keep their own licenses:

| Library | License |
| --- | --- |
| [GLFW](https://www.glfw.org/license.html) | zlib/libpng |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| [doctest](https://github.com/doctest/doctest) | MIT |

All of the above are permissive and compatible with Fork Engine's MIT license.
A new dependency must be reviewed for license compatibility before it is added;
see [`CONTRIBUTING.md`](../CONTRIBUTING.md).

When updating a vendored library, update the version noted in its subdirectory
README and the corresponding entry here.
