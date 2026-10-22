---
id: shader-debug
name: Debug a shader or rendering problem
description: How to isolate a graphics failure when the symptom is a black screen or wrong pixels rather than an error message.
tags: [graphics, shader, rendering, glsl, hlsl, vulkan, opengl]
triggers: [shader, glsl, hlsl, spir-v, black screen, nothing renders, rendering bug, vertex, fragment, pixel shader, texture, vulkan, opengl, directx]
tools: [run_command, read_file, search_text, web_search]
priority: 70
---

# Debug a shader or rendering problem

Rendering fails silently. There is no exception and no stack trace - just wrong
pixels, so the method is bisection down the pipeline.

## When this applies

Nothing appears, the wrong thing appears, or geometry looks corrupted.

## Procedure

1. **Turn on the validation layer first.** Vulkan validation layers, OpenGL
   `KHR_debug`, or the D3D debug layer catch most of these before you start
   guessing. Many "mysterious" bugs are one validation message away from being
   obvious.

2. **Check the shader compiled.** Compile it offline - `glslc`, `glslangValidator`,
   `dxc`, `fxc` - and read the log. A shader that failed to compile often binds
   silently and draws nothing.

3. **Bisect the pipeline, from the end backwards.** At each stage, ask what
   would prove it is working:
   - **Output** - force the fragment shader to output solid red. Still black?
     The problem is before fragment shading: geometry, viewport, or state.
   - **Rasterisation** - if red appears in the wrong place or not at all, check
     the viewport, scissor, winding order and face culling. Backface culling
     with reversed winding is the single most common cause of an invisible mesh.
   - **Geometry** - print or inspect a few transformed vertices. Are they inside
     clip space? A wrong projection matrix puts everything behind the camera.
   - **Depth** - a depth test against an uncleared or wrongly-compared buffer
     hides correct geometry. Try disabling the depth test.
   - **Inputs** - check the vertex attribute layout matches the shader's inputs,
     and that uniforms and descriptors are actually bound.

4. **Check the matrices' conventions.** Row versus column major, and the
   handedness and depth range of the clip space, differ between APIs. A matrix
   that is correct for OpenGL is wrong for Vulkan without a Y flip and a depth
   range adjustment.

5. **Use a frame debugger** when the above does not localise it - RenderDoc,
   PIX or NSight will show the actual draw calls, bound state and buffer
   contents. This is usually faster than any amount of reading.

## Failure modes

- **Editing the shader before confirming it is the shader.** Most "shader bugs"
  are state or binding bugs on the CPU side.
- **Ignoring validation output** because there is a lot of it.
- **Assuming the data is correct.** Inspect the actual buffer contents.
- **Testing several changes at once.** Change one thing, look, change the next.
