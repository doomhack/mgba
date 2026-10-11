# LLVM Itanium demangler

The Itanium C++ ABI demangler from the LLVM project, used by the Qt frontend's profiler to show C++ function names readably. GCC and Clang both mangle names this way for ARM.

- Source: https://github.com/llvm/llvm-project, tag `llvmorg-19.1.7`
- License: Apache License v2.0 with LLVM Exceptions (`LICENSE.TXT`, also installed as `res/licenses/llvm-demangle.txt`)

Files, unmodified:

| Here | In LLVM |
|---|---|
| `include/llvm/Demangle/Demangle.h` | `llvm/include/llvm/Demangle/Demangle.h` |
| `include/llvm/Demangle/DemangleConfig.h` | `llvm/include/llvm/Demangle/DemangleConfig.h` |
| `include/llvm/Demangle/ItaniumDemangle.h` | `llvm/include/llvm/Demangle/ItaniumDemangle.h` |
| `include/llvm/Demangle/ItaniumNodes.def` | `llvm/include/llvm/Demangle/ItaniumNodes.def` |
| `include/llvm/Demangle/StringViewExtras.h` | `llvm/include/llvm/Demangle/StringViewExtras.h` |
| `include/llvm/Demangle/Utility.h` | `llvm/include/llvm/Demangle/Utility.h` |
| `ItaniumDemangle.cpp` | `llvm/lib/Demangle/ItaniumDemangle.cpp` |
| `LICENSE.TXT` | `llvm/LICENSE.TXT` |

Only `llvm::itaniumDemangle` is used. `Demangle.h` declares LLVM's other demanglers too, but their sources aren't included or built.

To update, copy the same files from a newer LLVM release and update the tag above.

`CMakeLists.txt` turns off inlining for MSVC, which otherwise miscompiles the demangler (seen with MSVC 19.51): some parameter types come out in parentheses, as in `big(int, (int))`. The Qt tests check for this.
