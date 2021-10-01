#include "GltfPrimitiveBuilder.h"
#include "BitangentAndTangentGenerator.h"
#include "GltfLoadContext.h"

// Window 10 wingdi.h header defines OPAQUE macro which mess up with CesiumGltf::Material::AlphaMode::OPAQUE.
// This only happens with unity build
#include <AzCore/PlatformDef.h>
#ifdef AZ_COMPILER_MSVC
#pragma push_macro("OPAQUE")
#undef OPAQUE
#endif

#include <CesiumGltf/Model.h>
#include <CesiumGltf/MeshPrimitive.h>
#include <CesiumGltf/AccessorView.h>

#ifdef AZ_COMPILER_MSVC
#pragma pop_macro("OPAQUE")
#endif

#include <CesiumUtility/Math.h>
#include <Atom/RPI.Reflect/Model/ModelAsset.h>
#include <Atom/RPI.Reflect/Buffer/BufferAsset.h>
#include <Atom/RPI.Reflect/Model/ModelLodAsset.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelLodAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelAssetCreator.h>
#include <AzCore/Asset/AssetCommon.h>
#include <AzCore/std/limits.h>
#include <cassert>
#include <cstdint>

namespace Cesium
{
    GltfTrianglePrimitiveBuilderOption::GltfTrianglePrimitiveBuilderOption(bool needTangents)
        : m_needTangents{needTangents}
    {
    }

    struct GltfTrianglePrimitiveBuilder::CommonAccessorViews final
    {
        CommonAccessorViews(const CesiumGltf::Model& model, const CesiumGltf::MeshPrimitive& primitive)
            : m_positionAccessor{ nullptr }
        {
            // retrieve required positions first
            auto positionAttribute = primitive.attributes.find("POSITION");
            if (positionAttribute == primitive.attributes.end())
            {
                return;
            }

            m_positionAccessor = model.getSafe<CesiumGltf::Accessor>(&model.accessors, positionAttribute->second);
            if (!m_positionAccessor)
            {
                return;
            }

            m_positions = CesiumGltf::AccessorView<glm::vec3>(model, *m_positionAccessor);
            if (m_positions.status() != CesiumGltf::AccessorViewStatus::Valid)
            {
                return;
            }

            // get normal view
            auto normalAttribute = primitive.attributes.find("NORMAL");
            if (normalAttribute != primitive.attributes.end())
            {
                m_normals = CesiumGltf::AccessorView<glm::vec3>(model, normalAttribute->second);
            }

            // get tangent
            auto tangentAttribute = primitive.attributes.find("TANGENT");
            if (tangentAttribute != primitive.attributes.end())
            {
                m_tangents = CesiumGltf::AccessorView<glm::vec4>(model, tangentAttribute->second);
            }
        }

        const CesiumGltf::Accessor* m_positionAccessor;
        CesiumGltf::AccessorView<glm::vec3> m_positions;
        CesiumGltf::AccessorView<glm::vec3> m_normals;
        CesiumGltf::AccessorView<glm::vec4> m_tangents;
    };

     GltfTrianglePrimitiveBuilder::LoadContext::LoadContext()
        : m_generateFlatNormal{ false }
        , m_generateTangent{ false }
        , m_generateUnIndexedMesh{ false }
    {
    }

    GltfTrianglePrimitiveBuilder::GPUBuffer::GPUBuffer()
        : m_buffer{}
        , m_format{ AZ::RHI::Format::Unknown }
        , m_elementCount{ 0 }
    {
    }

    void GltfTrianglePrimitiveBuilder::Create(
        const CesiumGltf::Model& model,
        const CesiumGltf::MeshPrimitive& primitive,
        const GltfTrianglePrimitiveBuilderOption& option,
        GltfLoadPrimitive& result)
    {
        Reset();

        // Construct common accessor views. This is needed to begin determine loading context
        CommonAccessorViews commonAccessorViews{ model, primitive };
        if (commonAccessorViews.m_positions.status() != CesiumGltf::AccessorViewStatus::Valid)
        {
            return;
        }

        if (commonAccessorViews.m_positions.size() == 0)
        {
            return;
        }

        // construct bounding volume
        auto positionAccessor = commonAccessorViews.m_positionAccessor;
        AZ::Aabb aabb = AZ::Aabb::CreateNull();
        if (positionAccessor->min.size() == 3 && positionAccessor->max.size() == 3)
        {
            aabb = AZ::Aabb::CreateFromMinMaxValues(
                positionAccessor->min[0], positionAccessor->min[1], positionAccessor->min[2], positionAccessor->max[0],
                positionAccessor->max[1], positionAccessor->max[2]);
        }
        else
        {
            aabb = CreateAabbFromPositions(commonAccessorViews.m_positions);
        }

        // set indices
        if (!CreateIndices(model, primitive))
        {
            return;
        }

        // if indices is empty, so the mesh is non-indexed mesh. We should expect positions size
        // is a multiple of 3
        if (m_indices.empty() && (commonAccessorViews.m_positions.size() % 3 == 0))
        {
            return;
        }

        // determine loading context
        DetermineLoadContext(commonAccessorViews, option);

        // Create attributes. The order call of the functions is important
        CreatePositionsAttribute(commonAccessorViews);
        CreateNormalsAttribute(commonAccessorViews);
        CreateUVsAttributes(commonAccessorViews, model, primitive);
        CreateTangentsAndBitangentsAttributes(commonAccessorViews);

        // create buffer assets
        auto indicesBuffer = CreateIndicesBufferAsset(model, primitive);
        auto positionBuffer = CreateBufferAsset(m_positions.data(), m_positions.size(), AZ::RHI::Format::R32G32B32_FLOAT);
        auto normalBuffer = CreateBufferAsset(m_normals.data(), m_normals.size(), AZ::RHI::Format::R32G32B32_FLOAT);
        auto tangentBuffer = CreateBufferAsset(m_tangents.data(), m_tangents.size(), AZ::RHI::Format::R32G32B32A32_FLOAT);
        auto bitangentBuffer = CreateBufferAsset(m_bitangents.data(), m_bitangents.size(), AZ::RHI::Format::R32G32B32_FLOAT);

        AZ::Data::Asset<AZ::RPI::BufferAsset> uvDummyBuffer;
        AZStd::array<AZ::Data::Asset<AZ::RPI::BufferAsset>, 2> uvBuffers;
        for (std::size_t i = 0; i < m_uvs.size(); ++i)
        {
            if (!m_uvs[i].m_buffer.empty())
            {
                uvBuffers[i] = CreateBufferAsset(m_uvs[i].m_buffer.data(), m_uvs[i].m_elementCount, m_uvs[i].m_format);
            }
            else
            {
                // create only one dummy buffer and shared between uv to save GPU memory
                if (!uvDummyBuffer)
                {
                    AZStd::vector<std::uint8_t> uvs(2 * m_positions.size(), 0);
                    uvDummyBuffer = CreateBufferAsset(uvs.data(), uvs.size() / 2, AZ::RHI::Format::R8G8_UNORM);
                }

                uvBuffers[i] = uvDummyBuffer;
            }
        }

        // create LOD asset
        AZ::RPI::ModelLodAssetCreator lodCreator;
        lodCreator.Begin(AZ::Uuid::CreateRandom());
        lodCreator.AddLodStreamBuffer(indicesBuffer);
        lodCreator.AddLodStreamBuffer(positionBuffer);
        lodCreator.AddLodStreamBuffer(normalBuffer);
        lodCreator.AddLodStreamBuffer(tangentBuffer);
        lodCreator.AddLodStreamBuffer(bitangentBuffer);

        for (std::size_t i = 0; i < uvBuffers.size(); ++i)
        {
            if (uvBuffers[i])
            {
                lodCreator.AddLodStreamBuffer(uvBuffers[i]);
            }
        }

        // create mesh
        lodCreator.BeginMesh();
        lodCreator.SetMeshIndexBuffer(AZ::RPI::BufferAssetView(indicesBuffer, indicesBuffer->GetBufferViewDescriptor()));
        lodCreator.AddMeshStreamBuffer(
            AZ::RHI::ShaderSemantic("POSITION"), AZ::Name(),
            AZ::RPI::BufferAssetView(positionBuffer, positionBuffer->GetBufferViewDescriptor()));
        lodCreator.AddMeshStreamBuffer(
            AZ::RHI::ShaderSemantic("NORMAL"), AZ::Name(), AZ::RPI::BufferAssetView(normalBuffer, normalBuffer->GetBufferViewDescriptor()));
        lodCreator.AddMeshStreamBuffer(
            AZ::RHI::ShaderSemantic("TANGENT"), AZ::Name(),
            AZ::RPI::BufferAssetView(tangentBuffer, tangentBuffer->GetBufferViewDescriptor()));
        lodCreator.AddMeshStreamBuffer(
            AZ::RHI::ShaderSemantic("BITANGENT"), AZ::Name(),
            AZ::RPI::BufferAssetView(bitangentBuffer, bitangentBuffer->GetBufferViewDescriptor()));

        for (std::size_t i = 0; i < uvBuffers.size(); ++i)
        {
            if (uvBuffers[i])
            {
                lodCreator.AddMeshStreamBuffer(
                    AZ::RHI::ShaderSemantic("UV", i), AZ::Name(),
                    AZ::RPI::BufferAssetView(uvBuffers[i], uvBuffers[i]->GetBufferViewDescriptor()));
            }
        }

        lodCreator.SetMeshAabb(std::move(aabb));
        lodCreator.EndMesh();

        AZ::Data::Asset<AZ::RPI::ModelLodAsset> lodAsset;
        lodCreator.End(lodAsset);

        // create model asset
        AZ::RPI::ModelAssetCreator modelCreator;
        modelCreator.Begin(AZ::Uuid::CreateRandom());
        modelCreator.AddLodAsset(std::move(lodAsset));

        AZ::Data::Asset<AZ::RPI::ModelAsset> modelAsset;
        modelCreator.End(modelAsset);

        result.m_modelAsset = std::move(modelAsset);
        result.m_materialId = primitive.material;
    }

    void GltfTrianglePrimitiveBuilder::DetermineLoadContext(const CommonAccessorViews& accessorViews, const GltfTrianglePrimitiveBuilderOption& option)
    {
        // check if we should generate normal
        bool isNormalAccessorValid = accessorViews.m_normals.status() == CesiumGltf::AccessorViewStatus::Valid;
        bool hasEnoughNormalVertices = accessorViews.m_normals.size() == accessorViews.m_positions.size();
        m_context.m_generateFlatNormal = !isNormalAccessorValid || !hasEnoughNormalVertices;

        // check if we should generate tangent
        if (option.m_needTangents)
        {
            bool isTangentAccessorValid = accessorViews.m_tangents.status() == CesiumGltf::AccessorViewStatus::Valid;
            bool hasEnoughTangentVertices = accessorViews.m_tangents.size() == accessorViews.m_positions.size();
            m_context.m_generateTangent = !isTangentAccessorValid || !hasEnoughTangentVertices;
        }
        else
        {
            m_context.m_generateTangent = false;
        }

        // check if we should generate unindexed mesh
        m_context.m_generateUnIndexedMesh = m_context.m_generateFlatNormal || m_context.m_generateTangent;
    }

    template<typename AccessorType>
    void GltfTrianglePrimitiveBuilder::CopyAccessorToBuffer(
        const CesiumGltf::AccessorView<AccessorType>& attributeAccessorView, AZStd::vector<AccessorType>& attributes)
    {
        if (m_context.m_generateUnIndexedMesh && m_indices.size() > 0)
        {
            // mesh has indices
            attributes.resize(m_indices.size());
            for (std::size_t i = 0; i < m_indices.size(); ++i)
            {
                std::int64_t index = static_cast<std::int64_t>(m_indices[i]);
                attributes[static_cast<std::size_t>(i)] = attributeAccessorView[index];
            }
        }
        else
        {
            // if m_generateUnIndexedMesh is true but mesh has no indices, that means mesh is already un-indexed. Copy accessor over
            // if m_generateUnIndexedMesh is false, copy accessor over
            attributes.resize(static_cast<std::size_t>(attributeAccessorView.size()));
            for (std::int64_t i = 0; i < attributeAccessorView.size(); ++i)
            {
                attributes[static_cast<std::size_t>(i)] = attributeAccessorView[i];
            }
        }
    }

    template<typename AccessorType>
    void GltfTrianglePrimitiveBuilder::CopyAccessorToBuffer(
        const CesiumGltf::AccessorView<AccessorType>& accessorView, AZStd::vector<std::byte>& buffer)
    {
        if (m_context.m_generateUnIndexedMesh && m_indices.size() > 0)
        {
            buffer.resize(m_indices.size() * sizeof(AccessorType));
            AccessorType* value = reinterpret_cast<AccessorType*>(buffer.data());
            for (std::size_t i = 0; i < m_indices.size(); ++i)
            {
                std::int64_t index = static_cast<std::int64_t>(m_indices[i]);
                value[i] = accessorView[index];
            }
        }
        else
        {
            buffer.resize(static_cast<std::size_t>(accessorView.size()) * sizeof(AccessorType));
            AccessorType* value = reinterpret_cast<AccessorType*>(buffer.data());
            for (std::int64_t i = 0; i < accessorView.size(); ++i)
            {
                value[i] = accessorView[i];
            }
        }
    }

    AZ::Data::Asset<AZ::RPI::BufferAsset> GltfTrianglePrimitiveBuilder::CreateBufferAsset(
        const void* data, const std::size_t elementCount, AZ::RHI::Format format)
    {
        AZ::RHI::BufferViewDescriptor bufferViewDescriptor = AZ::RHI::BufferViewDescriptor::CreateTyped(0, static_cast<std::uint32_t>(elementCount), format);
        AZ::RHI::BufferDescriptor bufferDescriptor;
        bufferDescriptor.m_bindFlags = AZ::RHI::BufferBindFlags::InputAssembly | AZ::RHI::BufferBindFlags::ShaderRead;
        bufferDescriptor.m_byteCount = bufferViewDescriptor.m_elementCount * bufferViewDescriptor.m_elementSize;

        AZ::RPI::BufferAssetCreator creator;
        creator.Begin(AZ::Uuid::CreateRandom());
        creator.SetBuffer(data, bufferDescriptor.m_byteCount, bufferDescriptor);
        creator.SetBufferViewDescriptor(bufferViewDescriptor);
        creator.SetUseCommonPool(AZ::RPI::CommonBufferPoolType::StaticInputAssembly);

        AZ::Data::Asset<AZ::RPI::BufferAsset> bufferAsset;
        creator.End(bufferAsset);

        return bufferAsset;
    }

    bool GltfTrianglePrimitiveBuilder::CreateIndices(const CesiumGltf::Model& model, const CesiumGltf::MeshPrimitive& primitive)
    {
        const CesiumGltf::Accessor* indicesAccessor = model.getSafe<CesiumGltf::Accessor>(&model.accessors, primitive.indices);
        if (!indicesAccessor)
        {
            // it's optional to have no indices
            return true;
        }

        // Gltf primitive says it is an indexed mesh, but if the view of the accessor is invalid, we will terminate the loader right away.
        if (indicesAccessor->type != CesiumGltf::AccessorSpec::Type::SCALAR)
        {
            return false;
        }

        if (indicesAccessor->componentType == CesiumGltf::AccessorSpec::ComponentType::UNSIGNED_BYTE)
        {
            CesiumGltf::AccessorView<std::uint8_t> view{ model, *indicesAccessor };
            return CreateIndices(primitive, view);
        }
        else if (indicesAccessor->componentType == CesiumGltf::AccessorSpec::ComponentType::UNSIGNED_SHORT)
        {
            CesiumGltf::AccessorView<std::uint16_t> view{ model, *indicesAccessor };
            return CreateIndices(primitive, view);
        }
        else if (indicesAccessor->componentType == CesiumGltf::AccessorSpec::ComponentType::UNSIGNED_INT)
        {
            CesiumGltf::AccessorView<std::uint32_t> view{ model, *indicesAccessor };
            return CreateIndices(primitive, view);
        }

        return false;
    }

    template<typename IndexType>
    bool GltfTrianglePrimitiveBuilder::CreateIndices(
        const CesiumGltf::MeshPrimitive& primitive, const CesiumGltf::AccessorView<IndexType>& indicesAccessorView)
    {
        if (indicesAccessorView.status() != CesiumGltf::AccessorViewStatus::Valid)
        {
            return false;
        }

        if (primitive.mode == CesiumGltf::MeshPrimitive::Mode::TRIANGLES)
        {
            if (indicesAccessorView.size() % 3 != 0)
            {
                return false;
            }

            m_indices.resize(static_cast<std::size_t>(indicesAccessorView.size()));
            for (std::int64_t i = 0; i < indicesAccessorView.size(); ++i)
            {
                m_indices[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(indicesAccessorView[i]);
            }

            return true;
        }

        if (primitive.mode == CesiumGltf::MeshPrimitive::Mode::TRIANGLE_STRIP)
        {
            if (indicesAccessorView.size() <= 2)
            {
                return false;
            }

            m_indices.reserve(static_cast<std::size_t>(indicesAccessorView.size() - 2) * 3);
            for (std::int64_t i = 0; i < indicesAccessorView.size() - 2; ++i)
            {
                if (i % 2)
                {
                    m_indices.emplace_back(static_cast<std::uint32_t>(indicesAccessorView[i]));
                    m_indices.emplace_back(static_cast<std::uint32_t>(indicesAccessorView[i + 2]));
                    m_indices.emplace_back(static_cast<std::uint32_t>(indicesAccessorView[i + 1]));
                }
                else
                {
                    m_indices.emplace_back(static_cast<std::uint32_t>(indicesAccessorView[i]));
                    m_indices.emplace_back(static_cast<std::uint32_t>(indicesAccessorView[i + 1]));
                    m_indices.emplace_back(static_cast<std::uint32_t>(indicesAccessorView[i + 2]));
                }
            }

            return true;
        }

        if (primitive.mode == CesiumGltf::MeshPrimitive::Mode::TRIANGLE_FAN)
        {
            if (indicesAccessorView.size() <= 2)
            {
                return false;
            }

            m_indices.reserve(static_cast<std::size_t>(indicesAccessorView.size() - 2) * 3);
            for (std::int64_t i = 0; i < indicesAccessorView.size() - 2; ++i)
            {
                m_indices.emplace_back(static_cast<std::uint32_t>(indicesAccessorView[0]));
                m_indices.emplace_back(static_cast<std::uint32_t>(indicesAccessorView[i + 1]));
                m_indices.emplace_back(static_cast<std::uint32_t>(indicesAccessorView[i + 2]));
            }

            return true;
        }

        return false;
    }

    void GltfTrianglePrimitiveBuilder::CreatePositionsAttribute(const CommonAccessorViews& commonAccessorViews)
    {
        assert(commonAccessorViews.m_positions.status() == CesiumGltf::AccessorViewStatus::Valid);
        assert(commonAccessorViews.m_positions.size() > 0);
        CopyAccessorToBuffer(commonAccessorViews.m_positions, m_positions);
    }

    void GltfTrianglePrimitiveBuilder::CreateNormalsAttribute(const CommonAccessorViews& commonAccessorViews)
    {
        if (m_context.m_generateFlatNormal)
        {
            // if we are at this point, positions is already un-indexed
            assert(m_context.m_generateUnIndexedMesh);
            assert(m_positions.size() > 0);
            assert(m_positions.size() % 3 == 0);
            CreateFlatNormal();
        }
        else
        {
            assert(commonAccessorViews.m_normals.status() == CesiumGltf::AccessorViewStatus::Valid);
            assert(commonAccessorViews.m_normals.size() > 0);
            assert(commonAccessorViews.m_normals.size() == commonAccessorViews.m_positions.size());
            CopyAccessorToBuffer(commonAccessorViews.m_normals, m_normals);
        }
    }

    void GltfTrianglePrimitiveBuilder::CreateUVsAttributes(
        const CommonAccessorViews& commonAccessorViews, const CesiumGltf::Model& model, const CesiumGltf::MeshPrimitive& primitive)
    {
        for (std::size_t i = 0; i < m_uvs.size(); ++i)
        {
            auto uvAttribute = primitive.attributes.find("TEXCOORD_" + std::to_string(i));
            if (uvAttribute == primitive.attributes.end())
            {
                continue;
            }

            const CesiumGltf::Accessor* uvAccessor = model.getSafe<CesiumGltf::Accessor>(&model.accessors, uvAttribute->second);
            if (uvAccessor->type != CesiumGltf::AccessorSpec::Type::VEC2)
            {
                continue;
            }

            if (uvAccessor->componentType == CesiumGltf::AccessorSpec::ComponentType::FLOAT)
            {
                CesiumGltf::AccessorView<glm::vec2> view{ model, *uvAccessor };
                if (view.status() != CesiumGltf::AccessorViewStatus::Valid)
                {
                    continue;
                }

                if (view.size() != commonAccessorViews.m_positions.size())
                {
                    continue;
                }

                CopyAccessorToBuffer(view, m_uvs[i].m_buffer);
                m_uvs[i].m_elementCount = m_uvs[i].m_buffer.size() / (sizeof(float) * 2);
                m_uvs[i].m_format = AZ::RHI::Format::R32G32_FLOAT;
            }
            else if (uvAccessor->componentType == CesiumGltf::AccessorSpec::ComponentType::UNSIGNED_BYTE)
            {
                CesiumGltf::AccessorView<glm::u8vec2> view{ model, *uvAccessor };
                if (view.status() != CesiumGltf::AccessorViewStatus::Valid)
                {
                    continue;
                }

                if (view.size() != commonAccessorViews.m_positions.size())
                {
                    continue;
                }

                CopyAccessorToBuffer(view, m_uvs[i].m_buffer);
                m_uvs[i].m_elementCount = m_uvs[i].m_buffer.size() / (sizeof(std::uint8_t) * 2);
                m_uvs[i].m_format = AZ::RHI::Format::R8G8_UNORM;
            }
            else if (uvAccessor->componentType == CesiumGltf::AccessorSpec::ComponentType::UNSIGNED_SHORT)
            {
                CesiumGltf::AccessorView<glm::u16vec2> view{ model, *uvAccessor };
                if (view.status() != CesiumGltf::AccessorViewStatus::Valid)
                {
                    continue;
                }

                if (view.size() != commonAccessorViews.m_positions.size())
                {
                    continue;
                }

                CopyAccessorToBuffer(view, m_uvs[i].m_buffer);
                m_uvs[i].m_elementCount = m_uvs[i].m_buffer.size() / (sizeof(std::uint16_t) * 2);
                m_uvs[i].m_format = AZ::RHI::Format::R16G16_UNORM;
            }
        }
    }

    void GltfTrianglePrimitiveBuilder::CreateTangentsAndBitangentsAttributes(const CommonAccessorViews& commonAccessorViews)
    {
        if (m_context.m_generateTangent)
        {
            // positions, normals, and uvs should be unindexed at this point
            assert(m_context.m_generateUnIndexedMesh);
            assert(m_positions.size() == m_normals.size());
            assert(m_positions.size() > 0);
            assert(m_positions.size() % 3 == 0);
            assert(m_normals.size() > 0);
            assert(m_normals.size() % 3 == 0);

            // Try to generate tangents and bitangents
            bool success = false;
            for (std::size_t i = 0; i < m_uvs.size(); ++i)
            {
                if (!m_uvs[i].m_buffer.empty())
                {
                    if (m_uvs[i].m_format == AZ::RHI::Format::R32G32_FLOAT)
                    {
                        AZStd::array_view<glm::vec2> uvs(
                            reinterpret_cast<const glm::vec2*>(m_uvs[i].m_buffer.data()), m_uvs[i].m_elementCount);
                        success = BitangentAndTangentGenerator::Generate(m_positions, m_normals, uvs, m_tangents, m_bitangents);
                    }
                    else if (m_uvs[i].m_format == AZ::RHI::Format::R8G8_UNORM)
                    {
                        AZStd::array_view<glm::u8vec2> uvs(
                            reinterpret_cast<const glm::u8vec2*>(m_uvs[i].m_buffer.data()), m_uvs[i].m_elementCount);
                        success = BitangentAndTangentGenerator::Generate(m_positions, m_normals, uvs, m_tangents, m_bitangents);
                    }
                    else if (m_uvs[i].m_format == AZ::RHI::Format::R16G16_UNORM)
                    {
                        AZStd::array_view<glm::u16vec2> uvs(
                            reinterpret_cast<const glm::u16vec2*>(m_uvs[i].m_buffer.data()), m_uvs[i].m_elementCount);
                        success = BitangentAndTangentGenerator::Generate(m_positions, m_normals, uvs, m_tangents, m_bitangents);
                    }
                    else
                    {
                        // This code path should not happen since we check uv accessors before this function
                        assert(false);
                    }
                }

                if (success)
                {
                    break;
                }
            }

            // if we still cannot generate MikkTSpace, then we generate dummy
            if (!success)
            {
                m_tangents.resize(m_positions.size(), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                m_bitangents.resize(m_positions.size(), glm::vec3(0.0f, 0.0f, 0.0f));
            }

            return;
        }

        // check if tangents accessor is valid. If it is, we just copy to the buffer
        const CesiumGltf::AccessorView<glm::vec4>& tangents = commonAccessorViews.m_tangents;
        if ((tangents.status() == CesiumGltf::AccessorViewStatus::Valid) && (tangents.size() > 0) &&
            (tangents.size() == commonAccessorViews.m_positions.size()))
        {
            // copy tangents to vector
            CopyAccessorToBuffer(commonAccessorViews.m_tangents, m_tangents);

            // create bitangents
            m_bitangents.resize(m_tangents.size(), glm::vec3(0.0f));
            for (std::size_t i = 0; i < m_tangents.size(); ++i)
            {
                m_bitangents[i] = glm::cross(m_normals[i], glm::vec3(m_tangents[i])) * m_tangents[i].w;
            }

            return;
        }

        // generate dummy if accessor is not valid
        m_tangents.resize(m_positions.size(), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        m_bitangents.resize(m_positions.size(), glm::vec3(0.0f, 0.0f, 0.0f));
    }

    void GltfTrianglePrimitiveBuilder::CreateFlatNormal()
    {
        m_normals.resize(m_positions.size());
        for (std::size_t i = 0; i < m_positions.size(); i += 3)
        {
            const glm::vec3& p0 = m_positions[i];
            const glm::vec3& p1 = m_positions[i + 1];
            const glm::vec3& p2 = m_positions[i + 2];
            glm::vec3 normal = glm::cross(p1 - p0, p2 - p0);
            if (CesiumUtility::Math::equalsEpsilon(glm::dot(normal, normal), 0.0, CesiumUtility::Math::EPSILON5))
            {
                normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }
            else
            {
                normal = glm::normalize(normal);
            }

            m_normals[i] = normal;
            m_normals[i + 1] = normal;
            m_normals[i + 2] = normal;
        }
    }

    AZ::Data::Asset<AZ::RPI::BufferAsset> GltfTrianglePrimitiveBuilder::CreateIndicesBufferAsset(
        const CesiumGltf::Model& model, const CesiumGltf::MeshPrimitive& primitive)
    {
        if (m_context.m_generateUnIndexedMesh || m_indices.empty())
        {
            // because o3de requires to have indices, we will create generate an array of incremental indices 
            return CreateUnIndexedIndicesBufferAsset();
        }

        // this accessor should be a valid accessor since we check the validity of the indices before parsing the mesh
        const CesiumGltf::Accessor* accessor = model.getSafe<CesiumGltf::Accessor>(&model.accessors, primitive.indices);
        assert(accessor != nullptr);
        assert(accessor->type == CesiumGltf::AccessorSpec::Type::SCALAR);

        if (accessor->componentType == CesiumGltf::AccessorSpec::ComponentType::UNSIGNED_BYTE ||
            accessor->componentType == CesiumGltf::AccessorSpec::ComponentType::UNSIGNED_SHORT)
        {
            // o3de doesn't support uin8_t indices buffer, so we pick the next smallest one which is uint16_t
            AZStd::vector<std::uint16_t> data(m_indices.size());
            for (std::size_t i = 0; i < data.size(); ++i)
            {
                data[i] = static_cast<std::uint16_t>(m_indices[i]);
            }

            return CreateBufferAsset(data.data(), data.size(), AZ::RHI::Format::R16_UINT);
        }
        else if (accessor->componentType == CesiumGltf::AccessorSpec::ComponentType::UNSIGNED_INT)
        {
            AZStd::vector<std::uint32_t> data(m_indices.size());  
            for (std::size_t i = 0; i < data.size(); ++i)
            {
                data[i] = static_cast<std::uint32_t>(m_indices[i]);
            }

            return CreateBufferAsset(data.data(), data.size(), AZ::RHI::Format::R32_UINT);
        }
        else
        {
            // this code path should not run since we check the validity of indices accessor at the very beginning
            assert(false);
            return AZ::Data::Asset<AZ::RPI::BufferAsset>();
        }
    }

    AZ::Data::Asset<AZ::RPI::BufferAsset> GltfTrianglePrimitiveBuilder::CreateUnIndexedIndicesBufferAsset()
    {
        if (m_positions.size() < static_cast<std::size_t>(AZStd::numeric_limits<std::uint16_t>::max()))
        {
            // o3de doesn't support uint8_t indices, so we pick the next smallest one which is uint16_t
            AZStd::vector<std::uint16_t> indices(m_positions.size());
            for (std::size_t i = 0; i < indices.size(); ++i)
            {
                indices[i] = static_cast<std::uint16_t>(i);
            }

            return CreateBufferAsset(indices.data(), indices.size(), AZ::RHI::Format::R16_UINT);
        }
        else
        {
            AZStd::vector<std::uint32_t> indices(m_positions.size());
            for (std::size_t i = 0; i < indices.size(); ++i)
            {
                indices[i] = static_cast<std::uint32_t>(i);
            }

            return CreateBufferAsset(indices.data(), indices.size(), AZ::RHI::Format::R32_UINT);
        }
    }

    void GltfTrianglePrimitiveBuilder::Reset()
    {
        m_context = LoadContext{};
        m_indices.clear();
        m_positions.clear();
        m_normals.clear();
        m_tangents.clear();
        m_bitangents.clear();
        for (std::size_t i = 0; i < m_uvs.size(); ++i)
        {
            m_uvs[i].m_buffer.clear();
            m_uvs[i].m_elementCount = 0;
            m_uvs[i].m_format = AZ::RHI::Format::Unknown;
        }
    }

    AZ::Aabb GltfTrianglePrimitiveBuilder::CreateAabbFromPositions(const CesiumGltf::AccessorView<glm::vec3>& positionAccessorView)
    {
        AZ::Aabb aabb = AZ::Aabb::CreateNull();
        for (std::int64_t i = 0; i < positionAccessorView.size(); ++i)
        {
            const glm::vec3& position = positionAccessorView[i];
            aabb.AddPoint(AZ::Vector3(position.x, position.y, position.z));
        }

        return aabb;
    }
} // namespace Cesium