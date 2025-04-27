#version 450
#extension GL_ARB_shading_language_include: enable
#include "common.glsl"

layout (location = 0) out vec4 vert_position;
layout (location = 1) out vec4 vert_color;
layout (location = 2) out vec4 vert_tangent;
layout (location = 3) out vec3 vert_normal;
layout (location = 4) out vec2 normal_uv;
layout (location = 5) out vec2 color_uv;
layout (location = 6) out vec2 occlusion_uv;
layout (location = 7) out vec2 metal_rough_uv;
layout (location = 8) out vec2 emissive_uv;
layout (location = 9) out vec2 clearcoat_uv;
layout (location = 10) out vec2 clearcoat_rough_uv;
layout (location = 11) out vec2 clearcoat_normal_uv;

const mat4 bias_mat = mat4(
0.5, 0.0, 0.0, 0.0,
0.0, 0.5, 0.0, 0.0,
0.0, 0.0, 1.0, 0.0,
0.5, 0.5, 0.0, 1.0);

void main() {
    Vertex v = constants.vertex_buffer.vertices[gl_VertexIndex];

    vert_position = constants.model_transform * vec4(v.position.xyz, 1.f);

    vert_color = v.color;
    vert_tangent = v.tangent;
    vert_normal = normalize(mat3(constants.model_transform) * v.normal.xyz);


    gl_Position = scene_data.proj * scene_data.view * vert_position;

    Material mat = material_buf.materials[nonuniformEXT(constants.material_index)];

    normal_uv = v.tex_coords[mat.normal_texture.tex_coord];
    color_uv = v.tex_coords[mat.base_color_texture.tex_coord];
    occlusion_uv = v.tex_coords[mat.occlusion_texture.tex_coord];
    metal_rough_uv = v.tex_coords[mat.metallic_roughness_texture.tex_coord];
    emissive_uv = v.tex_coords[mat.emissive_texture.tex_coord];

    clearcoat_uv = v.tex_coords[mat.clearcoat_texture.tex_coord];
    clearcoat_rough_uv = v.tex_coords[mat.clearcoat_roughness_texture.tex_coord];
    clearcoat_normal_uv = v.tex_coords[mat.clearcoat_normal_texture.tex_coord];
}