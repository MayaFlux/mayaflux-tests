#include <gtest/gtest.h>

#include "../../support/audio_container.hpp"

#include "MayaFlux/Kakshya/Processors/SpatialRegionProcessor.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::OrganizedRegion;
    using Kakshya::Region;
    using Kakshya::RegionGroup;
    using Kakshya::SpatialRegionProcessor;

    class InspectableSpatialRegionProcessor : public SpatialRegionProcessor {
    public:
        using SpatialRegionProcessor::organize_container_data;

        [[nodiscard]] const std::vector<OrganizedRegion>& organized_regions() const { return m_organized_regions; }
    };

    TEST(SpatialRegionProcessorFacetTest, OrganizationCopiesGroupAndRegionFacets)
    {
        const Facet<int> group_only;
        const Facet<int> region_only;
        const Facet<int> shared;
        const auto container = make_audio_container(std::vector<double>(16, 0.0));
        RegionGroup group("areas");
        group.set(group_only, 1);
        group.set(shared, 2);
        Region region({ 10 });
        region.set(region_only, 3);
        region.set(shared, 4);
        group.add_region(region);
        container->add_region_group(group);
        InspectableSpatialRegionProcessor processor;

        processor.organize_container_data(container);

        ASSERT_EQ(processor.organized_regions().size(), 1U);
        const auto& organized = processor.organized_regions().front();
        ASSERT_NE(organized.get(group_only), nullptr);
        ASSERT_NE(organized.get(region_only), nullptr);
        ASSERT_NE(organized.get(shared), nullptr);
        EXPECT_EQ(*organized.get(group_only), 1);
        EXPECT_EQ(*organized.get(region_only), 3);
        EXPECT_EQ(*organized.get(shared), 4);
    }

} // namespace
} // namespace MayaFlux::Test
