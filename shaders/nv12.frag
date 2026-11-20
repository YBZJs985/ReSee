#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float dimFactor;
    float isPreview;
    float pad0;
};

layout(binding = 1) uniform sampler2D texY;
layout(binding = 2) uniform sampler2D texUV;

void main()
{
    float y = texture(texY, vTexCoord).r;
    vec2 uv = texture(texUV, vTexCoord).rg;

    // BT.709 Studio Range (16..235 / 16..240) -> RGB
    float yNorm = clamp((y - 0.0627451) * 1.1643835, 0.0, 1.0);
    float u = uv.r - 0.5;
    float v = uv.g - 0.5;

    vec3 rgb = vec3(
        yNorm + 1.792741 * v,
        yNorm - 0.213249 * u - 0.532909 * v,
        yNorm + 2.112402 * u
    );

    rgb = clamp(rgb, 0.0, 1.0) * dimFactor;
    fragColor = vec4(rgb, 1.0) * qt_Opacity;
}
