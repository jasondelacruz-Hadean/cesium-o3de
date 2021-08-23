#include "MathHelper.h"
#include <CesiumUtility/Math.h>
#include <AzCore/Math/Quaternion.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Cesium
{
    glm::dmat4 MathHelper::ConvertTransformAndScaleToDMat4(const AZ::Transform& transform, const AZ::Vector3& nonUniformScale)
    {
        const AZ::Vector3& o3deTranslation = transform.GetTranslation();
        const AZ::Quaternion& o3deRotation = transform.GetRotation();
        AZ::Vector3 newScale = transform.GetUniformScale() * nonUniformScale;
        glm::dvec3 translation{ static_cast<double>(o3deTranslation.GetX()), static_cast<double>(o3deTranslation.GetY()),
                                static_cast<double>(o3deTranslation.GetZ()) };
        glm::dquat rotation{ static_cast<double>(o3deRotation.GetW()), static_cast<double>(o3deRotation.GetX()),
                             static_cast<double>(o3deRotation.GetY()), static_cast<double>(o3deRotation.GetZ()) };
        glm::dmat4 newTransform = glm::translate(glm::dmat4(1.0), translation);
        newTransform *= glm::dmat4(rotation);
        newTransform = glm::scale(
            newTransform,
            glm::dvec3(static_cast<double>(newScale.GetX()), static_cast<double>(newScale.GetY()), static_cast<double>(newScale.GetZ())));

        return newTransform;
    }

    bool MathHelper::IsIdentityMatrix(const glm::dmat4& mat)
    {
        constexpr glm::dvec4 col0 = glm::dvec4(1.0, 0.0, 0.0, 0.0);
        constexpr glm::dvec4 col1 = glm::dvec4(0.0, 1.0, 0.0, 0.0);
        constexpr glm::dvec4 col2 = glm::dvec4(0.0, 0.0, 1.0, 0.0);
        constexpr glm::dvec4 col3 = glm::dvec4(0.0, 0.0, 0.0, 1.0);

        auto column0 = glm::epsilonEqual(mat[0], col0, CesiumUtility::Math::EPSILON14);
        if (column0 != glm::bvec4(true))
        {
            return false;
        }

        auto column1 = glm::epsilonEqual(mat[1], col1, CesiumUtility::Math::EPSILON14);
        if (column1 != glm::bvec4(true))
        {
            return false;
        }

        auto column2 = glm::epsilonEqual(mat[2], col2, CesiumUtility::Math::EPSILON14);
        if (column2 != glm::bvec4(true))
        {
            return false;
        }

        auto column3 = glm::epsilonEqual(mat[3], col3, CesiumUtility::Math::EPSILON14);
        if (column3 != glm::bvec4(true))
        {
            return false;
        }

        return true;
    }
} // namespace Cesium
