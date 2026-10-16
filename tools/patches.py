"""Per-instruction replacements applied by gen.py (guest address -> function(gen, addr) returning C code).

Each entry replaces the single guest instruction at that address with hand-written C, for example
to redirect a hard-coded aspect-ratio load to the runtime. The addresses belong to one exact build
of the game's main module, so every patch must check the instruction word it expects before
replacing it (assert on the decoded word) and fail loudly on any other build.

Savage Wind starts with no patches. The Link's Awakening patches this file held in LASRecomp are
kept in the archived copy (reference/LASRecomp/tools/patches.py) as examples of the pattern.
"""

PATCHES = {}
