# Kotte — Week 4 starter

Kotte is the C++/raylib teaching project for Scalable Game Worlds in C++.
Open `kotte.slnx` in Visual Studio Community 2026 and build x64 Debug or Release.
The repository includes raylib and cpp_prngs. Move with WASD/arrows; quit with Q/Escape.

## Starting behavior

This starter is based on the `2026-week-04` reference, with three focused tasks
left for students. It preserves Week 3's correct full-scan rendering, camera,
tile culling, continuous player movement, and layer/depth command sorting.

The spatial grid is created and populated once from complete entity bounds.
Storage, rectangle-to-cell mapping, insertion, removal, and diagnostics are
supplied. The project already includes every required source file.

The query deliberately returns no candidates and movement does not yet maintain
grid membership. The camera query initially reports zero visited cells, references,
and unique candidates. Drawing still scans the complete entity vector, so the
image remains correct and exact visibility tests equal total entities.

## Your work

1. `TODO W4.1`, `kotte/src/spatial_grid.cpp`: implement `query()`. Gather indices
   from overlapping cells, count visited cells and raw references, and return
   sorted unique indices. Do not exact-test entities inside the grid.
2. `TODO W4.2`, the same file: implement `update()`. If cell ranges differ,
   remove old membership and insert new membership. The application already
   passes old bounds and final, clamped new bounds after player movement.
3. `TODO W4.3`, `kotte/src/application.cpp`: replace the complete visibility
   scan with iteration over query indices. Preserve the exact test, command
   extraction, sort, and execution.

Complete and check both grid operations before switching the rendering loop.
Remove the corresponding placeholder statements/comments as you finish each task.
Follow the Week 4 practical on Studium for predictions, checkpoints, and evidence.

## Completion

At the same world view, the exact visible entity set should match a complete
scan. Each unique candidate gets one exact visibility test, and each visible
entity produces one command. Raw references may exceed unique candidates because
an entity can occupy several cells. Test movement with small queries at both
old and new locations: the following camera can hide stale membership bugs.

The required endpoint has the same spatial-query behavior as `2026-week-04`.
No collision response, extra gameplay, generic framework, or grid overlay is
required. Keep entity membership and order fixed during play; cell indices do
not provide permanent entity identity.
