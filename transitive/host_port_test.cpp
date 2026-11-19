#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Transitive/Protocol/HostPort.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    namespace P = MayaFlux::Transitive::Protocol;

    TEST(HostPortTest, APortIsOneToSixtyFiveFiveThirtyFive)
    {
        EXPECT_EQ(P::parse_port("1"), std::optional<uint16_t> { 1 });
        EXPECT_EQ(P::parse_port("80"), std::optional<uint16_t> { 80 });
        EXPECT_EQ(P::parse_port("65535"), std::optional<uint16_t> { 65535 });
    }

    TEST(HostPortTest, APortRefusesZeroAndWhatIsAboveTheRange)
    {
        EXPECT_FALSE(P::parse_port("0").has_value());
        EXPECT_FALSE(P::parse_port("65536").has_value());
        EXPECT_FALSE(P::parse_port("123456").has_value());
        EXPECT_FALSE(P::parse_port("99999999999").has_value());
    }

    TEST(HostPortTest, APortRefusesAnythingButDigits)
    {
        EXPECT_FALSE(P::parse_port("").has_value());
        EXPECT_FALSE(P::parse_port("-1").has_value());
        EXPECT_FALSE(P::parse_port("+1").has_value());
        EXPECT_FALSE(P::parse_port(" 80").has_value());
        EXPECT_FALSE(P::parse_port("80 ").has_value());
        EXPECT_FALSE(P::parse_port("8a").has_value());
        EXPECT_FALSE(P::parse_port("8.0").has_value());
    }

    TEST(HostPortTest, ALeadingZeroIsStillThePort)
    {
        EXPECT_EQ(P::parse_port("0080"), std::optional<uint16_t> { 80 });
    }

    TEST(HostPortTest, ANameAndAPortSplitAtTheColon)
    {
        const auto parsed = P::parse_host_port("localhost:9000");

        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->host, "localhost");
        EXPECT_EQ(parsed->port, 9000);
    }

    TEST(HostPortTest, AnIPv4AddressAndADottedNameAreHosts)
    {
        const auto address = P::parse_host_port("127.0.0.1:80");
        const auto name = P::parse_host_port("host.example.com:65535");

        ASSERT_TRUE(address.has_value());
        EXPECT_EQ(address->host, "127.0.0.1");
        ASSERT_TRUE(name.has_value());
        EXPECT_EQ(name->host, "host.example.com");
        EXPECT_EQ(name->port, 65535);
    }

    TEST(HostPortTest, ABracketedIPv6AddressIsReturnedWithoutItsBrackets)
    {
        const auto parsed = P::parse_host_port("[::1]:9000");

        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->host, "::1");
        EXPECT_EQ(parsed->port, 9000);
    }

    TEST(HostPortTest, AnUnbracketedIPv6AddressIsRefusedSinceItsPortCannotBeTold)
    {
        EXPECT_FALSE(P::parse_host_port("::1:80").has_value());
        EXPECT_FALSE(P::parse_host_port("host:80:90").has_value());
    }

    TEST(HostPortTest, ATextWithoutAUsablePortIsRefused)
    {
        EXPECT_FALSE(P::parse_host_port("localhost").has_value());
        EXPECT_FALSE(P::parse_host_port("localhost:").has_value());
        EXPECT_FALSE(P::parse_host_port("localhost:0").has_value());
        EXPECT_FALSE(P::parse_host_port("localhost:65536").has_value());
        EXPECT_FALSE(P::parse_host_port("[::1]").has_value());
    }

    TEST(HostPortTest, AnEmptyOrMalformedHostIsRefused)
    {
        EXPECT_FALSE(P::parse_host_port(":80").has_value());
        EXPECT_FALSE(P::parse_host_port("[]:80").has_value());
        EXPECT_FALSE(P::parse_host_port("[::1:80").has_value());
        EXPECT_FALSE(P::parse_host_port("ho st:80").has_value());
        EXPECT_FALSE(P::parse_host_port("host\t:80").has_value());
        EXPECT_FALSE(P::parse_host_port(" :80").has_value());
        EXPECT_FALSE(P::parse_host_port("").has_value());
    }

} // namespace
} // namespace MayaFlux::Test
