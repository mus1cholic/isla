# Coordinate Systems in Isla

This document outlines the coordinate system conventions used within the `isla` engine and how it interprets data from external assets like glTF models.

## Engine Coordinate System (Left-Handed)
The `isla` engine internally uses a **Left-Handed (LH)** coordinate system for all its math, camera projections, and rendering (via `bgfx`):
*   **+X** is Right
*   **+Y** is Up
*   **+Z** is Forward
*   **Winding Order:** Clockwise (CW) winding is considered the "front" face of a triangle by default in a standard LH system.

## Asset Coordinate System (Right-Handed)
Most 3D authoring tools and modern formats like **glTF / GLB** use a **Right-Handed (RH)** coordinate system:
*   **+Y** is Up
*   **+Z** is Forward
*   **Winding Order:** Counter-Clockwise (CCW) winding is considered the "front" face in glTF.

## Handling the Conversion
Instead of manually mutating every vertex buffer, normal vector, and animation track on load to convert them from RH to LH, `isla` takes an optimized, zero-overhead approach: **We load the RH geometry exactly as-is into our LH engine.**

Wait, what happens when you put a RH model into a LH world?
Because `Mat4::look_at` is explicitly LH +Z-forward and the loader reads POSITION/NORMAL verbatim, the geometry's X-axis is effectively mirrored relative to the camera. When viewing the character from the front, this X-axis mirroring causes the triangles to visually wind backwards on-screen.

To correct this visual inversion without altering the geometry, we do the following:

1.  **Cull Mode Reversal**
    Since the model is mirrored horizontally relative to the camera, the CCW front faces of the glTF model appear to wind Clockwise (CW) on the screen.
    To properly cull the *back* faces, we configure our materials to cull **Counter-Clockwise (CCW)** faces. You'll see this reflected as `MaterialCullMode::CounterClockwise` serving as the default in `render_world.hpp` and `mesh_asset_loader.cpp`.

    *If we mistakenly culled CW faces, the engine would cull the entire front of the mesh, leaving only the hollow inside of the back geometry visible.*

2.  **Depth Writing for Alpha-Blended Materials**
    Hair meshes and other complex organic structures often rely heavily on alpha blending. Models passing through the conversion pipeline into `isla` require strict depth sorting to ensure back-facing transparent geometry (like hair on the back of the head) does not render on top of front-facing transparent geometry.
    To enforce this, we explicitly ensure `BGFX_STATE_WRITE_Z` is enabled for the `kAlphaBlendRenderStateBase` in `model_renderer.cpp`.
