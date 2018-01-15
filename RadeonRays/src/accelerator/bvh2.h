/**********************************************************************
Copyright (c) 2016 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
********************************************************************/
#pragma once

#include <cassert>
#include <utility>
#include <vector>

#include "../primitive/mesh.h"

namespace RadeonRays
{

    class Bvh2
    {
    public:
        // Constructor
        Bvh2(float traversal_cost, int num_bins = 64, bool usesah = false)
            : m_num_bins(num_bins)
            , m_usesah(usesah)
            , m_traversal_cost(traversal_cost)
            , m_nodes(nullptr)
            , m_nodecount(0)
        {
        }

        // Build function
        template <typename Iter>
        void Build(Iter begin, Iter end);

        void Clear()
        {
            Deallocate(m_nodes);
            m_nodes = nullptr;
            m_nodecount = 0;
        }

    protected:
        struct Node;

        using RefArray = std::vector<std::uint32_t>;
        using MetaDataArray = std::vector<std::pair<const Mesh *, std::size_t> >;

        // Constant values
        enum Constants
        {
            // Invalid index marker
            kInvalidId = 0xffffffffu,
            // Max triangles per leaf
            kMaxLeafPrimitives = 1u,
            // Threshold number of primitives to disable SAH split
            kMinSAHPrimitives = 32u
        };

        // Enum for node type
        enum NodeType
        {
            kLeaf,
            kInternal
        };

        // SAH flag
        bool m_usesah;
        // Node traversal cost
        float m_traversal_cost;
        // Number of spatial bins to use for SAH
        int m_num_bins;

        static void *Allocate(std::size_t size, std::size_t alignment)
        {
#ifdef WIN32
            return _aligned_malloc(size, alignment);
#else // WIN32
            (void)alignment;
            return malloc(size);
#endif // WIN32
        }

        static void Deallocate(void *ptr)
        {
#ifdef WIN32
            _aligned_free(ptr);
#else // WIN32
            free(ptr);
#endif // WIN32
        }

        void BuildImpl(
            __m128 scene_min,
            __m128 scene_max,
            __m128 centroid_scene_min,
            __m128 centroid_scene_max,
            const float3 *aabb_min,
            const float3 *aabb_max,
            const float3 *aabb_centroid,
            const MetaDataArray &metadata,
            std::size_t num_aabbs);

    private:
        Bvh2(const Bvh2 &);
        Bvh2 &operator = (const Bvh2 &);

        // Buffer of encoded nodes
        Node *m_nodes;
        // Number of encoded nodes
        std::size_t m_nodecount;
    };

    // Encoded node format
    struct Bvh2::Node
    {
        // Left AABB min or vertex 0 for a leaf node
        float aabb_left_min_or_v0[3] = { 0.0f, 0.0f, 0.0f };
        // Left child node address
        uint32_t addr_left = kInvalidId;
        // Left AABB max or vertex 1 for a leaf node
        float aabb_left_max_or_v1[3] = { 0.0f, 0.0f, 0.0f };
        // Shape ID for a leaf node
        uint32_t shape_id = kInvalidId;
        // Right AABB min or vertex 2 for a leaf node
        float aabb_right_min_or_v2[3] = { 0.0f, 0.0f, 0.0f };
        // Right child node address
        uint32_t addr_right = kInvalidId;
        // Left AABB max
        float aabb_right_max[3] = { 0.0f, 0.0f, 0.0f };
        // Primitive ID for a leaf node
        uint32_t prim_id = kInvalidId;
    };

    template <typename Iter>
    void Bvh2::Build(Iter begin, Iter end)
    {
        auto num_shapes = std::distance(begin, end);

        assert(num_shapes > 0);

        Clear();

        std::size_t num_items = 0;
        for (auto iter = begin; iter != end; ++iter)
        {
            // TODO: how to deal with quads? create 2 trianles?? (gboisse)
            assert(static_cast<const Mesh *>(*iter)->puretriangle());
            num_items += static_cast<const Mesh *>(*iter)->num_faces();
        }

        auto deleter = [](void *ptr) { Deallocate(ptr); };
        using aligned_float3_ptr = std::unique_ptr<float3 [], decltype(deleter)>;

        auto aabb_min = aligned_float3_ptr(
            reinterpret_cast<float3*>(
                Allocate(sizeof(float3) * num_items, 16u)),
            deleter);

        auto aabb_max = aligned_float3_ptr(
            reinterpret_cast<float3*>(
                Allocate(sizeof(float3) * num_items, 16u)),
            deleter);

        auto aabb_centroid = aligned_float3_ptr(
            reinterpret_cast<float3*>(
                Allocate(sizeof(float3) * num_items, 16u)),
            deleter);

        MetaDataArray metadata(num_items);

        auto constexpr inf = std::numeric_limits<float>::infinity();

        auto scene_min = _mm_set_ps(inf, inf, inf, inf);
        auto scene_max = _mm_set_ps(-inf, -inf, -inf, -inf);
        auto centroid_scene_min = _mm_set_ps(inf, inf, inf, inf);
        auto centroid_scene_max = _mm_set_ps(-inf, -inf, -inf, -inf);

        std::size_t current_face = 0;
        for (auto iter = begin; iter != end; ++iter)
        {
            auto mesh = static_cast<const Mesh *>(*iter);

            for (std::size_t face_index = 0; face_index < mesh->num_faces(); ++face_index, ++current_face)
            {
                auto face = mesh->GetFaceData()[face_index];

                auto load_vertex = [&](int idx)
                {
                    const float3 vertex = mesh->GetVertexData()[idx];
                    return _mm_set_ps(vertex.x, vertex.y, vertex.z, vertex.w);
                };

                auto v0 = load_vertex(face.idx[0]);
                auto v1 = load_vertex(face.idx[1]);
                auto v2 = load_vertex(face.idx[2]);

                auto pmin = _mm_min_ps(_mm_min_ps(v0, v1), v2);
                auto pmax = _mm_max_ps(_mm_max_ps(v0, v1), v2);
                auto centroid = _mm_mul_ps(
                    _mm_add_ps(pmin, pmax),
                    _mm_set_ps(0.5f, 0.5f, 0.5f, 0.5f));

                scene_min = _mm_min_ps(scene_min, pmin);
                scene_max = _mm_max_ps(scene_max, pmax);

                centroid_scene_min = _mm_min_ps(centroid_scene_min, centroid);
                centroid_scene_max = _mm_max_ps(centroid_scene_max, centroid);

                _mm_store_ps(&aabb_min[current_face].x, pmin);
                _mm_store_ps(&aabb_max[current_face].x, pmax);
                _mm_store_ps(&aabb_centroid[current_face].x, centroid);

                metadata[current_face] = std::make_pair(mesh, face_index);
            }
        }

        BuildImpl(
            scene_min,
            scene_max,
            centroid_scene_min,
            centroid_scene_max,
            aabb_min.get(),
            aabb_max.get(),
            aabb_centroid.get(),
            metadata,
            num_items);

        // TODO: finalize/translate?!? (gboisse)
    }
}
