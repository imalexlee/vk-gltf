#include "vk_gltf/loader.h"

#ifndef KHRONOS_STATIC
#define KHRONOS_STATIC
#endif

#include <functional>
#include <ktx.h>
#include <thread>

#include "stb_image.h"
#include <cgltf.h>
#include <iostream>

#include <vulkan/vk_enum_string_helper.h>

namespace vk_gltf {

[[noreturn]] static void abort_message(const std::string_view message) {
    std::cerr << message << std::endl;
    std::abort();
}

#define VK_CHECK(x)                                                                                                                                  \
    do {                                                                                                                                             \
        VkResult err = x;                                                                                                                            \
        if (err) {                                                                                                                                   \
            std::cerr << "Detected Vulkan error: " << string_VkResult(err) << std::endl;                                                             \
            abort();                                                                                                                                 \
        }                                                                                                                                            \
    } while (0)

static void vk_command_immediate_submit(VkDevice device, VkCommandPool command_pool, VkQueue queue,
                                        std::function<void(VkCommandBuffer command_buffer)>&& function) {
    VkFence                 fence{};
    const VkFenceCreateInfo fence_ci = vk_lib::fence_create_info();
    VK_CHECK(vkCreateFence(device, &fence_ci, nullptr, &fence));

    const VkCommandBufferAllocateInfo command_buffer_ai = vk_lib::command_buffer_allocate_info(command_pool);
    VkCommandBuffer                   cmd_buf;
    VK_CHECK(vkAllocateCommandBuffers(device, &command_buffer_ai, &cmd_buf));

    const VkCommandBufferBeginInfo command_buffer_bi = vk_lib::command_buffer_begin_info();
    VK_CHECK(vkBeginCommandBuffer(cmd_buf, &command_buffer_bi));

    function(cmd_buf);

    VK_CHECK(vkEndCommandBuffer(cmd_buf));

    const VkCommandBufferSubmitInfo command_buffer_submit_info = vk_lib::command_buffer_submit_info(cmd_buf);
    const VkSubmitInfo2             submit_info_2              = vk_lib::submit_info_2(&command_buffer_submit_info);

    VK_CHECK(vkQueueSubmit2(queue, 1, &submit_info_2, fence));

    VK_CHECK(vkWaitForFences(device, 1, &fence, true, UINT64_MAX));

    vkFreeCommandBuffers(device, command_pool, 1, &cmd_buf);

    vkDestroyFence(device, fence, nullptr);
}

static std::string cgltf_result_to_string(cgltf_result result) {
    switch (result) {
    case cgltf_result_success:
        return "Success";
    case cgltf_result_data_too_short:
        return "Data too short";
    case cgltf_result_unknown_format:
        return "Unknown format";
    case cgltf_result_invalid_json:
        return "Invalid JSON";
    case cgltf_result_invalid_gltf:
        return "Invalid glTF";
    case cgltf_result_invalid_options:
        return "Invalid options";
    case cgltf_result_file_not_found:
        return "File not found";
    case cgltf_result_io_error:
        return "I/O error";
    case cgltf_result_out_of_memory:
        return "Out of memory";
    case cgltf_result_legacy_gltf:
        return "Legacy glTF";
    case cgltf_result_max_enum:
        return "Invalid enum value";
    default:
        return "Unknown result code";
    }
}

static void get_format_for_image(const cgltf_data* cgltf_data, uint32_t image_index, uint32_t color_channels, VkFormat* uncompressed_vk_format,
                                 VkFormat* ideal_compressed_vk_format, ktx_transcode_fmt_e* ktx_transcode_format) {
    const cgltf_image* target_image = &cgltf_data->images[image_index];
    bool               is_srgb      = true; // default to sRGB, we'll set to false for data textures

    for (uint32_t i = 0; i < cgltf_data->materials_count; i++) {
        const cgltf_material* material = &cgltf_data->materials[i];
        if (material->normal_texture.texture && material->normal_texture.texture->image == target_image) {
            is_srgb = false;
            break;
        }

        if (material->occlusion_texture.texture && material->occlusion_texture.texture->image == target_image) {
            is_srgb = false;
            break;
        }

        if (material->has_pbr_metallic_roughness && material->pbr_metallic_roughness.metallic_roughness_texture.texture &&
            material->pbr_metallic_roughness.metallic_roughness_texture.texture->image == target_image) {
            is_srgb = false;
            break;
        }

        if (material->has_pbr_specular_glossiness && material->pbr_specular_glossiness.specular_glossiness_texture.texture &&
            material->pbr_specular_glossiness.specular_glossiness_texture.texture->image == target_image) {
            is_srgb = false;
            break;
        }

        if (material->has_clearcoat) {
            if (material->clearcoat.clearcoat_roughness_texture.texture &&
                material->clearcoat.clearcoat_roughness_texture.texture->image == target_image) {
                is_srgb = false;
                break;
            }

            if (material->clearcoat.clearcoat_normal_texture.texture && material->clearcoat.clearcoat_normal_texture.texture->image == target_image) {
                is_srgb = false;
                break;
            }
        }

        if (material->has_transmission && material->transmission.transmission_texture.texture &&
            material->transmission.transmission_texture.texture->image == target_image) {
            is_srgb = false;
            break;
        }

        if (material->has_sheen && material->sheen.sheen_roughness_texture.texture &&
            material->sheen.sheen_roughness_texture.texture->image == target_image) {
            is_srgb = false;
            break;
        }
    }

    switch (color_channels) {
    case 1:
        *uncompressed_vk_format     = VK_FORMAT_R8_UNORM;
        *ideal_compressed_vk_format = VK_FORMAT_BC4_UNORM_BLOCK;
        *ktx_transcode_format       = KTX_TTF_BC4_R;
        break;
    case 2:
        *uncompressed_vk_format     = VK_FORMAT_R8G8_UNORM;
        *ideal_compressed_vk_format = VK_FORMAT_BC5_UNORM_BLOCK;
        *ktx_transcode_format       = KTX_TTF_BC5_RG;
        break;
    case 3:
        if (is_srgb) {
            *uncompressed_vk_format     = VK_FORMAT_R8G8B8_SRGB;
            *ideal_compressed_vk_format = VK_FORMAT_BC7_SRGB_BLOCK;
        } else {
            *uncompressed_vk_format     = VK_FORMAT_R8G8B8_UNORM;
            *ideal_compressed_vk_format = VK_FORMAT_BC7_UNORM_BLOCK;
        }
        *ktx_transcode_format = KTX_TTF_BC7_RGBA;
        break;
    case 4:
    default:
        if (is_srgb) {
            *uncompressed_vk_format     = VK_FORMAT_R8G8B8A8_SRGB;
            *ideal_compressed_vk_format = VK_FORMAT_BC7_SRGB_BLOCK;
        } else {
            *uncompressed_vk_format     = VK_FORMAT_R8G8B8A8_UNORM;
            *ideal_compressed_vk_format = VK_FORMAT_BC7_UNORM_BLOCK;
        }
        *ktx_transcode_format = KTX_TTF_BC7_RGBA;
        break;
    }
}

static void allocate_staging_buffer(VmaAllocator allocator, uint64_t data_size, GltfBuffer* staging_buffer) {
    if (staging_buffer->allocation_info.size > 0) {
        vmaDestroyBuffer(allocator, staging_buffer->buffer, staging_buffer->allocation);
    }
    const VkBufferCreateInfo staging_buffer_ci =
        vk_lib::buffer_create_info(VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, data_size, 0);
    VmaAllocationCreateInfo allocation_ci{};
    allocation_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    allocation_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    VK_CHECK(vmaCreateBuffer(allocator, &staging_buffer_ci, &allocation_ci, &staging_buffer->buffer, &staging_buffer->allocation,
                             &staging_buffer->allocation_info));
}

// load gltf images, compress them, then create vulkan images and images views from them
[[nodiscard]] static std::vector<GltfImage> load_gltf_images(const LoadOptions* load_options, const cgltf_data* cgltf_data, VkDevice device,
                                                             VmaAllocator allocator, VkCommandPool command_pool, VkQueue queue,
                                                             GltfBuffer* staging_buffer) {
    bool check_cache    = false;
    bool write_to_cache = false;
    if (!load_options->cache_dir.empty()) {
        if (std::filesystem::exists(load_options->cache_dir)) {
            check_cache    = true;
            write_to_cache = true;
        } else {
            // will write to cache only if caller provided a cache directory and the image was not found in the cache
            write_to_cache = std::filesystem::create_directory(load_options->cache_dir);
        }
    }

    std::vector<GltfImage> gltf_images;
    gltf_images.reserve(cgltf_data->images_count);
    for (uint32_t i = 0; i < cgltf_data->images_count; i++) {
#ifndef NDEBUG
        std::cout << "loading GLTF image: " << std::to_string(i) << std::endl;
#endif

        // 1. pull formats from images
        int width, height, component_count;
        // todo: until i figure out the best way to deal with textures with different num of components in shaders, require 4
        constexpr int required_components = 4;

        const cgltf_buffer_view* buffer_view;
        std::string              uri;

        if (cgltf_data->file_type == cgltf_file_type_glb) {
            buffer_view = cgltf_data->images[i].buffer_view;
            stbi_info_from_memory(static_cast<uint8_t*>(buffer_view->buffer->data) + buffer_view->offset, static_cast<int>(buffer_view->size), &width,
                                  &height, &component_count);
        } else {
            uri = load_options->gltf_path.parent_path().string() + "/" + cgltf_data->images[i].uri;
            stbi_info(uri.c_str(), &width, &height, &component_count);
        }

        ktx_transcode_fmt_e ktx_transcode_format{};
        VkFormat            uncompressed_format{};
        VkFormat            compressed_format{}; // may not need this
        get_format_for_image(cgltf_data, i, required_components, &uncompressed_format, &compressed_format, &ktx_transcode_format);

        ktxTexture2*   ktx_texture = nullptr;
        KTX_error_code result;

        // 2. look into cache for images, if the cache directory exists
        uint32_t mip_levels = 1;
        if (load_options->create_mipmaps) {
            mip_levels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
        }
        // hash is the [gltf asset name]_[image index]_[mip levels].ktx2
        std::string image_hash = load_options->gltf_path.stem().string() + "_" + std::to_string(i) + "_" + std::to_string(mip_levels) + ".ktx2";
        std::string cache_path = load_options->cache_dir.string() + image_hash;

        bool cache_img_exists = false;
        if (check_cache) {
            if (std::filesystem::exists(cache_path)) {
                cache_img_exists = true;
                result           = ktxTexture2_CreateFromNamedFile(cache_path.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx_texture);
                if (result != KTX_SUCCESS) {
                    const std::string message = "Cannot load file with error code: " + std::to_string(result);
                    abort_message(message);
                }
            }
        }

        // 3. if caller didn't provide a cache directory, or if the image was not found there, load and compress the image now
        if (!cache_img_exists) {
            uint8_t* img_data{};
            if (cgltf_data->file_type == cgltf_file_type_glb) {
                img_data = stbi_load_from_memory(static_cast<uint8_t*>(buffer_view->buffer->data) + buffer_view->offset,
                                                 static_cast<int>(buffer_view->size), &width, &height, &component_count, required_components);
            } else {
                img_data = stbi_load(uri.c_str(), &width, &height, &component_count, required_components);
            }

            if (img_data == nullptr) {
                abort_message(stbi_failure_reason());
            }

            ktxTextureCreateInfo create_info{};
            create_info.vkFormat        = uncompressed_format;
            create_info.baseDepth       = 1;
            create_info.baseWidth       = width;
            create_info.baseHeight      = height;
            create_info.numDimensions   = 2;
            create_info.numFaces        = 1;
            create_info.numLayers       = 1;
            create_info.numLevels       = mip_levels;
            create_info.isArray         = false;
            create_info.generateMipmaps = false;
            result                      = ktxTexture2_Create(&create_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktx_texture);
            if (result != KTX_SUCCESS) {
                const std::string message = "Cannot create ktx texture with error code: " + std::to_string(result);
                abort_message(message);
            }

            if (load_options->create_mipmaps) {
                // 1. allocate vulkan image with appropriate mip levels, extents, etx
                // 2. make staging buffer large enough for all levels.
                // 3. fill in staging buffer at offset 0 with raw stb image data
                // 4. maybe free stb image data now
                // 5. buffer->image copy from staging to first mip level of image
                // 6. for each mip level:
                //    6.a blit from last level to the next (start at i = 1);
                //    6.b create buffer to image copy struct
                // 7. perform image->buffer copy to get all blitted images into the staging buffer
                // 8. do ktx setImageFromMemory at the correct staging buffer offsets for all mip levels

                // 1. allocate vulkan image with appropriate mip levels, extents, etx
                VkImageCreateInfo mipmapped_image_ci =
                    vk_lib::image_create_info(VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                              vk_lib::extent_3d(width, height), mip_levels);
                VmaAllocationCreateInfo mipmapped_allocation_ci{};
                mipmapped_allocation_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
                VkImage           mipmapped_image;
                VmaAllocation     mipmapped_image_allocation;
                VmaAllocationInfo mipmapped_image_allocation_info;
                VK_CHECK(vmaCreateImage(allocator, &mipmapped_image_ci, &mipmapped_allocation_ci, &mipmapped_image, &mipmapped_image_allocation,
                                        &mipmapped_image_allocation_info));
                // 2. make staging buffer large enough for all levels.
                uint64_t required_mip_buffer_size = 0;
                {
                    uint32_t mip_width  = width;
                    uint32_t mip_height = height;
                    for (uint32_t level = 0; level < mip_levels; level++) {
                        required_mip_buffer_size += mip_width * mip_height * 4; // 4 color channels. each color channel is a byte
                        if (mip_width > 1) {
                            mip_width /= 2;
                        }
                        if (mip_height > 1) {
                            mip_height /= 2;
                        }
                    }
                }
                if (staging_buffer->allocation_info.size < required_mip_buffer_size) {
                    allocate_staging_buffer(allocator, required_mip_buffer_size, staging_buffer);
                }
                // 3. fill in staging buffer at offset 0 with raw stb image data
                memcpy(staging_buffer->allocation_info.pMappedData, img_data, width * height * 4);
                // 4. maybe free stb image data now
                stbi_image_free(img_data);

                // 5. buffer->image copy from staging to first mip level of image
                vk_command_immediate_submit(device, command_pool, queue, [&](VkCommandBuffer cmd_buf) {
                    // transition all mip levels to DST_OPTIMAL
                    VkImageSubresourceRange     subresource_range   = vk_lib::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, mip_levels);
                    const VkImageMemoryBarrier2 copy_memory_barrier = vk_lib::image_memory_barrier_2(
                        mipmapped_image, subresource_range, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

                    const VkDependencyInfo copy_dependency_info = vk_lib::dependency_info(&copy_memory_barrier, nullptr, nullptr);
                    vkCmdPipelineBarrier2(cmd_buf, &copy_dependency_info);

                    VkImageSubresourceLayers subresource_layers = vk_lib::image_subresource_layers(VK_IMAGE_ASPECT_COLOR_BIT);
                    VkExtent3D               image_extent       = vk_lib::extent_3d(width, height);
                    VkBufferImageCopy        buffer_image_copy  = vk_lib::buffer_image_copy(subresource_layers, image_extent);

                    vkCmdCopyBufferToImage(cmd_buf, staging_buffer->buffer, mipmapped_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                           &buffer_image_copy);
                });

                // 6. for each mip level:
                std::vector<VkBufferImageCopy> buffer_copies{};
                buffer_copies.reserve(mip_levels - 1); // since first image is already in the staging buffer
                vk_command_immediate_submit(device, command_pool, queue, [&](VkCommandBuffer cmd_buf) {
                    uint32_t mip_width     = width;
                    uint32_t mip_height    = height;
                    uint64_t buffer_offset = mip_width * mip_height * 4;
                    for (uint32_t level = 1; level < mip_levels; level++) {
                        VkImageSubresourceRange     subresource_range = vk_lib::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 1, level - 1);
                        const VkImageMemoryBarrier2 convert_old_to_src_optimal_barrier = vk_lib::image_memory_barrier_2(
                            mipmapped_image, subresource_range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

                        const VkDependencyInfo src_level_dependency_info =
                            vk_lib::dependency_info(&convert_old_to_src_optimal_barrier, nullptr, nullptr);

                        vkCmdPipelineBarrier2(cmd_buf, &src_level_dependency_info);

                        // blit
                        VkImageSubresourceLayers blit_src_subresource_layers = vk_lib::image_subresource_layers(VK_IMAGE_ASPECT_COLOR_BIT, level - 1);
                        VkImageSubresourceLayers blit_dst_subresource_layers = vk_lib::image_subresource_layers(VK_IMAGE_ASPECT_COLOR_BIT, level);
                        std::array               src_offsets                 = {vk_lib::offset_3d(), vk_lib::offset_3d(mip_width, mip_height, 1)};
                        std::array               dst_offsets                 = {vk_lib::offset_3d(),
                                                                                vk_lib::offset_3d(mip_width > 1 ? mip_width / 2 : 1, mip_height > 1 ? mip_height / 2 : 1, 1)};
                        VkImageBlit              image_blit =
                            vk_lib::image_blit(blit_src_subresource_layers, blit_dst_subresource_layers, src_offsets, dst_offsets);

                        vkCmdBlitImage(cmd_buf, mipmapped_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mipmapped_image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_blit, VK_FILTER_LINEAR);

                        if (level == mip_levels - 1) {
                            // if the destination mip is the last mip level, it won't enter this loop again after this.
                            // this means we have to transition it to transfer source optimal here
                            subresource_range = vk_lib::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 1, level);
                            const VkImageMemoryBarrier2 convert_last_to_src_optimal_barrier = vk_lib::image_memory_barrier_2(
                                mipmapped_image, subresource_range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

                            const VkDependencyInfo final_level_dependency_info =
                                vk_lib::dependency_info(&convert_last_to_src_optimal_barrier, nullptr, nullptr);

                            vkCmdPipelineBarrier2(cmd_buf, &final_level_dependency_info);
                        }

                        VkImageSubresourceLayers copy_subresource_layers = vk_lib::image_subresource_layers(VK_IMAGE_ASPECT_COLOR_BIT, level);
                        VkExtent3D mip_extent = vk_lib::extent_3d(mip_width > 1 ? mip_width / 2 : 1, mip_height > 1 ? mip_height / 2 : 1);

                        VkBufferImageCopy buffer_image_copy = vk_lib::buffer_image_copy(copy_subresource_layers, mip_extent, buffer_offset);

                        buffer_copies.push_back(buffer_image_copy);

                        if (mip_width > 1) {
                            mip_width /= 2;
                        }
                        if (mip_height > 1) {
                            mip_height /= 2;
                        }

                        buffer_offset += mip_width * mip_height * 4;
                    }

                    // copy all image data into the CPU side staging buffer in order for ktx to compress
                    vkCmdCopyImageToBuffer(cmd_buf, mipmapped_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_buffer->buffer,
                                           buffer_copies.size(), buffer_copies.data());
                });

                uint32_t mip_width     = width;
                uint32_t mip_height    = height;
                uint64_t buffer_offset = 0;
                for (uint32_t level = 0; level < mip_levels; level++) {
                    uint64_t       data_size = mip_width * mip_height * 4;
                    const uint8_t* src_data  = static_cast<uint8_t*>(staging_buffer->allocation_info.pMappedData);

                    result = ktxTexture_SetImageFromMemory(ktxTexture(ktx_texture), level, 0, 0, src_data + buffer_offset, data_size);

                    if (result != KTX_SUCCESS) {
                        const std::string message = "Cannot set ktx image from memory with error code: " + std::to_string(result);
                        abort_message(message);
                    }

                    buffer_offset += data_size;
                    if (mip_width > 1) {
                        mip_width /= 2;
                    }
                    if (mip_height > 1) {
                        mip_height /= 2;
                    }
                }
                vmaDestroyImage(allocator, mipmapped_image, mipmapped_image_allocation);

            } else {
                uint64_t img_data_size = required_components * width * height;
                result                 = ktxTexture_SetImageFromMemory(ktxTexture(ktx_texture), 0, 0, 0, img_data, img_data_size);
                stbi_image_free(img_data);
                if (result != KTX_SUCCESS) {
                    const std::string message = "Cannot set ktx image from memory with error code: " + std::to_string(result);
                    abort_message(message);
                }
            }

            ktxBasisParams params{};
            params.structSize  = sizeof(params);
            params.uastc       = KTX_TRUE;
            params.threadCount = std::max(std::thread::hardware_concurrency(), 1u);

            result = ktxTexture2_CompressBasisEx(ktx_texture, &params);
            if (result != KTX_SUCCESS) {
                const std::string message = "Cannot compress with error code: " + std::to_string(result);
                abort_message(message);
            }
            if (write_to_cache) {
                // 4. write the ktx texture to the cache directory only if the user provided a directory and if the image wasn't found in the cache
                result = ktxTexture2_WriteToNamedFile(ktx_texture, cache_path.c_str());
                if (result != KTX_SUCCESS) {
                    const std::string message = "Cannot write ktx data to file with error code: " + std::to_string(result);
                    abort_message(message);
                }
            }
        }

        // 5. now that I have a ktx texture, transcode it appropriately
        if (ktxTexture2_NeedsTranscoding(ktx_texture)) {
            result = ktxTexture2_TranscodeBasis(ktx_texture, ktx_transcode_format, 0);
            if (result != KTX_SUCCESS) {
                const std::string message = "Cannot transcode with error code: " + std::to_string(result);
                abort_message(message);
            }
        }

        static uint64_t total_texture_bytes_allocated = 0;
        VkExtent3D      base_image_extent             = vk_lib::extent_3d(ktx_texture->baseWidth, ktx_texture->baseHeight);

        total_texture_bytes_allocated += (base_image_extent.height * base_image_extent.width);
        VkImageCreateInfo image_ci =
            vk_lib::image_create_info(static_cast<VkFormat>(ktx_texture->vkFormat), VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                      base_image_extent, ktx_texture->numLevels);
        VmaAllocationCreateInfo texture_allocation_ci{};
        texture_allocation_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        texture_allocation_ci.flags = VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT;

        GltfImage new_texture{};
        new_texture.extent = base_image_extent;
        VK_CHECK(
            vmaCreateImage(allocator, &image_ci, &texture_allocation_ci, &new_texture.image, &new_texture.allocation, &new_texture.allocation_info));

        // create image view
        VkImageSubresourceRange subresource_range = vk_lib::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, ktx_texture->numLevels);
        VkImageViewCreateInfo   image_view_ci =
            vk_lib::image_view_create_info(static_cast<VkFormat>(ktx_texture->vkFormat), new_texture.image, &subresource_range);
        vkCreateImageView(device, &image_view_ci, nullptr, &new_texture.image_view);

        new_texture.image_format = static_cast<VkFormat>(ktx_texture->vkFormat);

        // 6. allocate additional memory in the staging buffer allocation if needed for this texture
        if (staging_buffer->allocation_info.size < ktx_texture->dataSize) {
            allocate_staging_buffer(allocator, ktx_texture->dataSize, staging_buffer);
        }
        // 7. fill staging buffer with all image levels
        memcpy(staging_buffer->allocation_info.pMappedData, ktx_texture->pData, ktx_texture->dataSize);

        // 8. create copy info for each mip level for our staging_buffer -> image copy
        std::vector<VkBufferImageCopy> buffer_image_copies;
        buffer_image_copies.reserve(ktx_texture->numLevels);
        uint32_t mip_width  = width;
        uint32_t mip_height = height;
        for (uint32_t mip_level = 0; mip_level < ktx_texture->numLevels; mip_level++) {
            size_t ktx_offset;
            result = ktxTexture2_GetImageOffset(ktx_texture, mip_level, 0, 0, &ktx_offset);
            if (result != KTX_SUCCESS) {
                const std::string message = "Cannot get offset into ktx image error code: " + std::to_string(result);
                abort_message(message);
            }

            VkImageSubresourceLayers image_subresource = vk_lib::image_subresource_layers(VK_IMAGE_ASPECT_COLOR_BIT, mip_level);
            VkExtent3D               image_extent      = vk_lib::extent_3d(mip_width, mip_height);
            VkBufferImageCopy        buffer_image_copy = vk_lib::buffer_image_copy(image_subresource, image_extent, ktx_offset);

            buffer_image_copies.push_back(buffer_image_copy);
            if (mip_width > 1) {
                mip_width /= 2;
            }
            if (mip_height > 1) {
                mip_height /= 2;
            }
        }

        // 9. run copy commands to upload the staging data to image memory

        vk_command_immediate_submit(device, command_pool, queue, [&](VkCommandBuffer cmd_buf) {
            // TODO: specify more fine grained stage and access flags
            const VkImageMemoryBarrier2 copy_memory_barrier =
                vk_lib::image_memory_barrier_2(new_texture.image, subresource_range, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

            const VkDependencyInfo copy_dependency_info = vk_lib::dependency_info(&copy_memory_barrier, nullptr, nullptr);
            vkCmdPipelineBarrier2(cmd_buf, &copy_dependency_info);

            vkCmdCopyBufferToImage(cmd_buf, staging_buffer->buffer, new_texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   buffer_image_copies.size(), buffer_image_copies.data());

            // TODO: specify more fine grained stage and access flags
            const VkImageMemoryBarrier2 texture_use_memory_barrier = vk_lib::image_memory_barrier_2(
                new_texture.image, subresource_range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            const VkDependencyInfo texture_use_dependency_info = vk_lib::dependency_info(&texture_use_memory_barrier, nullptr, nullptr);
            vkCmdPipelineBarrier2(cmd_buf, &texture_use_dependency_info);
        });

        ktxTexture2_Destroy(ktx_texture);

        new_texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        gltf_images.push_back(new_texture);
    }

    return gltf_images;
}

[[nodiscard]] static std::vector<VkSampler> load_gltf_samplers(const cgltf_data* cgltf_data, VkDevice device) {
    std::vector<VkSampler> samplers;
    samplers.reserve(cgltf_data->samplers_count);

    for (uint32_t i = 0; i < cgltf_data->samplers_count; i++) {
        const cgltf_sampler* cgltf_sampler = &cgltf_data->samplers[i];

        VkSamplerCreateInfo sampler_info = {};
        sampler_info.sType               = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

        switch (cgltf_sampler->mag_filter) {
        case cgltf_filter_type_nearest:
        case cgltf_filter_type_nearest_mipmap_nearest:
        case cgltf_filter_type_nearest_mipmap_linear:
            sampler_info.magFilter = VK_FILTER_NEAREST;
            break;
        default:
            sampler_info.magFilter = VK_FILTER_LINEAR;
            break;
        }

        switch (cgltf_sampler->min_filter) {
        case cgltf_filter_type_nearest:
        case cgltf_filter_type_nearest_mipmap_nearest:
            sampler_info.minFilter  = VK_FILTER_NEAREST;
            sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            break;

        case cgltf_filter_type_linear:
        case cgltf_filter_type_linear_mipmap_nearest:
            sampler_info.minFilter  = VK_FILTER_LINEAR;
            sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            break;

        case cgltf_filter_type_nearest_mipmap_linear:
            sampler_info.minFilter  = VK_FILTER_NEAREST;
            sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            break;

        default:
            sampler_info.minFilter  = VK_FILTER_LINEAR;
            sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            break;
        }

        switch (cgltf_sampler->wrap_s) {
        case cgltf_wrap_mode_clamp_to_edge:
            sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            break;
        case cgltf_wrap_mode_mirrored_repeat:
            sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        default:
            sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            break;
        }

        switch (cgltf_sampler->wrap_t) {
        case cgltf_wrap_mode_clamp_to_edge:
            sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            break;

        case cgltf_wrap_mode_mirrored_repeat:
            sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        default:
            sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            break;
        }

        sampler_info.addressModeW            = sampler_info.addressModeV; // Usually the same as V
        sampler_info.anisotropyEnable        = VK_TRUE;
        sampler_info.maxAnisotropy           = 16.0f;
        sampler_info.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        sampler_info.unnormalizedCoordinates = VK_FALSE;
        sampler_info.compareEnable           = VK_FALSE;
        sampler_info.compareOp               = VK_COMPARE_OP_NEVER;
        sampler_info.mipLodBias              = 0.0f;
        sampler_info.minLod                  = 0.0f;
        sampler_info.maxLod                  = VK_LOD_CLAMP_NONE;

        VkSampler sampler;
        VK_CHECK(vkCreateSampler(device, &sampler_info, nullptr, &sampler));

        samplers.push_back(sampler);
    }

    return samplers;
}

// create meshes along with primitives. allocate vertex and index buffers on gpu
[[nodiscard]] static std::vector<GltfMesh> load_gltf_meshes(const cgltf_data* cgltf_data, VkDevice device, VkQueue queue, VkCommandPool command_pool,
                                                            VmaAllocator allocator, GltfBuffer* staging_buffer) {
    std::vector<GltfMesh> meshes;
    meshes.reserve(cgltf_data->meshes_count);

    for (uint32_t i = 0; i < cgltf_data->meshes_count; i++) {
        const cgltf_mesh* gltf_mesh = &cgltf_data->meshes[i];

        GltfMesh mesh;

        mesh.primitives.reserve(gltf_mesh->primitives_count);
        for (uint32_t j = 0; j < gltf_mesh->primitives_count; j++) {
            const cgltf_primitive* gltf_primitive = &gltf_mesh->primitives[j];

            GltfPrimitive primitive{};
            if (gltf_primitive->material) {
                primitive.material = gltf_primitive->material - cgltf_data->materials;
            }

            switch (gltf_primitive->type) {
            case cgltf_primitive_type_points:
                primitive.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                break;
            case cgltf_primitive_type_lines:
                primitive.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
                break;
            case cgltf_primitive_type_line_strip:
            case cgltf_primitive_type_line_loop:
                primitive.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
                break;
            case cgltf_primitive_type_triangle_strip:
                primitive.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
                break;
            case cgltf_primitive_type_triangle_fan:
                primitive.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
                break;
            default:
                primitive.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            }

            // load indices if present
            if (gltf_primitive->indices) {
                const cgltf_accessor* indices_accessor    = gltf_primitive->indices;
                uint32_t              component_byte_size = 2;
                if (gltf_primitive->indices->component_type == cgltf_component_type_r_32u) {
                    primitive.index_type = VK_INDEX_TYPE_UINT32;
                    component_byte_size  = 4;
                }
                uint32_t total_data_size = component_byte_size * indices_accessor->count;
                // up the size of the staging buffer if needed
                if (staging_buffer->allocation_info.size < total_data_size) {
                    allocate_staging_buffer(allocator, total_data_size, staging_buffer);
                }
                primitive.index_count = indices_accessor->count;
                memcpy(staging_buffer->allocation_info.pMappedData,
                       static_cast<uint8_t*>(indices_accessor->buffer_view->buffer->data) + indices_accessor->offset +
                           indices_accessor->buffer_view->offset,
                       total_data_size);

                VkBufferCopy buffer_copy = vk_lib::buffer_copy(total_data_size);

                // create the actual index buffer on the gpu
                VkBufferCreateInfo indices_buffer_ci =
                    vk_lib::buffer_create_info(VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, total_data_size);
                VmaAllocationCreateInfo allocation_ci{};
                allocation_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
                allocation_ci.flags = VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT;

                GltfBuffer index_buffer{};
                VK_CHECK(vmaCreateBuffer(allocator, &indices_buffer_ci, &allocation_ci, &index_buffer.buffer, &index_buffer.allocation,
                                         &index_buffer.allocation_info));

                primitive.index_buffer = index_buffer;

                vk_command_immediate_submit(device, command_pool, queue, [&](VkCommandBuffer cmd_buf) {
                    vkCmdCopyBuffer(cmd_buf, staging_buffer->buffer, primitive.index_buffer->buffer, 1, &buffer_copy);
                });
            }

            // pre-allocate staging buffer to hold all per-vertex attributes
            size_t attribute_count = 0;
            for (uint64_t k = 0; k < gltf_primitive->attributes_count; k++) {
                attribute_count = std::max(attribute_count, gltf_primitive->attributes[k].data->count);
            }

            uint64_t vertex_data_size = attribute_count * sizeof(Vertex);

            if (staging_buffer->allocation_info.size < vertex_data_size) {
                allocate_staging_buffer(allocator, vertex_data_size, staging_buffer);
            }

            Vertex* vertex_arr = static_cast<Vertex*>(staging_buffer->allocation_info.pMappedData);

            // Indicates if we need to go back and fill in vertex attributes with default data if the accessor for this attribute
            // wasn't found. Don't pre-initialize the vertex_arr since will be a lot of wasted computation many times.
            // currently only worried about colors not being found
            bool colors_found = false;

            // load all per-vertex attributes
            for (uint32_t k = 0; k < gltf_primitive->attributes_count; k++) {
                const cgltf_attribute* gltf_attribute = &gltf_primitive->attributes[k];
                // get vertex positions
                if (gltf_attribute->type == cgltf_attribute_type_position) {
                    const cgltf_accessor* position_accessor = gltf_attribute->data;

                    //  find bounds
                    {
                        float min_pos[3], max_pos[3];
                        memcpy(min_pos, position_accessor->min, 3 * sizeof(float));
                        memcpy(max_pos, position_accessor->max, 3 * sizeof(float));

                        primitive.bounds.origin[0] = (min_pos[0] + max_pos[0]) / 2.f;
                        primitive.bounds.origin[1] = (min_pos[1] + max_pos[1]) / 2.f;
                        primitive.bounds.origin[2] = (min_pos[2] + max_pos[2]) / 2.f;

                        primitive.bounds.extent[0] = (max_pos[0] - min_pos[0]) / 2.f;
                        primitive.bounds.extent[1] = (max_pos[1] - min_pos[1]) / 2.f;
                        primitive.bounds.extent[2] = (max_pos[2] - min_pos[2]) / 2.f;

                        float ex_0 = primitive.bounds.extent[0];
                        float ex_1 = primitive.bounds.extent[1];
                        float ex_2 = primitive.bounds.extent[2];

                        primitive.bounds.sphere_radius = sqrt((ex_0 * ex_0) + (ex_1 * ex_1) + (ex_2 * ex_2));
                    }

                    // for now, assume its always vec3's of 32f's
                    for (uint64_t position_idx = 0; position_idx < position_accessor->count; position_idx++) {
                        const float* gltf_position = reinterpret_cast<const float*>(
                            static_cast<uint8_t*>(position_accessor->buffer_view->buffer->data) + position_accessor->offset +
                            position_accessor->buffer_view->offset + position_idx * position_accessor->stride);

                        memcpy(vertex_arr[position_idx].position, gltf_position, 3 * sizeof(float));
                    }
                }

                // get vertex normals
                if (gltf_attribute->type == cgltf_attribute_type_normal) {
                    const cgltf_accessor* normal_accessor = gltf_attribute->data;
                    for (uint64_t normal_idx = 0; normal_idx < normal_accessor->count; normal_idx++) {
                        const float* gltf_normal = reinterpret_cast<const float*>(static_cast<uint8_t*>(normal_accessor->buffer_view->buffer->data) +
                                                                                  normal_accessor->offset + normal_accessor->buffer_view->offset +
                                                                                  normal_idx * normal_accessor->stride);

                        memcpy(vertex_arr[normal_idx].normal, gltf_normal, 3 * sizeof(float));
                    }
                }
                // get uv's
                if (gltf_attribute->type == cgltf_attribute_type_texcoord) {
                    const cgltf_accessor* uv_accessor = gltf_attribute->data;

                    // only handling 2 texture coordinates per vertex for now
                    uint32_t tex_coord_idx;
                    if (strcmp(gltf_attribute->name, "TEXCOORD_0") == 0) {
                        tex_coord_idx = 0;
                    } else if (strcmp(gltf_attribute->name, "TEXCOORD_1") == 0) {
                        tex_coord_idx = 1;
                    } else {
                        continue;
                        // abort_message("gltf loading does not handle more than 2 texture coordinates per vertex");
                    }

                    for (uint64_t uv_idx = 0; uv_idx < uv_accessor->count; uv_idx++) {
                        const float* gltf_uv =
                            reinterpret_cast<const float*>(static_cast<uint8_t*>(uv_accessor->buffer_view->buffer->data) + uv_accessor->offset +
                                                           uv_accessor->buffer_view->offset + uv_idx * uv_accessor->stride);

                        memcpy(vertex_arr[uv_idx].tex_coord[tex_coord_idx], gltf_uv, 2 * sizeof(float));
                    }
                }

                if (gltf_attribute->type == cgltf_attribute_type_color) {
                    colors_found                         = true;
                    const cgltf_accessor* color_accessor = gltf_attribute->data;
                    for (uint64_t color_idx = 0; color_idx < color_accessor->count; color_idx++) {
                        const float* gltf_color =
                            reinterpret_cast<const float*>(static_cast<uint8_t*>(color_accessor->buffer_view->buffer->data) + color_accessor->offset +
                                                           color_accessor->buffer_view->offset + color_idx * color_accessor->stride);

                        if (color_accessor->type == cgltf_type_vec3) {
                            float color[4] = {gltf_color[0], gltf_color[1], gltf_color[2], 1};
                            memcpy(vertex_arr[color_idx].color, color, 4 * sizeof(float));
                        } else if (color_accessor->type == cgltf_type_vec4) {
                            float color[4] = {gltf_color[0], gltf_color[1], gltf_color[2], gltf_color[4]};
                            memcpy(vertex_arr[color_idx].color, color, 4 * sizeof(float));
                        }
                    }
                }

                // get tangents
                if (gltf_attribute->type == cgltf_attribute_type_tangent) {
                    const cgltf_accessor* tangent_accessor = gltf_attribute->data;
                    for (uint64_t tangent_idx = 0; tangent_idx < tangent_accessor->count; tangent_idx++) {
                        const float* gltf_tangent = reinterpret_cast<const float*>(
                            static_cast<uint8_t*>(tangent_accessor->buffer_view->buffer->data) + tangent_accessor->offset +
                            tangent_accessor->buffer_view->offset + tangent_idx * tangent_accessor->stride);

                        memcpy(vertex_arr[tangent_idx].tangent, gltf_tangent, 4 * sizeof(float));
                    }
                }
            }

            if (!colors_found) {
                float default_color[4] = {1, 1, 1, 1};
                for (uint64_t k = 0; k < attribute_count; k++) {
                    memcpy(vertex_arr[k].color, default_color, 4 * sizeof(float));
                }
            }

            // upload per-vertex attributes to the gpu and get the device address for it

            VkBufferCopy buffer_copy = vk_lib::buffer_copy(vertex_data_size);

            // create the actual index buffer on the gpu
            VkBufferCreateInfo vertex_buffer_ci =
                vk_lib::buffer_create_info(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, vertex_data_size);

            VmaAllocationCreateInfo allocation_ci{};
            allocation_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            allocation_ci.flags = VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT;

            VK_CHECK(vmaCreateBuffer(allocator, &vertex_buffer_ci, &allocation_ci, &primitive.vertex_buffer.buffer,
                                     &primitive.vertex_buffer.allocation, &primitive.vertex_buffer.allocation_info));

            vk_command_immediate_submit(device, command_pool, queue, [&](VkCommandBuffer cmd_buf) {
                vkCmdCopyBuffer(cmd_buf, staging_buffer->buffer, primitive.vertex_buffer.buffer, 1, &buffer_copy);
            });

            VkBufferDeviceAddressInfo device_address_info = vk_lib::buffer_device_address_info(primitive.vertex_buffer.buffer);
            primitive.vertex_buffer.address               = vkGetBufferDeviceAddress(device, &device_address_info);

            mesh.primitives.push_back(primitive);
        }
        meshes.push_back(mesh);
    }

    return meshes;
}

[[nodiscard]] static std::vector<GltfMaterial> load_gltf_materials(const cgltf_data* cgltf_data) {
    std::vector<GltfMaterial> materials;
    materials.reserve(cgltf_data->materials_count);

    for (uint32_t i = 0; i < cgltf_data->materials_count; i++) {
        GltfMaterial material{};

        const cgltf_material* gltf_material = &cgltf_data->materials[i];

        material.alpha_mode = static_cast<GltfAlphaMode>(gltf_material->alpha_mode);

        material.double_sided = gltf_material->double_sided;

        // pbr textures
        if (gltf_material->has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness* metal_rough = &gltf_material->pbr_metallic_roughness;
            memcpy(material.base_color_factors, metal_rough->base_color_factor, 4 * sizeof(float));
            material.metallic_factor  = metal_rough->metallic_factor;
            material.roughness_factor = metal_rough->roughness_factor;

            if (metal_rough->base_color_texture.texture) {
                TextureInfo color_tex_info{};
                color_tex_info.tex_index = metal_rough->base_color_texture.texture - cgltf_data->textures;
                color_tex_info.tex_coord = metal_rough->base_color_texture.texcoord;

                material.base_color_texture = color_tex_info;
            }

            if (metal_rough->metallic_roughness_texture.texture) {
                TextureInfo metal_rough_tex_info{};
                metal_rough_tex_info.tex_index = metal_rough->metallic_roughness_texture.texture - cgltf_data->textures;
                metal_rough_tex_info.tex_coord = metal_rough->metallic_roughness_texture.texcoord;

                material.metallic_roughness_texture = metal_rough_tex_info;
            }
        }

        // normal texture
        if (gltf_material->normal_texture.texture) {
            TextureInfo normal_tex_info{};
            normal_tex_info.tex_index = gltf_material->normal_texture.texture - cgltf_data->textures;
            normal_tex_info.tex_coord = gltf_material->normal_texture.texcoord;

            material.normal_texture = normal_tex_info;
        }
        material.normal_scale = gltf_material->normal_texture.scale;

        // occlusion texture
        if (gltf_material->occlusion_texture.texture) {
            TextureInfo occlusion_tex_info{};
            occlusion_tex_info.tex_index = gltf_material->occlusion_texture.texture - cgltf_data->textures;
            occlusion_tex_info.tex_coord = gltf_material->occlusion_texture.texcoord;

            material.occlusion_texture = occlusion_tex_info;
        }
        material.occlusion_strength = gltf_material->occlusion_texture.scale;

        // emissive texture
        if (gltf_material->emissive_texture.texture) {
            TextureInfo emissive_tex_info{};
            emissive_tex_info.tex_index = gltf_material->emissive_texture.texture - cgltf_data->textures;
            emissive_tex_info.tex_coord = gltf_material->emissive_texture.texcoord;

            material.emissive_texture = emissive_tex_info;
        }
        memcpy(material.emissive_factors, gltf_material->emissive_factor, 3 * sizeof(float));
        // EXTENSIONS

        // clearcoat texture
        if (gltf_material->has_clearcoat) {
            if (gltf_material->clearcoat.clearcoat_texture.texture) {
                TextureInfo clearcoat_tex_info{};
                clearcoat_tex_info.tex_index = gltf_material->clearcoat.clearcoat_texture.texture - cgltf_data->textures;
                clearcoat_tex_info.tex_coord = gltf_material->clearcoat.clearcoat_texture.texcoord;

                material.clearcoat_texture = clearcoat_tex_info;
            }
            if (gltf_material->clearcoat.clearcoat_roughness_texture.texture) {
                TextureInfo clearcoat_rough_tex_info{};
                clearcoat_rough_tex_info.tex_index = gltf_material->clearcoat.clearcoat_roughness_texture.texture - cgltf_data->textures;
                clearcoat_rough_tex_info.tex_coord = gltf_material->clearcoat.clearcoat_roughness_texture.texcoord;

                material.clearcoat_roughness_texture = clearcoat_rough_tex_info;
            }
            if (gltf_material->clearcoat.clearcoat_normal_texture.texture) {
                TextureInfo clearcoat_normal_tex_info{};
                clearcoat_normal_tex_info.tex_index = gltf_material->clearcoat.clearcoat_normal_texture.texture - cgltf_data->textures;
                clearcoat_normal_tex_info.tex_coord = gltf_material->clearcoat.clearcoat_normal_texture.texcoord;

                material.clearcoat_normal_texture = clearcoat_normal_tex_info;
            }
        }
        material.clearcoat_factor           = gltf_material->clearcoat.clearcoat_factor;
        material.clearcoat_roughness_factor = gltf_material->clearcoat.clearcoat_roughness_factor;

        materials.push_back(material);
    }

    return materials;
}

[[nodiscard]] static std::vector<GltfNode> load_gltf_nodes(const cgltf_data* cgltf_data) {
    std::vector<GltfNode> nodes;
    nodes.reserve(cgltf_data->nodes_count);

    for (uint32_t i = 0; i < cgltf_data->nodes_count; i++) {
        GltfNode          node{};
        const cgltf_node* gltf_node = &cgltf_data->nodes[i];

        cgltf_node_transform_local(gltf_node, node.local_transform);
        cgltf_node_transform_world(gltf_node, node.world_transform);

        node.children.reserve(gltf_node->children_count);

        for (uint32_t j = 0; j < gltf_node->children_count; j++) {
            node.children.push_back(gltf_node->children[j] - cgltf_data->nodes);
        }
        if (gltf_node->mesh) {
            node.mesh = gltf_node->mesh - cgltf_data->meshes;
        }

        // EXTENSIONS
        if (gltf_node->light) {
            node.light = gltf_node->light - cgltf_data->lights;
        }

        nodes.push_back(node);
    }

    return nodes;
}

[[nodiscard]] static std::vector<GltfTexture> load_gltf_textures(const cgltf_data* cgltf_data) {

    std::vector<GltfTexture> textures;
    textures.reserve(cgltf_data->textures_count);

    for (uint32_t i = 0; i < cgltf_data->textures_count; i++) {
        GltfTexture texture{};

        const cgltf_texture* gltf_texture = &cgltf_data->textures[i];
        if (gltf_texture->image) {
            texture.image_index = gltf_texture->image - cgltf_data->images;
        }

        if (gltf_texture->sampler) {
            texture.sampler_index = gltf_texture->sampler - cgltf_data->samplers;
        }

        textures.push_back(texture);
    }

    return textures;
}

[[nodiscard]] static std::vector<GltfScene> load_gltf_scenes(const cgltf_data* cgltf_data) {
    std::vector<GltfScene> scenes;
    scenes.reserve(cgltf_data->scenes_count);

    for (uint32_t i = 0; i < cgltf_data->scenes_count; i++) {
        const cgltf_scene* gltf_scene = &cgltf_data->scenes[i];

        GltfScene scene{};
        scene.nodes.reserve(gltf_scene->nodes_count);

        for (uint32_t j = 0; j < gltf_scene->nodes_count; j++) {
            scene.nodes.push_back(gltf_scene->nodes[i] - cgltf_data->nodes);
        }

        scenes.push_back(scene);
    }

    return scenes;
}

[[nodiscard]] static std::vector<GltfLight> load_gltf_lights_ext(const cgltf_data* cgltf_data) {
    std::vector<GltfLight> lights;
    lights.reserve(cgltf_data->lights_count);

    for (uint32_t i = 0; i < cgltf_data->lights_count; i++) {
        const cgltf_light* gltf_light = &cgltf_data->lights[i];

        GltfLight light{};
        light.range            = gltf_light->range;
        light.intensity        = gltf_light->intensity;
        light.inner_cone_angle = gltf_light->spot_inner_cone_angle;
        light.outer_cone_angle = gltf_light->spot_outer_cone_angle;

        memcpy(light.color, gltf_light->color, 3 * sizeof(float));

        light.type = static_cast<GltfLightType>(gltf_light->type - 1);

        lights.push_back(light);
    }

    return lights;
}

GltfAsset load_gltf(const LoadOptions* load_options, VmaAllocator allocator, VkDevice device, VkCommandPool command_pool, VkQueue queue) {
    cgltf_options options{};
    cgltf_data*   gltf_data = nullptr;

    cgltf_result result = cgltf_parse_file(&options, load_options->gltf_path.string().c_str(), &gltf_data);
    if (result != cgltf_result_success) {
        const std::string message = "Cannot parse gltf/glb file with error: " + cgltf_result_to_string(result);
        abort_message(message);
    }

    result = cgltf_load_buffers(&options, gltf_data, load_options->gltf_path.string().c_str());
    if (result != cgltf_result_success) {
        const std::string message = "Cannot parse gltf/glb file with error: " + cgltf_result_to_string(result);
        abort_message(message);
    }

    result = cgltf_validate(gltf_data);
    if (result != cgltf_result_success) {
        const std::string message = "Cannot parse gltf/glb file with error: " + cgltf_result_to_string(result);
        abort_message(message);
    }

    GltfBuffer staging_buffer{};

    GltfAsset gltf_asset{};
    gltf_asset.images    = load_gltf_images(load_options, gltf_data, device, allocator, command_pool, queue, &staging_buffer);
    gltf_asset.meshes    = load_gltf_meshes(gltf_data, device, queue, command_pool, allocator, &staging_buffer);
    gltf_asset.samplers  = load_gltf_samplers(gltf_data, device);
    gltf_asset.materials = load_gltf_materials(gltf_data);
    gltf_asset.textures  = load_gltf_textures(gltf_data);
    gltf_asset.nodes     = load_gltf_nodes(gltf_data);
    gltf_asset.scenes    = load_gltf_scenes(gltf_data);

    // EXTENSIONS
    gltf_asset.lights = load_gltf_lights_ext(gltf_data);

    if (staging_buffer.allocation_info.size > 0) {
        vmaDestroyBuffer(allocator, staging_buffer.buffer, staging_buffer.allocation);
    }
    cgltf_free(gltf_data);

    return gltf_asset;
}

} // namespace vk_gltf