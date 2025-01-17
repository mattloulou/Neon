
#include "Neon/Neon.h"
#include "gridInterface.h"
#include "gtest/gtest.h"
#include "runHelper.h"

TEST(domain_unit_test_globalIdx, dGrid)
{
    int nGpus = 3;
    using Type = int64_t;
    runAllTestConfiguration(std::function(globalIdx::run<Neon::dGrid, Type, 0>),
                            nGpus,
                            1);
}

TEST(domain_unit_test_globalIdx, eGrid)
{
    int nGpus = 3;
    using Type = int64_t;
    runAllTestConfiguration(std::function(globalIdx::run<Neon::eGrid, Type, 0>),
                            nGpus,
                            1);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    Neon::init();
    return RUN_ALL_TESTS();
}
