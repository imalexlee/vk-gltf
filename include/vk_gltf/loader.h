#pragma once
#include <cgltf.h>
#include <filesystem>
#include <optional>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vk_mem_alloc.h>

struct Vertex {
    float color[4]{1, 1, 1, 1};
    float tangent[4]{1, 1, 1, 1};
    float position[3]{};
    float normal[3]{};
    float tex_coord[2][2]{};
};

struct GltfTexture {
    std::optional<uint32_t> image_index;
    std::optional<uint32_t> sampler_index;
};

struct TextureInfo {
    uint32_t tex_index{};
    uint32_t tex_coord{};
};

enum class GltfAlphaMode {
    opaque,
    mask,
    blend,
};

struct GltfMaterial {
    std::optional<TextureInfo> base_color_texture{};
    std::optional<TextureInfo> metallic_roughness_texture{};
    std::optional<TextureInfo> normal_texture{};
    std::optional<TextureInfo> occlusion_texture{};
    std::optional<TextureInfo> emissive_texture{};

    // extension textures
    std::optional<TextureInfo> clearcoat_texture{};
    std::optional<TextureInfo> clearcoat_roughness_texture{};
    std::optional<TextureInfo> clearcoat_normal_texture{};

    float base_color_factors[4]{1, 1, 1, 1};
    float emissive_factors[3]{0, 0, 0};
    float metallic_factor{1};
    float roughness_factor{1};
    float occlusion_strength{1};
    float normal_scale{1};

    // extension factors
    float clearcoat_factor{0};
    float clearcoat_roughness_factor{0};

    GltfAlphaMode alpha_mode{};
};

struct GltfImage {
    VkImage           image{};
    VkImageView       image_view{};
    VkFormat          image_format{};
    VkImageLayout     layout{};
    VkExtent3D        extent{};
    VmaAllocation     allocation{};
    VmaAllocationInfo allocation_info{};
};

struct GltfBuffer {
    VkBuffer          buffer{};
    VkDeviceAddress   address{};
    VmaAllocation     allocation{};
    VmaAllocationInfo allocation_info{};
};

struct GltfPrimitive {
    std::optional<GltfBuffer> index_buffer;
    VkIndexType               index_type{VK_INDEX_TYPE_UINT16};
    uint32_t                  index_count{};
    GltfBuffer                vertex_buffer{};
    std::optional<uint32_t>   material;
    VkPrimitiveTopology       topology{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
};

struct GltfMesh {
    // todo: add morph target weights
    std::vector<GltfPrimitive> primitives{};
};

struct GltfNode {
    float                   local_transform[16]{};
    float                   world_transform[16]{};
    std::optional<uint32_t> mesh;
    std::vector<uint32_t>   children{};
};

struct GltfScene {
    std::vector<uint32_t> nodes;
};

struct GltfAsset {
    std::vector<GltfScene>    scenes{};
    std::vector<GltfNode>     nodes{};
    std::vector<GltfMesh>     meshes{};
    std::vector<GltfMaterial> materials{};
    std::vector<GltfTexture>  textures{};
    std::vector<GltfImage>    images{};
    std::vector<VkSampler>    samplers{};
};

struct LoadOptions {
    std::filesystem::path gltf_path{};
    std::filesystem::path cache_dir{};
};

[[nodiscard]] GltfAsset load_gltf(const LoadOptions* load_options, VmaAllocator allocator, VkDevice device, VkCommandPool command_pool, VkQueue queue,
                                  uint32_t queue_family_index);
