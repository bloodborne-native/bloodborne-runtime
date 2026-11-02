# The PS4's C and file libraries

[← The PS4 layers](README.md)

The game does not only use its own code. `libc.prx`, the PS4's C library, and `libSceFios2.prx`, its
file streaming library, ship with the game and run as game code.

## What we did

- Both modules are linked into the game's image at start, on the player's device.
- The busiest C library functions are replaced by host code (`src/runtime.c`): `memcpy`, `memset`
  and the mutexes, and the C library's math and string functions the game calls most.
- **Both modules go through the recompiler** (bloodborne-recomp's `tools/recomp/modules.py`), at the
  offsets the game links them to, so they are part of the recompiled library.
- The recompiled library calls those host functions directly, which is where most of the fall in
  translator calls came from.

## Where it stands

4,453 of the 4,759 functions in the two modules run recompiled (94%), 96% of their instructions in C.
It comes with the recompiled library, so it is opt-in: without the library they are translated.

What stays with the translator, on purpose: the import stubs, and code that jumps between stack frames
(`setjmp`, `longjmp`, C++ exception throwing) with its direct callers, because such a jump cannot come
back into a C function that went on.

## What is left

Replacing what stays translated, and a long play session: the tests so far are minutes long.
