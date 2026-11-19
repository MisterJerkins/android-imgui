#version 450

layout(push_constant) uniform Screen {
    vec2 scale;
    vec2 translate;
} screen;

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

void main() {
    gl_Position = vec4(position * screen.scale + screen.translate, 0.0, 1.0);
    out_uv = uv;
    out_color = color;
}
