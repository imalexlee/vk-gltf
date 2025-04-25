#extension GL_EXT_buffer_reference: enable
#extension GL_EXT_scalar_block_layout: enable
#extension GL_EXT_nonuniform_qualifier: enable


struct TextureInfo {
    uint index;
    uint tex_coord;
};

struct Material {
    TextureInfo base_color_texture;
    TextureInfo metallic_roughness_texture;
    TextureInfo normal_texture;
    TextureInfo occlusion_texture;
    TextureInfo emissive_texture;

// extension textures
    TextureInfo clearcoat_texture;
    TextureInfo clearcoat_roughness_texture;
    TextureInfo clearcoat_normal_texture;

    vec4 base_color_factors;
    vec3 emissive_factors;
    float metallic_factor;
    float roughness_factor;
    float occlusion_strength;
    float normal_scale;

// extension factors
    float clearcoat_factor;
    float clearcoat_roughness_factor;
};

layout (scalar, set = 0, binding = 0) uniform SceneData {
    mat4 view;
    mat4 proj;
    vec3 eye_pos;
    vec3 sun_dir;
} scene_data;

layout (scalar, set = 1, binding = 0) readonly buffer MaterialBuffer {
    Material materials[];
} material_buf;

layout (set = 1, binding = 1) uniform sampler2D tex_samplers[];

struct Vertex {
    vec4 color;
    vec4 tangent;
    vec3 position;
    vec3 normal;
    vec2 tex_coords[2];
};

layout (scalar, buffer_reference) readonly buffer VertexBuffer {
    Vertex vertices[];
};

layout (push_constant) uniform PushConstants {
    mat4 model_transform;
    VertexBuffer vertex_buffer;
    uint material_index;
} constants;