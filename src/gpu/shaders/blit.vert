#version 450
// Full-screen triangle for copies (resolves and presentation). The push constants match
// the renderer's pipeline layout (24 bytes): texture index, sampler index, then the source
// rectangle in normalized coordinates.

layout(push_constant) uniform PushConstants
{
    uint textureIndex;
    uint samplerIndex;
    float u0;
    float v0;
    float u1;
    float v1;
} pc;

layout(location = 0) out vec2 outUV;

void main()
{
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    outUV = mix(vec2(pc.u0, pc.v0), vec2(pc.u1, pc.v1), position);
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
