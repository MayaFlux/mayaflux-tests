#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Transitive/Platform/SerialPorts.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    TEST(SerialPortsTest, EveryEntryIsNamedHasAPathAndTheListIsSortedByName)
    {
        const auto ports = Platform::list_serial_ports();

        for (const auto& port : ports) {
            EXPECT_FALSE(port.name.empty());
            EXPECT_FALSE(port.path.empty());
        }
        EXPECT_TRUE(std::ranges::is_sorted(ports,
            [](const Platform::SerialPortEntry& a, const Platform::SerialPortEntry& b) { return a.name < b.name; }));
    }

    TEST(SerialPortsTest, ListingTwiceGivesTheSameEntries)
    {
        const auto first = Platform::list_serial_ports();
        const auto second = Platform::list_serial_ports();

        ASSERT_EQ(first.size(), second.size());
        for (size_t i = 0; i < first.size(); ++i) {
            EXPECT_EQ(first[i].name, second[i].name);
            EXPECT_EQ(first[i].path, second[i].path);
        }
    }

    TEST(SerialPortsTest, NamesAreUniquePerPort)
    {
        const auto ports = Platform::list_serial_ports();
        std::set<std::string> names;
        for (const auto& port : ports) {
            EXPECT_TRUE(names.insert(port.name).second) << port.name;
        }
    }

} // namespace
} // namespace MayaFlux::Test
