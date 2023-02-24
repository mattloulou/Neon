#pragma once
#include "Neon/core/core.h"

#include "Neon/set/Backend.h"
#include "Neon/set/MemoryOptions.h"

#include "Neon/Neon.h"
#include "Neon/domain/interface/Stencil.h"
#include "Neon/domain/tools/Geometries.h"
#include "Neon/domain/tools/IODomain.h"

#include <iostream>
#include <sstream>
#include <string>

namespace Neon::domain::tool::testing {

enum class FieldNames
{
    X = 0,
    Y = 1,
    Z = 2,
    W = 3
};

class FieldNamesUtils
{
   public:
    static auto toString(FieldNames name) -> std::string;
    static auto toInt(FieldNames name) -> int;
    static auto fromInt(int id) -> FieldNames;
};

template <typename G, typename T, int C>
class TestData
{
   public:
    using Grid = G;
    using Field = typename G::template Field<T, C>;
    using Type = T;
    static constexpr int Cardinality = C;
    using IODomain = Neon::domain::tool::testing::IODomain<Type, int>;

    TestData(const Neon::Backend&         backend,
             Neon::index_3d               dimension,
             int                          cardinality,
             Neon::MemoryOptions          memoryOptions = Neon::MemoryOptions(),
             Neon::domain::tool::Geometry geometry = Neon::domain::tool::Geometry::FullDomain,
             double                       domainRatio = .8,
             double                       hollowRatio = .5,
             const Neon::domain::Stencil& stencil = Neon::domain::Stencil::s7_Laplace_t(),
             Type                         outsideValue = Type(0));

    auto updateIO() -> void;

    auto updateCompute() -> void;

    auto getField(FieldNames name) -> Field&;

    auto getIODomain(FieldNames name) -> IODomain&;

    auto getBackend() const -> const Neon::Backend&;

    auto getGrid() const -> const Grid&;

    auto getGrid() -> Grid&;

    template <typename LambdaCompare>
    auto compare(FieldNames name, LambdaCompare lambdaCompare)
        -> void;

    auto compare(FieldNames name, T tollerance = T(0.0000001))
        -> bool;

    auto resetValuesToLinear(Type offset,
                             Type offsetBetweenFields = 1)
        -> void;

    auto resetValuesToRandom(int min,
                             int max)
        -> void;

    auto resetValuesToMasked(Type offset,
                             Type offsetBetweenFieds = 1,
                             int  digit = 2)
        -> void;

    auto resetValuesToConst(Type offset,
                            Type offsetBetweenFieds = 1)
        -> void;

    template <typename Lambda, typename FirstType, typename... ExportTypeVariadic_ta>
    auto forEachActiveIODomain(Lambda                                                 userCode,
                               Neon::domain::tool::testing::IODomain<FirstType, int>& first,
                               Neon::domain::tool::testing::IODomain<ExportTypeVariadic_ta, int>&... ioDomains);

    auto dot(IODomain& A, IODomain& B, Type* alpha)
        -> void;

    auto aInvXpY(Type* alpha, IODomain& A, IODomain& B)
        -> void;

    auto axpy(const Type* alpha, IODomain& A, IODomain& B)
        -> void;

    auto laplace(IODomain& A, IODomain& B)
        -> void;

    auto toString() const
        -> std::string;

    auto getGeometry() const
        -> Neon::domain::tool::Geometry;

   private:
    static constexpr int nFields = 4;

    Grid                                     mGrid;
    Field                                    mFields[nFields];
    Neon::domain::tool::testing::IODomain<T> mIODomains[nFields];
    Neon::domain::tool::Geometry             mGeometry;
};

template <typename G, typename T, int C>
TestData<G, T, C>::TestData(const Neon::Backend&         backend,
                            Neon::index_3d               dimension,
                            int                          cardinality,
                            Neon::MemoryOptions          memoryOptions,
                            Neon::domain::tool::Geometry geometry,
                            double                       domainRatio,
                            double                       hollowRatio,
                            const domain::Stencil&       stencil,
                            Type                         outsideValue)
{
    Neon::init();

    mGeometry = geometry;
    Neon::domain::tool::GeometryMask geometryMask(geometry,
                                                  dimension,
                                                  domainRatio,
                                                  hollowRatio);

    const IODense<uint8_t, int> maskDense = geometryMask.getIODenseMask();

    mGrid = Grid(
        backend, dimension, [&](const Neon::index_3d& idx) {
            return geometryMask(idx);
        },
        stencil);

    for (int i = 0; i < nFields; i++) {
        auto fieldName = FieldNamesUtils::fromInt(i);
        auto fieldStrig = FieldNamesUtils::toString(fieldName);

        mFields[i] = mGrid.template newField<Type, Cardinality>(fieldStrig,
                                                                cardinality,
                                                                outsideValue,
                                                                Neon::DataUse::IO_COMPUTE,
                                                                memoryOptions);

        mIODomains[i] = Neon::domain::tool::testing::IODomain<T>(dimension,
                                                                 cardinality,
                                                                 maskDense,
                                                                 outsideValue);
    }
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::getField(FieldNames name)
    -> Field&
{
    auto fieldIdx = FieldNamesUtils::toInt(name);
    return mFields[fieldIdx];
}
template <typename G, typename T, int C>
auto TestData<G, T, C>::getIODomain(FieldNames name)
    -> TestData::IODomain&
{
    auto i = FieldNamesUtils::toInt(name);
    return mIODomains[i];
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::getBackend() const -> const Neon::Backend&
{
    return mGrid.getBackend();
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::getGrid() const -> const Grid&
{
    return mGrid;
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::getGrid() -> Grid&
{
    return mGrid;
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::resetValuesToLinear(Type offset, Type offsetBetweenFields)
    -> void
{
    for (int i = 0; i < nFields; i++) {
        auto fieldName = FieldNamesUtils::fromInt(i);
        auto fieldStrig = FieldNamesUtils::toString(fieldName);

        mIODomains[i].resetValuesToLinear(offset + i * offsetBetweenFields);
        mFields[i].ioFromDense(mIODomains[i].getData());
        mFields[i].updateDeviceData(0);
    }
    mGrid.getBackend().sync(0);
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::resetValuesToRandom(int min, int max) -> void
{
    for (int i = 0; i < nFields; i++) {
        auto fieldName = FieldNamesUtils::fromInt(i);
        auto fieldStrig = FieldNamesUtils::toString(fieldName);

        mIODomains[i].resetValuesToRandom(min, max);
        mFields[i].ioFromDense(mIODomains[i].getData());
        mFields[i].updateCompute(0);
    }
    mGrid.getBackend().sync(0);
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::resetValuesToMasked(Type offset, Type offsetBetweenFields, int digit)
    -> void
{
    for (int i = 0; i < nFields; i++) {
        auto fieldName = FieldNamesUtils::fromInt(i);
        auto fieldStrig = FieldNamesUtils::toString(fieldName);

        mIODomains[i].resetValuesToMasked(offset + i * offsetBetweenFields, digit);
        mFields[i].ioFromDense(mIODomains[i].getData());
        mFields[i].updateCompute(0);
    }
    mGrid.getBackend().sync(0);
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::resetValuesToConst(Type offset, Type offsetBetweenFields)
    -> void
{
    for (int i = 0; i < nFields; i++) {
        auto fieldName = FieldNamesUtils::fromInt(i);
        auto fieldStrig = FieldNamesUtils::toString(fieldName);

        mIODomains[i].resetValuesToConst(offset + i * offsetBetweenFields);
        mFields[i].ioFromDense(mIODomains[i].getData());
        mFields[i].updateCompute(0);
    }
    mGrid.getBackend().sync(0);
}
template <typename G, typename T, int C>
template <typename LambdaCompare>
auto TestData<G, T, C>::compare(FieldNames    name,
                                LambdaCompare lambdaCompare)
    -> void
{
    auto idx = FieldNamesUtils::toInt(name);
    mFields[idx].updateHostData(0);
    mGrid.getBackend().sync(0);

    auto                                     tmpDense = mFields[idx].template ioToDense<Type>();
    Neon::domain::tool::testing::IODomain<T> tmpIODomain(tmpDense, mIODomains[idx].getMask(), mIODomains[idx].getOutsideValue());

    mIODomains[idx].forEachActive([&](const Neon::index_3d&                                          idx,
                                      int                                                            cardinality,
                                      const typename Neon::domain::tool::testing::IODomain<T>::Type& goldenVal,
                                      const typename Neon::domain::tool::testing::IODomain<T>::Type& testVal) {
        lambdaCompare(idx, cardinality, goldenVal, testVal);
    },
                                  tmpIODomain);
}
template <typename G, typename T, int C>
template <typename Lambda, typename FirstType, typename... ExportTypeVariadic_ta>
auto TestData<G, T, C>::forEachActiveIODomain(Lambda                                                 userCode,
                                              Neon::domain::tool::testing::IODomain<FirstType, int>& first,
                                              Neon::domain::tool::testing::IODomain<ExportTypeVariadic_ta, int>&... ioDomains)
{
    first.forEachActive(userCode, ioDomains...);
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::aInvXpY(Type* alpha, IODomain& A, IODomain& B)
    -> void
{
    this->template forEachActiveIODomain([&](const Neon::index_3d& idx,
                                             int                   cardinality,
                                             Type&                 A,
                                             Type&                 B) {
        B += (1.0 / (*alpha)) * A;
    },
                                         A, B);
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::axpy(const Type* alpha, IODomain& A, IODomain& B)
    -> void
{
    this->forEachActiveIODomain([&](const Neon::index_3d& /*idx*/,
                                    int /*cardinality*/,
                                    Type& a,
                                    Type& b) {
        b += (*alpha) * a;
    },
                                A, B);
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::laplace(IODomain& A, NEON_IO IODomain& B)
    -> void
{
    if (A.getCardinality() != 1) {
        NEON_THROW_UNSUPPORTED_OPTION("");
    }
    this->template forEachActiveIODomain([&](const Neon::index_3d& idx,
                                             int                   cardinality,
                                             Type& /*a*/,
                                             Type& b) {
        // Laplacian stencil operates on 6 neighbors (assuming 3D)
        T    res = 0;
        bool isValid = false;

        const std::array<Neon::int8_3d, 6> stencil{Neon::int8_3d(1, 0, 0),
                                                   Neon::int8_3d(-1, 0, 0),
                                                   Neon::int8_3d(0, 1, 0),
                                                   Neon::int8_3d(0, -1, 0),
                                                   Neon::int8_3d(0, 0, 1),
                                                   Neon::int8_3d(0, 0, -1)};
        for (const auto& direction : stencil) {
            auto neighborVal = A.nghVal(idx, direction, cardinality, &isValid);
            if (isValid) {
                res += neighborVal;
            }
        }
        b = -6 * res;
    },
                                         A, B);
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::compare(FieldNames         name,
                                [[maybe_unused]] T tollerance) -> bool
{
    bool isTheSame = false;
    if constexpr (std::is_integral_v<T>) {
        bool foundAnIssue = false;
        this->compare(name, [&](const Neon::index_3d& /*idx*/,
                                int /*cardinality*/,
                                const T& golden,
                                const T& computed) {
            if (golden != computed) {
                {
#pragma omp critical
                    {
                        foundAnIssue = true;
                        // std::stringstream s;
                        // s << idx.to_string() << " " << golden << " " << computed << std::endl;
                        // NEON_INFO(s.str());
                    }
                }
            }
        });
        isTheSame = !foundAnIssue;
    } else {
        bool foundAnIssue = false;
        this->compare(name, [&](const Neon::index_3d& idx,
                                int                   cardinality,
                                const T&              golden,
                                const T&              computed) {
            T goldenABS = std::abs(golden);
            T computedABS = std::abs(computed);
            T maxAbs = std::max(goldenABS, computedABS);

            auto relativeDiff = (maxAbs == 0.0 ? 0.0 : std::abs(golden - computed) / maxAbs);
            foundAnIssue = relativeDiff >= tollerance;
        });
        isTheSame = !foundAnIssue;
    }
    return isTheSame;
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::toString() const -> std::string
{
    std::stringstream s;
    s << " [Grid]:{ " << getGrid().toString()
      << "} [Cardinality]:{ " << mIODomains[0].getCardinality()
      << "} [Geometry]:{ " << GeometryUtils::toString(getGeometry())
      << "}";
    return s.str();
}

template <typename G, typename T, int C>
auto TestData<G, T, C>::getGeometry() const -> Neon::domain::tool::Geometry
{
    return mGeometry;
}
template <typename G, typename T, int C>
auto TestData<G, T, C>::dot(IODomain& A, IODomain& B, Type* alpha)
    -> void
{
    // Not the most efficinet way but it will do the trick for
    // computing the testing golden data
    T sum = 0;

    this->template forEachActiveIODomain([&](const Neon::index_3d& idx,
                                             int                   cardinality,
                                             Type&                 a,
                                             Type&                 b) {
        {
            const Type mul = a * b;
#pragma omp atomic
            sum += mul;
        }
    },
                                         A, B);
    *alpha = sum;
}
template <typename G, typename T, int C>
auto TestData<G, T, C>::updateIO() -> void
{
    auto& X = getField(FieldNames::X);
    auto& Y = getField(FieldNames::Y);
    auto& Z = getField(FieldNames::Z);
    auto& W = getField(FieldNames::W);

    X.updateIO(0);
    Y.updateIO(0);
    Z.updateIO(0);
    W.updateIO(0);

    getBackend().syncAll();
}
template <typename G, typename T, int C>
auto TestData<G, T, C>::updateCompute() -> void
{
    using FieldNames = Neon::domain::tool::testing::FieldNames;

    auto& X = getField(FieldNames::X);
    auto& Y = getField(FieldNames::Y);
    auto& Z = getField(FieldNames::Z);
    auto& W = getField(FieldNames::W);

    X.updateCompute(0);
    Y.updateCompute(0);
    Z.updateCompute(0);
    W.updateCompute(0);

    getBackend().syncAll();
}


}  // namespace Neon::domain::tool::testing
