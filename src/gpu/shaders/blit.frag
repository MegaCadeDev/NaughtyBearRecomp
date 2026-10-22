#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(push_constant) uniform PushConstants
{
    uint textureIndex;
    uint samplerIndex;
    float u0;
    float v0;
    float u1;
    float v1;
} pc;

layout(set = 0, binding = 0) uniform texture2D g_Textures[];
layout(set = 3, binding = 0) uniform sampler g_Samplers[];

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(sampler2D(g_Textures[nonuniformEXT(pc.textureIndex)], g_Samplers[pc.samplerIndex]), inUV);
}
