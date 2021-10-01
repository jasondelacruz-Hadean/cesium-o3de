#pragma once

#include <AzCore/Component/ComponentBus.h>
#include <AzCore/EBus/Event.h>
#include <glm/glm.hpp>

namespace Cesium
{
    struct CoordinateTransformConfiguration
    {
        CoordinateTransformConfiguration()
            : m_origin{0.0}
            , m_O3DEToECEF{1.0}
            , m_ECEFToO3DE{1.0}
        {
        }

        glm::dvec3 m_origin;
        glm::dmat4 m_O3DEToECEF;
        glm::dmat4 m_ECEFToO3DE;
    };

    using TransformChangeEvent = AZ::Event<const CoordinateTransformConfiguration&>;

    using TransformEnableEvent = AZ::Event<bool, const CoordinateTransformConfiguration&>;

    class CoordinateTransformRequest : public AZ::ComponentBus
    {
    public:
        virtual void SetECEFCoordOrigin(const glm::dvec3& origin) = 0;

        virtual const glm::dvec3& GetECEFCoordOrigin() const = 0;

        virtual const glm::dmat4& O3DEToECEF() const = 0;

        virtual const glm::dmat4& ECEFToO3DE() const = 0;

        virtual glm::dmat4 CalculateO3DEToECEFAtOrigin(const glm::dvec3& origin) const = 0;

        virtual glm::dmat4 CalculateECEFToO3DEAtOrigin(const glm::dvec3& origin) const = 0;

        virtual const CoordinateTransformConfiguration& GetConfiguration() const = 0;

        virtual bool IsEnable() const = 0;

        virtual void BindTransformChangeEventHandler(TransformChangeEvent::Handler& handler) = 0;

        virtual void BindTransformEnableEventHandler(TransformEnableEvent::Handler& handler) = 0;
    };

    using CoordinateTransformRequestBus = AZ::EBus<CoordinateTransformRequest>;
} // namespace Cesium