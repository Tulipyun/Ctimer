# Third-party notices

Ctimer uses Windows APIs and is built with LLVM/MinGW-w64. Release binaries statically link the C++ runtime and may include supporting runtime code from these projects:

- LLVM, libc++, libc++abi, compiler-rt, and libunwind: Apache License 2.0 with LLVM Exceptions and applicable legacy notices. See [LLVM-LICENSE.txt](licenses/LLVM-LICENSE.txt).
- MinGW-w64 runtime and headers: the applicable licenses and notices are recorded in the MinGW-w64 COPYING files under [licenses](licenses/).
- winpthreads: applicable MIT/BSD-style notices are included in [winpthreads-COPYING.txt](licenses/winpthreads-COPYING.txt).

Windows system DLLs are provided by the operating system and are not distributed with this project. The bundled icon is the project's supplied artwork. These notices do not grant a separate license to Ctimer's own source or artwork.
