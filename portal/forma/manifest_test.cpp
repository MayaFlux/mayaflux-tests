#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/forma_call_support.hpp"
#include "../../support/forma_manifest_support.hpp"
#include "../../support/scripted_window.hpp"

#include "MayaFlux/Portal/Forma/Manifest/CallTable.hpp"
#include "MayaFlux/Portal/Forma/Manifest/DragGeometry.hpp"
#include "MayaFlux/Portal/Forma/Manifest/ElementHit.hpp"
#include "MayaFlux/Portal/Forma/Manifest/FactoryTable.hpp"
#include "MayaFlux/Portal/Forma/Manifest/Instantiate.hpp"
#include "MayaFlux/Portal/Forma/Manifest/StaticGeometry.hpp"
#include "MayaFlux/Portal/Forma/Surface.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

struct FormaManifestNote {
    std::string text;
    int32_t times { 1 };
};

}

namespace MayaFlux::Reflect::Generated {

template <>
struct Description<::MayaFlux::Test::FormaManifestNote> {
    static constexpr std::string_view name = "MayaFlux::Test::FormaManifestNote";

    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::FormaManifestNote::text> { "text" },
            Member<&::MayaFlux::Test::FormaManifestNote::times> { "times" },
        };
    }
};

}

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using Kinesis::AABB2D;
using Portal::Graphics::PrimitiveTopology;

namespace {

    using Attributes = std::vector<std::pair<std::string, std::string>>;
    using ProbeTable = Forma::FactoryTable<FormaManifestElement>;
    using StubTable = Forma::FactoryTable<FormaManifestElement>;

    constexpr uint64_t k_factory_row = 7;
    constexpr uint64_t k_instance_row = 11;

    // =========================================================================
    // Typed table fixtures
    // =========================================================================

    struct ProbeLog {
        std::vector<FormaManifestProbe> inputs;
        std::vector<FormaManifestSpot> placements;
    };

    ProbeTable make_probe_table(const std::shared_ptr<ProbeLog>& log)
    {
        Reflect::RecordBinding<FormaManifestProbe, FormaManifestElement> input;
        Reflect::RecordBinding<FormaManifestSpot, FormaManifestElement> placement;
        placement.required("at");

        ProbeTable table;
        table.expose_typed<float>("probe", std::move(input), std::move(placement),
            [log](const FormaManifestProbe& decoded, const FormaManifestSpot& spot,
                std::string& error) -> std::optional<Forma::Geometry::Form<float>> {
                log->inputs.push_back(decoded);
                log->placements.push_back(spot);
                if (decoded.label == "bad") {
                    error = "refused label";
                    return std::nullopt;
                }
                if (decoded.label == "silent")
                    return std::nullopt;
                return Forma::Geometry::Form<float> {
                    [](float, std::vector<uint8_t>& out, Forma::Element&) { out.assign(4, 0); },
                    PrimitiveTopology::TRIANGLE_LIST,
                    64,
                };
            },
            [](const FormaManifestProbe& decoded, const FormaManifestSpot&,
                std::string&) -> std::optional<float> { return decoded.gain; });
        return table;
    }

    struct ProbePair {
        FormaManifestElement factory;
        FormaManifestElement instance;
    };

    ProbePair make_probe_pair(Attributes factory_attributes, Attributes instance_attributes)
    {
        ProbePair pair {
            .factory = manifest_node("probe", std::move(factory_attributes)),
            .instance = manifest_node("instance", std::move(instance_attributes)),
        };
        pair.factory.row = k_factory_row;
        pair.instance.row = k_instance_row;
        return pair;
    }

    Attributes valid_instance()
    {
        return { { "id", "i" }, { "template", "t" }, { "at", "1 2" } };
    }

    std::optional<ProbeTable::Placer> decode(const ProbeTable& table, const ProbePair& pair,
        std::string& error)
    {
        const auto* instantiator = table.find("probe");
        return std::invoke(*instantiator, pair.factory, pair.instance, error);
    }

    Kakshya::CompositeArray spot_rows(const std::vector<FormaManifestSpot>& spots)
    {
        Kakshya::CompositeArray rows(Kakshya::described_composite_layout<FormaManifestSpot>());
        Kakshya::CompositeInsertion insertion(rows);
        std::string error;
        for (const FormaManifestSpot& spot : spots)
            EXPECT_TRUE(Kakshya::write_described(insertion, insertion.append(), spot, error)) << error;
        return rows;
    }

    [[nodiscard]] std::string row_prefix(uint64_t row)
    {
        return "row " + std::to_string(row) + ":";
    }

    // =========================================================================
    // Driver fixtures
    // =========================================================================

    struct StubLog {
        std::vector<std::string> placed;
        std::vector<Forma::Surface*> surfaces;
        std::string refuse_decode;
        std::string refuse_place;
        int decodes {};
    };

    StubTable make_stub_table(const std::shared_ptr<StubLog>& log)
    {
        StubTable table;
        table.expose_placement("stub",
            [log](const FormaManifestElement& factory, const FormaManifestElement& instance,
                std::string& error) -> std::optional<StubTable::Placer> {
                const std::string id = manifest_attribute(instance, "id");
                const std::string extent = manifest_attribute(instance, "half");
                const float half = extent.empty() ? 1.F : std::stof(extent);
                ++log->decodes;
                if (id == log->refuse_decode) {
                    error = StubTable::located(instance.row, "stub refused " + id);
                    return std::nullopt;
                }
                return StubTable::Placer([log, id, half, size = manifest_attribute(factory, "size")](
                                             Forma::Surface& surface, std::string& place_error) -> std::optional<uint32_t> {
                    if (id == log->refuse_place) {
                        place_error = "stub could not place " + id;
                        return std::nullopt;
                    }
                    log->placed.push_back(size + ":" + id);
                    log->surfaces.push_back(&surface);
                    return surface.add(Forma::Element {}.with_rect({ -half, -half }, { half, half })).id();
                });
            });
        return table;
    }

}

// =============================================================================
// FactoryTable: typed registration
// =============================================================================

TEST(FormaManifestTableTest, DecodesInputAndPlacementThroughTheirDescribedMembers)
{
    const auto log = std::make_shared<ProbeLog>();
    const auto table = make_probe_table(log);
    const auto pair = make_probe_pair({ { "gain", "0.5" }, { "label", "x" }, { "on", "true" } },
        { { "id", "i" }, { "template", "t" }, { "at", "1 2" }, { "scale", "3" } });

    std::string error;
    const auto placer = decode(table, pair, error);

    ASSERT_TRUE(placer) << error;
    ASSERT_EQ(log->inputs.size(), 1U);
    EXPECT_FLOAT_EQ(log->inputs[0].gain, 0.5F);
    EXPECT_EQ(log->inputs[0].label, "x");
    EXPECT_TRUE(log->inputs[0].on);
    ASSERT_EQ(log->placements.size(), 1U);
    EXPECT_EQ(log->placements[0].at, glm::vec2(1.F, 2.F));
    EXPECT_FLOAT_EQ(log->placements[0].scale, 3.F);
}

TEST(FormaManifestTableTest, TheDocumentsReservedNamesAreSkippedOnAPlacement)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());
    const auto pair = make_probe_pair({}, { { "id", "i" }, { "template", "t" }, { "at", "0 0" } });

    std::string error;
    EXPECT_TRUE(decode(table, pair, error)) << error;
}

TEST(FormaManifestTableTest, EveryPrefixedAttributeIsLeftToTheDriverOnAPlacement)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());
    auto attributes = valid_instance();
    attributes.emplace_back("mf:hidden", "true");
    attributes.emplace_back("mf:anything", "else");
    const auto pair = make_probe_pair({}, attributes);

    std::string error;
    EXPECT_TRUE(decode(table, pair, error)) << error;
}

TEST(FormaManifestTableTest, AnOnChildOfAnInstanceIsLeftToTheDriverButOtherChildrenAreRejected)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());
    auto pair = make_probe_pair({}, valid_instance());
    pair.instance.children.push_back(manifest_node("on", { { "event", "MOUSE_BUTTON_PRESSED" }, { "action", "log" } }));

    std::string error;
    EXPECT_TRUE(decode(table, pair, error)) << error;

    pair.instance.children.push_back(manifest_node("legend"));
    EXPECT_FALSE(decode(table, pair, error));
    EXPECT_NE(error.find("<legend> is not allowed in <instance>"), std::string::npos) << error;
}

TEST(FormaManifestTableTest, AnUnknownPlacementAttributeIsRejectedAtTheInstanceRow)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());
    auto attributes = valid_instance();
    attributes.emplace_back("color", "red");
    const auto pair = make_probe_pair({}, attributes);

    std::string error;
    EXPECT_FALSE(decode(table, pair, error));
    EXPECT_TRUE(error.starts_with(row_prefix(k_instance_row))) << error;
    EXPECT_NE(error.find("color"), std::string::npos) << error;
}

TEST(FormaManifestTableTest, ARequiredPlacementAttributeMustBePresent)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());
    const auto pair = make_probe_pair({}, { { "id", "i" }, { "template", "t" } });

    std::string error;
    EXPECT_FALSE(decode(table, pair, error));
    EXPECT_TRUE(error.starts_with(row_prefix(k_instance_row))) << error;
    EXPECT_NE(error.find("at"), std::string::npos) << error;
}

TEST(FormaManifestTableTest, AnInvalidInputValueIsReportedAtTheFactoryRow)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());
    const auto pair = make_probe_pair({ { "gain", "loud" } }, valid_instance());

    std::string error;
    EXPECT_FALSE(decode(table, pair, error));
    EXPECT_TRUE(error.starts_with(row_prefix(k_factory_row))) << error;
    EXPECT_NE(error.find("gain"), std::string::npos) << error;
}

TEST(FormaManifestTableTest, AnUnknownInputAttributeIsRejectedAtTheFactoryRow)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());
    const auto pair = make_probe_pair({ { "bogus", "1" } }, valid_instance());

    std::string error;
    EXPECT_FALSE(decode(table, pair, error));
    EXPECT_TRUE(error.starts_with(row_prefix(k_factory_row))) << error;
    EXPECT_NE(error.find("bogus"), std::string::npos) << error;
}

TEST(FormaManifestTableTest, AFactoryRefusalIsReportedAtTheInstanceRow)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());
    const auto pair = make_probe_pair({ { "label", "bad" } }, valid_instance());

    std::string error;
    EXPECT_FALSE(decode(table, pair, error));
    EXPECT_EQ(error, row_prefix(k_instance_row) + " refused label");
}

TEST(FormaManifestTableTest, AFactoryRefusalWithoutAMessageGetsADefault)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());
    const auto pair = make_probe_pair({ { "label", "silent" } }, valid_instance());

    std::string error;
    EXPECT_FALSE(decode(table, pair, error));
    EXPECT_EQ(error, row_prefix(k_instance_row) + " could not create the geometry");
}

// =============================================================================
// FactoryTable: bulk decoding
// =============================================================================

TEST(FormaManifestTableTest, TypedRegistrationAlsoRegistersABulkPath)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());

    EXPECT_NE(table.find("probe"), nullptr);
    EXPECT_NE(table.find_bulk("probe"), nullptr);
}

TEST(FormaManifestTableTest, BulkMakesOnePlacerPerRowInRowOrder)
{
    const auto log = std::make_shared<ProbeLog>();
    const auto table = make_probe_table(log);
    const auto rows = spot_rows({ { .at = { 1.F, 1.F }, .scale = 2.F },
        { .at = { 2.F, 2.F }, .scale = 4.F },
        { .at = { 3.F, 3.F }, .scale = 8.F } });
    const auto factory = make_probe_pair({ { "gain", "0.25" } }, {}).factory;

    std::string error;
    const auto placers = std::invoke(*table.find_bulk("probe"), factory, rows, error);

    ASSERT_TRUE(placers) << error;
    EXPECT_EQ(placers->size(), 3U);
    ASSERT_EQ(log->placements.size(), 3U);
    EXPECT_FLOAT_EQ(log->placements[0].scale, 2.F);
    EXPECT_FLOAT_EQ(log->placements[1].scale, 4.F);
    EXPECT_FLOAT_EQ(log->placements[2].scale, 8.F);
    for (const FormaManifestProbe& input : log->inputs)
        EXPECT_FLOAT_EQ(input.gain, 0.25F);
}

TEST(FormaManifestTableTest, BulkRejectsRowsLaidOutForAnotherType)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());
    Kakshya::CompositeArray rows(Kakshya::described_composite_layout<Forma::RectPlacementInputs>());
    const auto factory = make_probe_pair({}, {}).factory;

    std::string error;
    EXPECT_FALSE(std::invoke(*table.find_bulk("probe"), factory, rows, error));
    EXPECT_TRUE(error.starts_with(row_prefix(k_factory_row))) << error;
    EXPECT_NE(error.find("layout"), std::string::npos) << error;
}

TEST(FormaManifestTableTest, BulkReportsAnInputErrorAtTheFactoryRow)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());
    const auto rows = spot_rows({ {} });
    const auto factory = make_probe_pair({ { "gain", "loud" } }, {}).factory;

    std::string error;
    EXPECT_FALSE(std::invoke(*table.find_bulk("probe"), factory, rows, error));
    EXPECT_TRUE(error.starts_with(row_prefix(k_factory_row))) << error;
}

TEST(FormaManifestTableTest, BulkFailsAsAWholeWhenTheFactoryRefusesARow)
{
    const auto table = make_probe_table(std::make_shared<ProbeLog>());
    const auto rows = spot_rows({ {}, {} });
    const auto factory = make_probe_pair({ { "label", "bad" } }, {}).factory;

    std::string error;
    EXPECT_FALSE(std::invoke(*table.find_bulk("probe"), factory, rows, error));
    EXPECT_EQ(error, row_prefix(k_factory_row) + " refused label");
}

// =============================================================================
// FactoryTable: names and merging
// =============================================================================

TEST(FormaManifestTableTest, NamesAreSortedAndContainsFollowsRegistration)
{
    auto table = make_probe_table(std::make_shared<ProbeLog>());
    table.expose_placement("alpha", [](const FormaManifestElement&, const FormaManifestElement&,
                                        std::string&) -> std::optional<ProbeTable::Placer> {
        return std::nullopt;
    });

    EXPECT_EQ(table.names(), (std::vector<std::string> { "alpha", "probe" }));
    EXPECT_TRUE(table.contains("alpha"));
    EXPECT_FALSE(table.contains("beta"));
    EXPECT_EQ(table.find("beta"), nullptr);
    EXPECT_EQ(table.find_bulk("beta"), nullptr);
}

TEST(FormaManifestTableTest, RegisteringAWholePlacementOverATypedNameDropsItsBulkPath)
{
    auto table = make_probe_table(std::make_shared<ProbeLog>());
    ASSERT_NE(table.find_bulk("probe"), nullptr);

    table.expose_placement("probe", [](const FormaManifestElement&, const FormaManifestElement&,
                                        std::string&) -> std::optional<ProbeTable::Placer> {
        return std::nullopt;
    });

    EXPECT_NE(table.find("probe"), nullptr);
    EXPECT_EQ(table.find_bulk("probe"), nullptr);
}

TEST(FormaManifestTableTest, MergeAddsEveryEntryUnderAPrefixWithItsBulkPath)
{
    const auto source = make_probe_table(std::make_shared<ProbeLog>());
    ProbeTable target;

    std::string error;
    ASSERT_TRUE(target.merge(source, "studio.", error)) << error;

    EXPECT_TRUE(target.contains("studio.probe"));
    EXPECT_FALSE(target.contains("probe"));
    EXPECT_NE(target.find_bulk("studio.probe"), nullptr);
}

TEST(FormaManifestTableTest, AFailedMergeNamesTheCollisionAndLeavesTheTargetUnchanged)
{
    const auto source = make_probe_table(std::make_shared<ProbeLog>());
    auto target = make_probe_table(std::make_shared<ProbeLog>());
    target.expose_placement("extra", [](const FormaManifestElement&, const FormaManifestElement&,
                                         std::string&) -> std::optional<ProbeTable::Placer> {
        return std::nullopt;
    });
    const auto before = target.names();

    std::string error;
    EXPECT_FALSE(target.merge(source, "", error));

    EXPECT_NE(error.find("probe"), std::string::npos) << error;
    EXPECT_EQ(target.names(), before);
}

TEST(FormaManifestTableTest, MergeWithReplaceOverwritesACollidingName)
{
    const auto source = make_probe_table(std::make_shared<ProbeLog>());
    ProbeTable target;
    target.expose_placement("probe", [](const FormaManifestElement&, const FormaManifestElement&,
                                         std::string&) -> std::optional<ProbeTable::Placer> {
        return std::nullopt;
    });
    ASSERT_EQ(target.find_bulk("probe"), nullptr);

    std::string error;
    ASSERT_TRUE(target.merge(source, "", error, true)) << error;

    EXPECT_NE(target.find_bulk("probe"), nullptr);
}

// =============================================================================
// instantiate_document: the driver over a plain element tree
// =============================================================================

class FormaManifestDriverTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        surfaces.emplace("main", make_surface("manifest-main"));
        log = std::make_shared<StubLog>();
        table = make_stub_table(log);
    }

    Forma::Surface make_surface(const std::string& name)
    {
        auto layer = std::make_shared<Forma::Layer>();
        auto ctx = std::make_shared<Forma::Context>(layer, window, manager, name);
        return Forma::Surface(Forma::SurfaceConfig { .window = window, .layer = layer, .ctx = ctx, .detach_on_close = false });
    }

    std::optional<Forma::PlacedInstances> run(const FormaManifestElement& document)
    {
        return Forma::instantiate_document(table, document, surfaces, error);
    }

    std::optional<Forma::PlacedInstances> run_with_actions(const FormaManifestElement& document)
    {
        return Forma::instantiate_document(table, actions, document, surfaces, error);
    }

    void expect_rejected(const FormaManifestElement& document, std::string_view fragment,
        std::optional<uint64_t> row = std::nullopt, bool with_actions = false)
    {
        EXPECT_FALSE(with_actions ? run_with_actions(document) : run(document));
        EXPECT_NE(error.find(fragment), std::string::npos) << error;
        if (row)
            EXPECT_TRUE(error.starts_with(row_prefix(*row))) << error;
        EXPECT_TRUE(log->placed.empty());
    }

    std::shared_ptr<Vruta::EventManager> manager = std::make_shared<Vruta::EventManager>();
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(200U, 100U);
    Forma::SurfaceMap surfaces;
    std::shared_ptr<StubLog> log;
    StubTable table;
    Forma::ActionTable<FormaManifestElement> actions;
    std::string error;
};

TEST_F(FormaManifestDriverTest, PlacesEveryInstanceInDocumentOrderOnTheBoundSurface)
{
    const auto count = run(manifest_document());

    ASSERT_TRUE(count) << error;
    EXPECT_EQ(count->size(), 3U);
    EXPECT_EQ(log->placed, (std::vector<std::string> { "3:a", "5:b", "3:c" }));
    for (Forma::Surface* placed_on : log->surfaces)
        EXPECT_EQ(placed_on, &surfaces.at("main"));
}

TEST_F(FormaManifestDriverTest, TheSectionsMayAppearInEitherOrder)
{
    auto document = manifest_document();
    std::swap(document.children[0], document.children[1]);

    const auto count = run(manifest_numbered(document));

    ASSERT_TRUE(count) << error;
    EXPECT_EQ(count->size(), 3U);
}

TEST_F(FormaManifestDriverTest, ADocumentWithoutSurfacesPlacesNothingAndSucceeds)
{
    auto document = manifest_document();
    document.children.pop_back();

    const auto count = run(manifest_numbered(document));

    ASSERT_TRUE(count) << error;
    EXPECT_EQ(count->size(), 0U);
}

TEST_F(FormaManifestDriverTest, SuccessClearsAStaleError)
{
    error = "stale";

    ASSERT_TRUE(run(manifest_document()));

    EXPECT_TRUE(error.empty());
}

TEST_F(FormaManifestDriverTest, EachSurfaceReceivesItsOwnInstances)
{
    surfaces.emplace("side", make_surface("manifest-side"));
    auto document = manifest_document();
    document.children.push_back(manifest_node("surface", { { "id", "side" } },
        { manifest_node("instance", { { "id", "d" }, { "template", "second" } }) }));

    const auto count = run(manifest_numbered(document));

    ASSERT_TRUE(count) << error;
    EXPECT_EQ(count->size(), 4U);
    EXPECT_EQ(log->surfaces[0], &surfaces.at("main"));
    EXPECT_EQ(log->surfaces[2], &surfaces.at("main"));
    EXPECT_EQ(log->surfaces[3], &surfaces.at("side"));
}

TEST_F(FormaManifestDriverTest, ASurfaceTheHostDidNotBindFailsBeforeAnythingIsDecoded)
{
    surfaces.erase("main");
    const auto document = manifest_document();

    expect_rejected(document, "surface main is not bound by the host", document.children[1].row);
    EXPECT_EQ(log->decodes, 0);
}

TEST_F(FormaManifestDriverTest, ADecodeFailureStopsBeforeAnythingIsPlaced)
{
    log->refuse_decode = "b";
    const auto document = manifest_document();

    expect_rejected(document, "stub refused b", document.children[1].children[1].row);
    EXPECT_EQ(log->decodes, 2);
}

TEST_F(FormaManifestDriverTest, APlacementFailureIsReportedAndEarlierInstancesStay)
{
    log->refuse_place = "b";

    EXPECT_FALSE(run(manifest_document()));

    EXPECT_EQ(error, "stub could not place b");
    EXPECT_EQ(log->placed, (std::vector<std::string> { "3:a" }));
}

TEST_F(FormaManifestDriverTest, TheRootMustBeNamedForma)
{
    auto document = manifest_document();
    document.name = "other";

    expect_rejected(manifest_numbered(document), "root element must be <forma>", 0);
}

TEST_F(FormaManifestDriverTest, TheVersionIsRequiredAndMustBeOne)
{
    auto missing = manifest_document();
    missing.attributes.clear();
    expect_rejected(manifest_numbered(missing), "needs a version attribute", 0);

    auto unsupported = manifest_document();
    unsupported.attributes[0].second = "2";
    expect_rejected(manifest_numbered(unsupported), "unsupported Forma document version 2", 0);

    auto invalid = manifest_document();
    invalid.attributes[0].second = "one";
    expect_rejected(manifest_numbered(invalid), "unsupported Forma document version one", 0);
}

TEST_F(FormaManifestDriverTest, AnyOtherRootAttributeIsRejected)
{
    auto document = manifest_document();
    document.attributes.emplace_back("mode", "fast");

    expect_rejected(manifest_numbered(document), "unsupported attribute mode", 0);
}

TEST_F(FormaManifestDriverTest, AnUnknownSectionIsRejectedAtItsRow)
{
    auto document = manifest_document();
    document.children.push_back(manifest_node("legend"));
    document = manifest_numbered(document);

    expect_rejected(document, "<legend> is not allowed in <forma>", document.children.back().row);
}

TEST_F(FormaManifestDriverTest, TemplatesMayAppearOnlyOnce)
{
    auto document = manifest_document();
    document.children.push_back(manifest_node("templates"));
    document = manifest_numbered(document);

    expect_rejected(document, "<templates> may appear only once", document.children.back().row);
}

TEST_F(FormaManifestDriverTest, TemplatesTakeNoAttributesAndOnlyTemplateChildren)
{
    auto attributed = manifest_document();
    attributed.children[0].attributes.emplace_back("x", "1");
    expect_rejected(manifest_numbered(attributed), "unsupported attribute x");

    auto foreign = manifest_document();
    foreign.children[0].children.push_back(manifest_node("item"));
    expect_rejected(manifest_numbered(foreign), "<item> is not allowed in <templates>");
}

TEST_F(FormaManifestDriverTest, ATemplateNeedsAnIdThatIsNotEmpty)
{
    auto missing = manifest_document();
    missing.children[0].children[0].attributes.clear();
    expect_rejected(manifest_numbered(missing), "needs a non-empty id");

    auto empty = manifest_document();
    empty.children[0].children[0].attributes[0].second = "";
    expect_rejected(manifest_numbered(empty), "needs a non-empty id");
}

TEST_F(FormaManifestDriverTest, ATemplateTakesNoAttributeBesidesItsId)
{
    auto document = manifest_document();
    document.children[0].children[0].attributes.emplace_back("kind", "x");

    expect_rejected(manifest_numbered(document), "unsupported attribute kind");
}

TEST_F(FormaManifestDriverTest, ATemplateNeedsExactlyOneFactoryElement)
{
    auto none = manifest_document();
    none.children[0].children[0].children.clear();
    expect_rejected(manifest_numbered(none), "exactly one factory element");

    auto two = manifest_document();
    two.children[0].children[0].children.push_back(manifest_node("stub"));
    expect_rejected(manifest_numbered(two), "exactly one factory element");
}

TEST_F(FormaManifestDriverTest, AnUnregisteredFactoryIsRejectedAtItsRow)
{
    auto document = manifest_document();
    document.children[0].children[0].children[0].name = "mystery";
    document = manifest_numbered(document);

    expect_rejected(document, "no registered factory mystery",
        document.children[0].children[0].children[0].row);
    EXPECT_EQ(log->decodes, 0);
}

TEST_F(FormaManifestDriverTest, IdsAreUniqueAcrossTemplatesSurfacesAndInstances)
{
    auto templates = manifest_document();
    templates.children[0].children[1].attributes[0].second = "first";
    expect_rejected(manifest_numbered(templates), "already used at row");

    auto surface = manifest_document();
    surface.children[1].attributes[0].second = "first";
    expect_rejected(manifest_numbered(surface), "already used at row");

    auto instance = manifest_document();
    instance.children[1].children[0].attributes[0].second = "first";
    expect_rejected(manifest_numbered(instance), "already used at row");

    auto repeated = manifest_document();
    repeated.children[1].children[1].attributes[0].second = "a";
    expect_rejected(manifest_numbered(repeated), "id a is already used at row");
}

TEST_F(FormaManifestDriverTest, ASurfaceNeedsAnIdAndTakesNoOtherAttribute)
{
    auto missing = manifest_document();
    missing.children[1].attributes.clear();
    expect_rejected(manifest_numbered(missing), "needs a non-empty id");

    auto extra = manifest_document();
    extra.children[1].attributes.emplace_back("title", "x");
    expect_rejected(manifest_numbered(extra), "unsupported attribute title");
}

TEST_F(FormaManifestDriverTest, ASurfaceRejectsAChildThatIsNeitherAnInstanceNorAnElement)
{
    auto document = manifest_document();
    document.children[1].children.push_back(manifest_node("legend"));

    expect_rejected(manifest_numbered(document), "<legend> is not allowed in <surface>");
}

TEST_F(FormaManifestDriverTest, AnInstanceNeedsAnIdAndAKnownTemplate)
{
    auto no_id = manifest_document();
    no_id.children[1].children[0].attributes.erase(no_id.children[1].children[0].attributes.begin());
    expect_rejected(manifest_numbered(no_id), "needs a non-empty id");

    auto no_template = manifest_document();
    no_template.children[1].children[0].attributes.erase(
        no_template.children[1].children[0].attributes.begin() + 1);
    expect_rejected(manifest_numbered(no_template), "needs a template attribute");

    auto unknown = manifest_document();
    unknown.children[1].children[0].attributes[1].second = "ghost";
    unknown = manifest_numbered(unknown);
    expect_rejected(unknown, "unknown template ghost", unknown.children[1].children[0].row);
    EXPECT_EQ(log->decodes, 0);
}

// =============================================================================
// instantiate_rows
// =============================================================================

TEST_F(FormaManifestDriverTest, RowsNeedATemplateThatExists)
{
    Kakshya::CompositeArray rows(Kakshya::described_composite_layout<Forma::RectPlacementInputs>());

    EXPECT_FALSE(Forma::instantiate_rows(table, manifest_document(), "ghost", rows,
        surfaces.at("main"), error));

    EXPECT_EQ(error, "document has no template ghost");
}

TEST_F(FormaManifestDriverTest, RowsNeedAFactoryRegisteredWithABulkPath)
{
    Kakshya::CompositeArray rows(Kakshya::described_composite_layout<Forma::RectPlacementInputs>());
    const auto document = manifest_document();

    EXPECT_FALSE(Forma::instantiate_rows(table, document, "first", rows,
        surfaces.at("main"), error));

    EXPECT_NE(error.find("has no bulk placement"), std::string::npos) << error;
    EXPECT_TRUE(error.starts_with(row_prefix(document.children[0].children[0].children[0].row))) << error;
}

TEST_F(FormaManifestDriverTest, RowsAreStillCheckedAgainstTheWholeDocument)
{
    Kakshya::CompositeArray rows(Kakshya::described_composite_layout<Forma::RectPlacementInputs>());
    auto document = manifest_document();
    document.name = "other";

    EXPECT_FALSE(Forma::instantiate_rows(table, manifest_numbered(document), "first", rows,
        surfaces.at("main"), error));

    EXPECT_NE(error.find("root element must be <forma>"), std::string::npos) << error;
}

// =============================================================================
// instantiate_document: the result, names and the document's Layer attributes
// =============================================================================

namespace {

    FormaManifestElement& manifest_instance(FormaManifestElement& document, size_t index)
    {
        return document.children[1].children[index];
    }

}

TEST_F(FormaManifestDriverTest, TheResultMapsEachInstanceToItsSurfaceAndElement)
{
    const auto placed = run(manifest_document());

    ASSERT_TRUE(placed) << error;
    ASSERT_EQ(placed->size(), 3U);
    for (const char* id : { "a", "b", "c" }) {
        const auto found = placed->find(id);
        ASSERT_NE(found, placed->end()) << id;
        EXPECT_EQ(found->second.surface, "main");
        EXPECT_NE(surfaces.at("main").layer().get(found->second.element), nullptr) << id;
    }
}

TEST_F(FormaManifestDriverTest, EveryPlacedElementIsNamedAfterItsInstanceAndFoundByThatName)
{
    const auto placed = run(manifest_document());
    ASSERT_TRUE(placed) << error;

    const Forma::Layer& layer = surfaces.at("main").layer();
    for (const auto& [id, instance] : *placed)
        EXPECT_EQ(layer.get(instance.element)->name, id);
    EXPECT_EQ(layer.find("b"), std::optional<uint32_t>(placed->at("b").element));
    EXPECT_FALSE(layer.find("ghost"));
}

TEST_F(FormaManifestDriverTest, HiddenInteractiveAndClipAreAppliedToThePlacedElement)
{
    auto document = manifest_document();
    manifest_instance(document, 0).attributes.emplace_back("mf:hidden", "true");
    manifest_instance(document, 1).attributes.emplace_back("mf:interactive", "false");
    manifest_instance(document, 2).attributes.emplace_back("mf:clip", "-0.5 -0.5 1 1");

    const auto placed = run(manifest_numbered(document));
    ASSERT_TRUE(placed) << error;

    const Forma::Layer& layer = surfaces.at("main").layer();
    EXPECT_FALSE(layer.get(placed->at("a").element)->visible);
    EXPECT_TRUE(layer.get(placed->at("a").element)->interactive);
    EXPECT_FALSE(layer.get(placed->at("b").element)->interactive);
    EXPECT_TRUE(layer.get(placed->at("b").element)->visible);
    const Forma::Element* clipped = layer.get(placed->at("c").element);
    ASSERT_TRUE(clipped->clip);
    EXPECT_EQ(clipped->clip->min, glm::vec2(-0.5F, -0.5F));
    EXPECT_EQ(clipped->clip->max, glm::vec2(0.5F, 0.5F));
}

TEST_F(FormaManifestDriverTest, RelateTiesAnInstanceToAnotherOnTheSameSurface)
{
    auto document = manifest_document();
    manifest_instance(document, 1).attributes.emplace_back("mf:relate", "a");

    const auto placed = run(manifest_numbered(document));
    ASSERT_TRUE(placed) << error;

    const Forma::Layer& layer = surfaces.at("main").layer();
    EXPECT_EQ(layer.related_ids(placed->at("a").element),
        std::vector<uint32_t> { placed->at("b").element });
    EXPECT_TRUE(layer.related_ids(placed->at("c").element).empty());
}

TEST_F(FormaManifestDriverTest, FrontAndBackChangeWhichOverlappingInstanceIsHit)
{
    const glm::vec2 point { 0.F, 0.F };

    const auto untouched = run(manifest_document());
    ASSERT_TRUE(untouched) << error;
    EXPECT_EQ(surfaces.at("main").layer().hit_test(point), untouched->at("c").element);

    auto back = manifest_document();
    manifest_instance(back, 2).attributes.emplace_back("mf:back", "true");
    surfaces.at("main").layer().clear();
    const auto sent_back = run(manifest_numbered(back));
    ASSERT_TRUE(sent_back) << error;
    EXPECT_EQ(surfaces.at("main").layer().hit_test(point), sent_back->at("b").element);

    auto front = manifest_document();
    manifest_instance(front, 0).attributes.emplace_back("mf:front", "true");
    surfaces.at("main").layer().clear();
    const auto brought_front = run(manifest_numbered(front));
    ASSERT_TRUE(brought_front) << error;
    EXPECT_EQ(surfaces.at("main").layer().hit_test(point), brought_front->at("a").element);
}

TEST_F(FormaManifestDriverTest, LayerAttributesAreCheckedBeforeAnythingIsPlaced)
{
    auto unknown = manifest_document();
    manifest_instance(unknown, 0).attributes.emplace_back("mf:bogus", "1");
    unknown = manifest_numbered(unknown);
    expect_rejected(unknown, "unsupported attribute mf:bogus", manifest_instance(unknown, 0).row);

    auto invalid = manifest_document();
    manifest_instance(invalid, 1).attributes.emplace_back("mf:hidden", "maybe");
    expect_rejected(manifest_numbered(invalid), "invalid mf:hidden attribute");

    auto both = manifest_document();
    manifest_instance(both, 0).attributes.emplace_back("mf:front", "true");
    manifest_instance(both, 0).attributes.emplace_back("mf:back", "true");
    expect_rejected(manifest_numbered(both), "cannot both be set");

    auto clip_numbers = manifest_document();
    manifest_instance(clip_numbers, 0).attributes.emplace_back("mf:clip", "1 2 3");
    expect_rejected(manifest_numbered(clip_numbers), "four numbers in mf:clip");

    auto clip_area = manifest_document();
    manifest_instance(clip_area, 0).attributes.emplace_back("mf:clip", "0 0 0 1");
    expect_rejected(manifest_numbered(clip_area), "mf:clip needs a positive width and height");

    EXPECT_EQ(log->decodes, 0);
}

TEST_F(FormaManifestDriverTest, RelateMustNameAnotherInstanceOfTheSameSurface)
{
    auto empty = manifest_document();
    manifest_instance(empty, 0).attributes.emplace_back("mf:relate", "");
    expect_rejected(manifest_numbered(empty), "needs an instance id");

    auto unknown = manifest_document();
    manifest_instance(unknown, 0).attributes.emplace_back("mf:relate", "ghost");
    unknown = manifest_numbered(unknown);
    expect_rejected(unknown, "mf:relate refers to ghost, which is not an instance of surface main",
        manifest_instance(unknown, 0).row);

    auto self = manifest_document();
    manifest_instance(self, 0).attributes.emplace_back("mf:relate", "a");
    expect_rejected(manifest_numbered(self), "may not name the instance itself");

    surfaces.emplace("side", make_surface("manifest-side"));
    auto across = manifest_document();
    across.children.push_back(manifest_node("surface", { { "id", "side" } },
        { manifest_node("instance", { { "id", "d" }, { "template", "second" }, { "mf:relate", "a" } }) }));
    expect_rejected(manifest_numbered(across), "not an instance of surface side");
}

TEST_F(FormaManifestDriverTest, AnElementThePlacerReturnsThatTheSurfaceDoesNotHoldIsReported)
{
    table.expose_placement("ghost", [](const FormaManifestElement&, const FormaManifestElement&,
                                        std::string&) -> std::optional<StubTable::Placer> {
        return StubTable::Placer([](Forma::Surface&, std::string&) -> std::optional<uint32_t> { return 9999U; });
    });
    auto document = manifest_document();
    document.children[0].children[0].children[0].name = "ghost";
    document = manifest_numbered(document);

    EXPECT_FALSE(run(document));

    EXPECT_NE(error.find("placed an element the surface does not hold"), std::string::npos) << error;
    EXPECT_TRUE(error.starts_with(row_prefix(manifest_instance(document, 0).row))) << error;
}

// =============================================================================
// instantiate_document: actions attached by the document
// =============================================================================

namespace {

    using ActionEntries = std::shared_ptr<std::vector<std::string>>;

    void expose_log_action(Forma::ActionTable<FormaManifestElement>& actions, const ActionEntries& entries)
    {
        actions.expose("log", Forma::Action([entries](const Forma::ActionEvent& event) {
            entries->push_back(std::string(Reflect::enum_to_string(event.event.type)) + ":" + std::string(event.instance));
        }));
    }

    FormaManifestElement on_element(std::string event, Attributes extra = {})
    {
        Attributes attributes { { "event", std::move(event) }, { "action", "log" } };
        attributes.insert(attributes.end(), extra.begin(), extra.end());
        return manifest_node("on", std::move(attributes));
    }

    FormaManifestElement action_document(std::vector<FormaManifestElement> on_children, Attributes extra = {})
    {
        Attributes attributes { { "id", "a" }, { "template", "first" } };
        attributes.insert(attributes.end(), extra.begin(), extra.end());
        return manifest_numbered(manifest_node("forma", { { "version", "1" } },
            {
                manifest_node("templates", {},
                    { manifest_node("template", { { "id", "first" } },
                        { manifest_node("stub", { { "size", "3" } }) }) }),
                manifest_node("surface", { { "id", "main" } },
                    { manifest_node("instance", std::move(attributes), std::move(on_children)) }),
            }));
    }

}

TEST_F(FormaManifestDriverTest, EveryPointerEventTypeReachesItsActionWithTheInstanceId)
{
    const auto entries = std::make_shared<std::vector<std::string>>();
    expose_log_action(actions, entries);
    const auto document = action_document({ on_element("MOUSE_ENTERED"), on_element("MOUSE_MOTION"),
                                              on_element("MOUSE_BUTTON_PRESSED"),
                                              on_element("MOUSE_MOTION", { { "button", "Left" } }),
                                              on_element("MOUSE_BUTTON_RELEASED"), on_element("MOUSE_SCROLLED"),
                                              on_element("MOUSE_EXITED") },
        { { "half", "0.3" } });
    ASSERT_TRUE(run_with_actions(document)) << error;

    window->move_to_ndc({ 0.F, 0.F });
    window->press(IO::MouseButtons::Left);
    window->move_to_ndc({ 0.1F, 0.F });
    window->release(IO::MouseButtons::Left);
    window->scroll(1.F, 2.F);
    window->move_to_ndc({ 0.8F, 0.F });

    std::ranges::sort(*entries);
    EXPECT_EQ(*entries, (std::vector<std::string> { "MOUSE_BUTTON_PRESSED:a", "MOUSE_BUTTON_RELEASED:a",
                            "MOUSE_ENTERED:a", "MOUSE_EXITED:a", "MOUSE_MOTION:a", "MOUSE_MOTION:a",
                            "MOUSE_MOTION:a", "MOUSE_SCROLLED:a" }));
}

TEST_F(FormaManifestDriverTest, MotionWithoutAButtonIsHoverAndMotionWithAButtonIsTheDrag)
{
    const auto hover = std::make_shared<std::vector<std::string>>();
    const auto drag = std::make_shared<std::vector<std::string>>();
    expose_log_action(actions, hover);
    actions.expose("drag", Forma::Action([drag](const Forma::ActionEvent& event) {
        drag->push_back(std::string(event.instance));
    }));
    auto hovering = on_element("MOUSE_MOTION");
    auto dragging = manifest_node("on", { { "event", "MOUSE_MOTION" }, { "button", "Left" }, { "action", "drag" } });
    ASSERT_TRUE(run_with_actions(action_document({ hovering, dragging }, { { "half", "0.9" } }))) << error;

    window->move_to_ndc({ 0.F, 0.F });
    window->press(IO::MouseButtons::Left);
    window->move_to_ndc({ 0.5F, 0.F });

    EXPECT_EQ(hover->size(), 2U);
    EXPECT_EQ(drag->size(), 1U);
}

TEST_F(FormaManifestDriverTest, AnActionIsToldTheWindowEventTheElementTheInstanceAndThePointer)
{
    uint32_t seen_element = 0;
    std::string seen_instance;
    glm::vec2 seen_position {};
    std::optional<Core::WindowEvent> seen_event;
    actions.expose("log", Forma::Action([&](const Forma::ActionEvent& event) {
        seen_element = event.element;
        seen_instance = std::string(event.instance);
        seen_position = event.position;
        seen_event = event.event;
        EXPECT_EQ(&event.layer, &surfaces.at("main").layer());
    }));
    const auto placed = run_with_actions(action_document({ on_element("MOUSE_BUTTON_PRESSED") }));
    ASSERT_TRUE(placed) << error;

    window->move_to_ndc({ 0.25F, -0.25F });
    window->press(IO::MouseButtons::Right);

    EXPECT_EQ(seen_element, placed->at("a").element);
    EXPECT_EQ(seen_instance, "a");
    EXPECT_NEAR(seen_position.x, 0.25F, 1e-4F);
    EXPECT_NEAR(seen_position.y, -0.25F, 1e-4F);
    ASSERT_TRUE(seen_event.has_value());
    EXPECT_EQ(seen_event->type, Core::WindowEventType::MOUSE_BUTTON_PRESSED);
    const auto* data = std::get_if<Core::WindowEvent::MouseButtonData>(&seen_event->data);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->button, static_cast<int8_t>(IO::MouseButtons::Right));
}

TEST_F(FormaManifestDriverTest, AnActionRunsBesideTheElementsOwnCallbackAndNeverReplacesIt)
{
    const auto entries = std::make_shared<std::vector<std::string>>();
    expose_log_action(actions, entries);
    const auto placed = run_with_actions(action_document({ on_element("MOUSE_BUTTON_PRESSED") }));
    ASSERT_TRUE(placed) << error;

    surfaces.at("main").ctx().on_press(placed->at("a").element, IO::MouseButtons::Left,
        [entries](uint32_t, glm::vec2) { entries->emplace_back("own"); });
    window->move_to_ndc({ 0.F, 0.F });
    window->press(IO::MouseButtons::Left);

    EXPECT_EQ(*entries, (std::vector<std::string> { "own", "MOUSE_BUTTON_PRESSED:a" }));
}

TEST_F(FormaManifestDriverTest, AButtonSelectsWhichPressRunsTheActionAndNoButtonSelectsAny)
{
    const auto entries = std::make_shared<std::vector<std::string>>();
    expose_log_action(actions, entries);
    ASSERT_TRUE(run_with_actions(action_document({ on_element("MOUSE_BUTTON_PRESSED", { { "button", "Right" } }) }))) << error;

    window->move_to_ndc({ 0.F, 0.F });
    window->press(IO::MouseButtons::Left);
    EXPECT_TRUE(entries->empty());

    window->press(IO::MouseButtons::Right);
    EXPECT_EQ(*entries, (std::vector<std::string> { "MOUSE_BUTTON_PRESSED:a" }));
}

TEST_F(FormaManifestDriverTest, ANoButtonPressActionRunsForEveryButton)
{
    const auto entries = std::make_shared<std::vector<std::string>>();
    expose_log_action(actions, entries);
    ASSERT_TRUE(run_with_actions(action_document({ on_element("MOUSE_BUTTON_PRESSED") }))) << error;

    window->move_to_ndc({ 0.F, 0.F });
    window->press(IO::MouseButtons::Left);
    window->press(IO::MouseButtons::Middle);

    EXPECT_EQ(entries->size(), 2U);
}

TEST_F(FormaManifestDriverTest, AKeyActionRunsForItsKeyWhileTheInstanceHasFocus)
{
    const auto entries = std::make_shared<std::vector<std::string>>();
    expose_log_action(actions, entries);
    ASSERT_TRUE(run_with_actions(action_document({ on_element("KEY_PRESSED", { { "key", "a" } }),
        on_element("KEY_RELEASED", { { "key", "a" } }) }))) << error;

    window->key_down(IO::Keys::A);
    EXPECT_TRUE(entries->empty());

    window->move_to_ndc({ 0.F, 0.F });
    window->press(IO::MouseButtons::Left);
    window->key_down(IO::Keys::A);
    window->key_down(IO::Keys::B);
    window->key_up(IO::Keys::A);

    EXPECT_EQ(*entries, (std::vector<std::string> { "KEY_PRESSED:a", "KEY_RELEASED:a" }));
}

TEST_F(FormaManifestDriverTest, ATextActionReceivesTheCodepointWhileTheInstanceHasFocus)
{
    std::vector<uint32_t> typed;
    actions.expose("log", Forma::Action([&](const Forma::ActionEvent& event) {
        if (const auto* data = std::get_if<Core::WindowEvent::TextData>(&event.event.data))
            typed.push_back(data->codepoint);
    }));
    ASSERT_TRUE(run_with_actions(action_document({ on_element("TEXT_INPUT") }))) << error;

    window->move_to_ndc({ 0.F, 0.F });
    window->press(IO::MouseButtons::Left);
    window->type(0x263A);

    EXPECT_EQ(typed, (std::vector<uint32_t> { 0x263A }));
}

TEST_F(FormaManifestDriverTest, FocusActionsRunWhenTheInstanceGainsAndLosesFocus)
{
    const auto entries = std::make_shared<std::vector<std::string>>();
    expose_log_action(actions, entries);
    ASSERT_TRUE(run_with_actions(action_document({ on_element("WINDOW_FOCUS_GAINED"),
        on_element("WINDOW_FOCUS_LOST") }, { { "half", "0.3" } }))) << error;

    window->move_to_ndc({ 0.F, 0.F });
    window->press(IO::MouseButtons::Left);
    window->move_to_ndc({ 0.9F, 0.9F });
    window->press(IO::MouseButtons::Left);

    EXPECT_EQ(*entries, (std::vector<std::string> { "WINDOW_FOCUS_GAINED:a", "WINDOW_FOCUS_LOST:a" }));
}

TEST_F(FormaManifestDriverTest, AnActionParameterIsDecodedThroughItsDescribedMembers)
{
    const auto notes = std::make_shared<std::vector<FormaManifestNote>>();
    Reflect::RecordBinding<FormaManifestNote, FormaManifestElement> binding;
    binding.required("text");
    actions.expose_typed<FormaManifestNote>("note", std::move(binding),
        [notes](const FormaManifestNote& note) {
            notes->push_back(note);
            return Forma::Action([](const Forma::ActionEvent&) { });
        });
    auto note = manifest_node("on", { { "event", "MOUSE_BUTTON_PRESSED" }, { "action", "note" }, { "text", "hello" }, { "times", "3" } });

    ASSERT_TRUE(run_with_actions(action_document({ note }))) << error;

    ASSERT_EQ(notes->size(), 1U);
    EXPECT_EQ(notes->front().text, "hello");
    EXPECT_EQ(notes->front().times, 3);
}

TEST_F(FormaManifestDriverTest, OnElementsAreCheckedBeforeAnythingIsPlaced)
{
    const auto entries = std::make_shared<std::vector<std::string>>();
    expose_log_action(actions, entries);

    expect_rejected(action_document({ on_element("MOUSE_BUTTON_PRESSED") }), "needs an action table, and none was given");

    auto without_event = action_document({ manifest_node("on", { { "action", "log" } }) });
    expect_rejected(without_event, "<on> needs an event attribute",
        without_event.children[1].children[0].children[0].row, true);

    expect_rejected(action_document({ on_element("press") }), "unsupported event press", std::nullopt, true);
    expect_rejected(action_document({ on_element("MOUSE_ENTERED", { { "button", "Left" } }) }),
        "takes a button only for MOUSE_BUTTON_PRESSED, MOUSE_BUTTON_RELEASED and MOUSE_MOTION", std::nullopt, true);
    expect_rejected(action_document({ on_element("MOUSE_BUTTON_PRESSED", { { "button", "Unknown" } }) }),
        "unsupported button Unknown", std::nullopt, true);
    expect_rejected(action_document({ on_element("MOUSE_BUTTON_PRESSED", { { "button", "Banana" } }) }),
        "unsupported button Banana", std::nullopt, true);
    expect_rejected(action_document({ on_element("MOUSE_BUTTON_PRESSED", { { "key", "a" } }) }),
        "takes a key only for KEY_PRESSED, KEY_RELEASED and KEY_REPEAT", std::nullopt, true);
    expect_rejected(action_document({ on_element("KEY_PRESSED", { { "key", "Banana" } }) }),
        "unsupported key Banana", std::nullopt, true);
    expect_rejected(action_document({ on_element("KEY_PRESSED") }),
        "needs a key attribute", std::nullopt, true);
    expect_rejected(action_document({ on_element("WINDOW_RESIZED") }),
        "is not delivered to an element", std::nullopt, true);
    expect_rejected(action_document({ manifest_node("on", { { "event", "MOUSE_BUTTON_PRESSED" } }) }),
        "<on> needs an action attribute", std::nullopt, true);
    expect_rejected(action_document({ manifest_node("on", { { "event", "MOUSE_BUTTON_PRESSED" }, { "action", "ghost" } }) }),
        "no registered action ghost", std::nullopt, true);
    expect_rejected(action_document({ manifest_node("legend") }),
        "<legend> is not allowed in <instance>", std::nullopt, true);
    EXPECT_EQ(log->decodes, 0);
}

TEST_F(FormaManifestDriverTest, AnActionParameterErrorIsReportedAtTheOnElementBeforePlacing)
{
    Reflect::RecordBinding<FormaManifestNote, FormaManifestElement> binding;
    binding.required("text");
    actions.expose_typed<FormaManifestNote>("note", std::move(binding),
        [](const FormaManifestNote&) { return Forma::Action([](const Forma::ActionEvent&) { }); });

    auto missing = action_document({ manifest_node("on", { { "event", "MOUSE_BUTTON_PRESSED" }, { "action", "note" } }) });
    expect_rejected(missing, "text", missing.children[1].children[0].children[0].row, true);

    auto unknown = action_document({ manifest_node("on",
        { { "event", "MOUSE_BUTTON_PRESSED" }, { "action", "note" }, { "text", "x" }, { "bogus", "1" } }) });
    expect_rejected(unknown, "unsupported attribute bogus", unknown.children[1].children[0].children[0].row, true);

    auto invalid = action_document({ manifest_node("on",
        { { "event", "MOUSE_BUTTON_PRESSED" }, { "action", "note" }, { "text", "x" }, { "times", "many" } }) });
    expect_rejected(invalid, "times", invalid.children[1].children[0].children[0].row, true);
}

TEST_F(FormaManifestDriverTest, TheRootMayDeclareNamespaces)
{
    auto declared = manifest_document();
    declared.attributes.emplace_back("xmlns", "urn:other");
    declared.attributes.emplace_back("xmlns:mf", "urn:mayaflux:forma");

    const auto placed = run(manifest_numbered(declared));

    ASSERT_TRUE(placed) << error;
    EXPECT_EQ(placed->size(), 3U);
}

TEST_F(FormaManifestDriverTest, AnAttributeThatOnlyLooksLikeANamespaceDeclarationIsRejected)
{
    auto lookalike = manifest_document();
    lookalike.attributes.emplace_back("xmlnsmf", "urn:mayaflux:forma");
    expect_rejected(manifest_numbered(lookalike), "unsupported attribute xmlnsmf", 0);
}

TEST_F(FormaManifestDriverTest, AnActionTableMayServeADocumentWithoutAnyOnElements)
{
    ASSERT_TRUE(run_with_actions(manifest_document())) << error;
    EXPECT_EQ(log->placed.size(), 3U);
}

// =============================================================================
// Static geometry: vertices
// =============================================================================

namespace {

    template <typename V>
    std::vector<V> read_vertices(const std::vector<uint8_t>& bytes)
    {
        std::vector<V> vertices(bytes.size() / sizeof(V));
        std::memcpy(vertices.data(), bytes.data(), vertices.size() * sizeof(V));
        return vertices;
    }

    Forma::RectPlacementInputs rect_placement(float x, float y, float width, float height)
    {
        return Forma::RectPlacementInputs { .rect = { x, y, width, height } };
    }

    Forma::VertexGeometryInputs triangle_input()
    {
        Forma::VertexGeometryInputs input;
        input.topology = PrimitiveTopology::TRIANGLE_LIST;
        input.vertices = {
            Kakshya::Vertex { .position = { 0.F, 0.F, 0.F }, .color = { 1.F, 0.F, 0.F, 1.F } },
            Kakshya::Vertex { .position = { 1.F, 0.F, 0.F }, .color = { 0.F, 1.F, 0.F, 1.F } },
            Kakshya::Vertex { .position = { 0.5F, 1.F, 0.25F }, .color = { 0.F, 0.F, 1.F, 1.F } },
        };
        return input;
    }

    void expect_bounds(const Forma::Element& element, glm::vec2 min, glm::vec2 max)
    {
        ASSERT_TRUE(element.bounds_hint);
        EXPECT_NEAR(element.bounds_hint->min.x, min.x, 1e-5F);
        EXPECT_NEAR(element.bounds_hint->min.y, min.y, 1e-5F);
        EXPECT_NEAR(element.bounds_hint->max.x, max.x, 1e-5F);
        EXPECT_NEAR(element.bounds_hint->max.y, max.y, 1e-5F);
    }

}

TEST(FormaManifestGeometryTest, VerticesAreMappedThroughTheRectAndKeepTheirOtherFields)
{
    std::string error;
    const auto form = Forma::vertex_geometry_form(triangle_input(),
        rect_placement(-0.5F, -0.25F, 1.F, 0.5F), error);
    ASSERT_TRUE(form) << error;

    EXPECT_EQ(form->topology, PrimitiveTopology::TRIANGLE_LIST);
    EXPECT_EQ(form->capacity, 3 * sizeof(Kakshya::Vertex));

    std::vector<uint8_t> bytes;
    Forma::Element element;
    form->geometry(std::monostate {}, bytes, element);
    const auto vertices = read_vertices<Kakshya::Vertex>(bytes);

    ASSERT_EQ(vertices.size(), 3U);
    EXPECT_EQ(vertices[0].position, glm::vec3(-0.5F, -0.25F, 0.F));
    EXPECT_EQ(vertices[1].position, glm::vec3(0.5F, -0.25F, 0.F));
    EXPECT_EQ(vertices[2].position, glm::vec3(0.F, 0.25F, 0.25F));
    EXPECT_EQ(vertices[1].color, glm::vec4(0.F, 1.F, 0.F, 1.F));
}

TEST(FormaManifestGeometryTest, TriangleVerticesHitInsideTheTrianglesOnly)
{
    std::string error;
    const auto form = Forma::vertex_geometry_form(triangle_input(),
        rect_placement(-0.5F, -0.25F, 1.F, 0.5F), error);
    ASSERT_TRUE(form) << error;

    std::vector<uint8_t> bytes;
    Forma::Element element;
    form->geometry(std::monostate {}, bytes, element);

    expect_bounds(element, { -0.5F, -0.25F }, { 0.5F, 0.25F });
    ASSERT_TRUE(element.contains);
    EXPECT_TRUE(element.contains({ 0.F, 0.F }));
    EXPECT_FALSE(element.contains({ 0.45F, 0.2F }));
}

TEST(FormaManifestGeometryTest, LineVerticesGetTheBoundingBoxAsTheirOnlyHitRegion)
{
    Forma::VertexGeometryInputs input;
    input.topology = PrimitiveTopology::LINE_LIST;
    input.vertices = {
        Kakshya::Vertex { .position = { 0.F, 0.F, 0.F } },
        Kakshya::Vertex { .position = { 1.F, 1.F, 0.F } },
    };

    std::string error;
    const auto form = Forma::vertex_geometry_form(input, rect_placement(0.F, 0.F, 2.F, 2.F), error);
    ASSERT_TRUE(form) << error;

    std::vector<uint8_t> bytes;
    Forma::Element element;
    form->geometry(std::monostate {}, bytes, element);

    expect_bounds(element, { 0.F, 0.F }, { 2.F, 2.F });
    EXPECT_FALSE(element.contains);
}

TEST(FormaManifestGeometryTest, ARectWithoutAreaIsRefused)
{
    std::string error;
    EXPECT_FALSE(Forma::vertex_geometry_form(triangle_input(), rect_placement(0.F, 0.F, 0.F, 1.F), error));
    EXPECT_NE(error.find("rect"), std::string::npos) << error;

    error.clear();
    EXPECT_FALSE(Forma::vertex_geometry_form(triangle_input(), rect_placement(0.F, 0.F, 1.F, -1.F), error));
    EXPECT_NE(error.find("rect"), std::string::npos) << error;
}

// =============================================================================
// Static geometry: polygons
// =============================================================================

namespace {

    Forma::PolygonInputs square_input()
    {
        Forma::PolygonInputs input;
        input.points = { { 0.F, 0.F }, { 1.F, 0.F }, { 1.F, 1.F }, { 0.F, 1.F } };
        return input;
    }

    Forma::PolygonHole inner_square()
    {
        return Forma::PolygonHole { .points = { { 0.3F, 0.3F }, { 0.7F, 0.3F }, { 0.7F, 0.7F }, { 0.3F, 0.7F } } };
    }

    std::optional<Forma::Geometry::Form<bool>> make_polygon(const Forma::PolygonInputs& input,
        const Forma::RectPlacementInputs& placement, std::string& error,
        const Forma::PolygonFactory& factory = {})
    {
        return factory(input, placement, error);
    }

    std::vector<uint8_t> draw(const Forma::Geometry::Form<bool>& form, bool on, Forma::Element& element)
    {
        std::vector<uint8_t> bytes;
        form.geometry(on, bytes, element);
        return bytes;
    }

}

TEST(FormaManifestGeometryTest, AFilledPolygonIsATriangleListWithTheCapacityOfItsTriangles)
{
    std::string error;
    const auto form = make_polygon(square_input(), rect_placement(-0.5F, -0.25F, 1.F, 0.5F), error);
    ASSERT_TRUE(form) << error;

    EXPECT_EQ(form->topology, PrimitiveTopology::TRIANGLE_LIST);
    EXPECT_EQ(form->capacity, 6 * sizeof(Kakshya::MeshVertex));

    Forma::Element element;
    const auto bytes = draw(*form, false, element);
    EXPECT_EQ(read_vertices<Kakshya::MeshVertex>(bytes).size(), 6U);
    expect_bounds(element, { -0.5F, -0.25F }, { 0.5F, 0.25F });
}

TEST(FormaManifestGeometryTest, TheActiveColorReplacesEveryVertexColorWhileTheStateIsTrue)
{
    auto input = square_input();
    input.color = { 0.1F, 0.2F, 0.3F, 1.F };
    input.active = glm::vec4(0.9F, 0.8F, 0.7F, 1.F);

    std::string error;
    const auto form = make_polygon(input, rect_placement(0.F, 0.F, 1.F, 1.F), error);
    ASSERT_TRUE(form) << error;

    Forma::Element element;
    for (const auto& vertex : read_vertices<Kakshya::MeshVertex>(draw(*form, false, element)))
        EXPECT_EQ(vertex.color, input.color);
    for (const auto& vertex : read_vertices<Kakshya::MeshVertex>(draw(*form, true, element)))
        EXPECT_EQ(vertex.color, *input.active);
}

TEST(FormaManifestGeometryTest, WithoutAnActiveColorTheStateLeavesTheDrawingUnchanged)
{
    std::string error;
    const auto form = make_polygon(square_input(), rect_placement(0.F, 0.F, 1.F, 1.F), error);
    ASSERT_TRUE(form) << error;

    Forma::Element element;
    EXPECT_EQ(draw(*form, false, element), draw(*form, true, element));
}

TEST(FormaManifestGeometryTest, PerPointColorsFollowTheirPoints)
{
    auto input = square_input();
    input.colors = { { 1.F, 0.F, 0.F, 1.F }, { 0.F, 1.F, 0.F, 1.F }, { 0.F, 0.F, 1.F, 1.F }, { 1.F, 1.F, 0.F, 1.F } };

    std::string error;
    const auto form = make_polygon(input, rect_placement(-1.F, -1.F, 2.F, 2.F), error);
    ASSERT_TRUE(form) << error;

    Forma::Element element;
    const std::array<glm::vec2, 4> corners { glm::vec2(-1.F, -1.F), glm::vec2(1.F, -1.F),
        glm::vec2(1.F, 1.F), glm::vec2(-1.F, 1.F) };
    for (const auto& vertex : read_vertices<Kakshya::MeshVertex>(draw(*form, false, element))) {
        bool matched = false;
        for (size_t i = 0; i < corners.size(); ++i) {
            if (glm::distance(glm::vec2(vertex.position), corners[i]) < 1e-5F) {
                EXPECT_EQ(vertex.color, input.colors[i]);
                matched = true;
            }
        }
        EXPECT_TRUE(matched);
    }
}

TEST(FormaManifestGeometryTest, AThicknessDrawsTheRingsAsALineLoopWithTheBoxAsTheirHitRegion)
{
    auto input = square_input();
    input.thickness = 2.F;

    std::string error;
    const auto form = make_polygon(input, rect_placement(-0.5F, -0.25F, 1.F, 0.5F), error);
    ASSERT_TRUE(form) << error;

    EXPECT_EQ(form->topology, PrimitiveTopology::LINE_LIST);
    EXPECT_EQ(form->capacity, 4 * 2 * sizeof(Kakshya::LineVertex));

    Forma::Element element;
    const auto lines = read_vertices<Kakshya::LineVertex>(draw(*form, false, element));
    ASSERT_EQ(lines.size(), 8U);
    for (const auto& line : lines)
        EXPECT_FLOAT_EQ(line.thickness, 2.F);
    expect_bounds(element, { -0.5F, -0.25F }, { 0.5F, 0.25F });
    EXPECT_FALSE(element.contains);
}

TEST(FormaManifestGeometryTest, AHoleIsCutOutOfTheHitRegion)
{
    auto input = square_input();
    input.holes.push_back(inner_square());

    std::string error;
    const auto form = make_polygon(input, rect_placement(-1.F, -1.F, 2.F, 2.F), error);
    ASSERT_TRUE(form) << error;

    Forma::Element element;
    (void)draw(*form, false, element);

    ASSERT_TRUE(element.contains);
    EXPECT_FALSE(element.contains({ 0.F, 0.F }));
    EXPECT_TRUE(element.contains({ -0.8F, -0.6F }));
    EXPECT_TRUE(element.contains({ 0.8F, 0.5F }));
}

TEST(FormaManifestGeometryTest, StrokeRingsIncludeTheHoleRing)
{
    auto input = square_input();
    input.thickness = 1.F;
    input.holes.push_back(inner_square());

    std::string error;
    const auto form = make_polygon(input, rect_placement(0.F, 0.F, 1.F, 1.F), error);
    ASSERT_TRUE(form) << error;

    Forma::Element element;
    EXPECT_EQ(read_vertices<Kakshya::LineVertex>(draw(*form, false, element)).size(), 16U);
}

TEST(FormaManifestGeometryTest, PolygonsThatAreNotValidAreRefusedWithAReason)
{
    const auto placement = rect_placement(0.F, 0.F, 1.F, 1.F);
    std::string error;

    auto few = square_input();
    few.points.resize(2);
    EXPECT_FALSE(make_polygon(few, placement, error));
    EXPECT_NE(error.find("three"), std::string::npos) << error;

    auto colors = square_input();
    colors.colors = { { 1.F, 1.F, 1.F, 1.F } };
    EXPECT_FALSE(make_polygon(colors, placement, error));
    EXPECT_NE(error.find("colors"), std::string::npos) << error;

    auto negative = square_input();
    negative.thickness = -1.F;
    EXPECT_FALSE(make_polygon(negative, placement, error));
    EXPECT_NE(error.find("thickness"), std::string::npos) << error;

    auto small_hole = square_input();
    small_hole.holes.push_back(Forma::PolygonHole { .points = { { 0.4F, 0.4F }, { 0.6F, 0.4F } } });
    EXPECT_FALSE(make_polygon(small_hole, placement, error));
    EXPECT_NE(error.find("hole"), std::string::npos) << error;

    auto bowtie = square_input();
    bowtie.points = { { 0.F, 0.F }, { 1.F, 1.F }, { 1.F, 0.F }, { 0.F, 1.F } };
    EXPECT_FALSE(make_polygon(bowtie, placement, error));
    EXPECT_NE(error.find("valid region"), std::string::npos) << error;

    EXPECT_FALSE(make_polygon(square_input(), rect_placement(0.F, 0.F, 0.F, 0.F), error));
    EXPECT_NE(error.find("rect"), std::string::npos) << error;
}

TEST(FormaManifestGeometryTest, HoleColorsMustMatchWhetherTheOutlineHasColors)
{
    const auto placement = rect_placement(0.F, 0.F, 1.F, 1.F);
    std::string error;

    auto uncolored_outline = square_input();
    auto colored_hole = inner_square();
    colored_hole.colors.assign(4, glm::vec4(1.F));
    uncolored_outline.holes.push_back(colored_hole);
    EXPECT_FALSE(make_polygon(uncolored_outline, placement, error));

    auto colored_outline = square_input();
    colored_outline.colors.assign(4, glm::vec4(1.F));
    colored_outline.holes.push_back(inner_square());
    EXPECT_FALSE(make_polygon(colored_outline, placement, error));
}

TEST(FormaManifestGeometryTest, APolygonResolvesANamedContainmentAgainstItsPlacedGeometry)
{
    size_t seen_positions = 0;
    PrimitiveTopology seen_topology = PrimitiveTopology::POINT_LIST;
    std::optional<AABB2D> seen_placement;

    Forma::PolygonFactory factory;
    factory.containments.expose("rect",
        [&](const Forma::ContainmentSource& source, std::string&) -> std::optional<Forma::Containment> {
            seen_positions = source.positions.size();
            seen_topology = source.topology;
            seen_placement = source.placement;
            return Forma::Containment { .bounds = source.placement };
        });
    auto input = square_input();
    input.containment = "rect";

    std::string error;
    const auto form = make_polygon(input, rect_placement(-0.5F, -0.25F, 1.F, 0.5F), error, factory);
    ASSERT_TRUE(form) << error;

    EXPECT_EQ(seen_positions, 6U);
    EXPECT_EQ(seen_topology, PrimitiveTopology::TRIANGLE_LIST);
    ASSERT_TRUE(seen_placement);
    EXPECT_NEAR(seen_placement->min.x, -0.5F, 1e-5F);
    EXPECT_NEAR(seen_placement->max.y, 0.25F, 1e-5F);

    Forma::Element element;
    (void)draw(*form, false, element);
    expect_bounds(element, { -0.5F, -0.25F }, { 0.5F, 0.25F });
    EXPECT_FALSE(element.contains);
}

TEST(FormaManifestGeometryTest, AnUnregisteredContainmentOrInteractionNameIsRefused)
{
    const auto placement = rect_placement(0.F, 0.F, 1.F, 1.F);
    std::string error;

    auto containment = square_input();
    containment.containment = "nowhere";
    EXPECT_FALSE(make_polygon(containment, placement, error));
    EXPECT_NE(error.find("nowhere"), std::string::npos) << error;

    auto interaction = square_input();
    interaction.interaction = "nothing";
    error.clear();
    EXPECT_FALSE(make_polygon(interaction, placement, error));
    EXPECT_NE(error.find("nothing"), std::string::npos) << error;
}

TEST(FormaManifestGeometryTest, ARegisteredInteractionBecomesTheFormsWire)
{
    Forma::PolygonFactory factory;
    factory.interactions.expose("flip", Forma::press_flip_interaction());
    auto input = square_input();
    input.interaction = "flip";

    std::string error;
    const auto wired = make_polygon(input, rect_placement(0.F, 0.F, 1.F, 1.F), error, factory);
    ASSERT_TRUE(wired) << error;
    EXPECT_TRUE(static_cast<bool>(wired->wire));

    const auto plain = make_polygon(square_input(), rect_placement(0.F, 0.F, 1.F, 1.F), error, factory);
    ASSERT_TRUE(plain) << error;
    EXPECT_FALSE(static_cast<bool>(plain->wire));
}

// =============================================================================
// Drag geometry: scale and handle path
// =============================================================================

namespace {

    Forma::ScaleInputs bar_input(bool horizontal = true)
    {
        Forma::ScaleInputs input;
        input.points = { { 0.F, 0.F }, { 1.F, 0.F }, { 1.F, 1.F }, { 0.F, 1.F } };
        input.horizontal = horizontal;
        return input;
    }

    Forma::HandlePathInputs curve_input()
    {
        Forma::HandlePathInputs input;
        input.points = { { 0.F, 0.F }, { 0.5F, 1.F }, { 1.F, 0.F } };
        return input;
    }

    std::pair<float, float> extent_along(const std::vector<Kakshya::MeshVertex>& vertices, bool x_axis)
    {
        float low = std::numeric_limits<float>::max();
        float high = std::numeric_limits<float>::lowest();
        for (const auto& vertex : vertices) {
            const float value = x_axis ? vertex.position.x : vertex.position.y;
            low = std::min(low, value);
            high = std::max(high, value);
        }
        return { low, high };
    }

}

TEST(FormaManifestDragGeometryTest, AScaleFormFillsTheOutlineWithTheCapacityOfItsTriangles)
{
    std::string error;
    const auto form = Forma::scale_form(bar_input(), rect_placement(-0.5F, -0.25F, 1.F, 0.5F), error);
    ASSERT_TRUE(form) << error;

    EXPECT_EQ(form->topology, PrimitiveTopology::TRIANGLE_LIST);
    EXPECT_EQ(form->capacity, 6 * sizeof(Kakshya::MeshVertex));
    EXPECT_TRUE(static_cast<bool>(form->wire));

    std::vector<uint8_t> bytes;
    Forma::Element element;
    form->geometry(1.F, bytes, element);
    EXPECT_EQ(read_vertices<Kakshya::MeshVertex>(bytes).size(), 6U);
}

TEST(FormaManifestDragGeometryTest, AHorizontalScaleGrowsFromTheLeftEdgeWithTheClampedState)
{
    std::string error;
    const auto form = Forma::scale_form(bar_input(true), rect_placement(-0.5F, -0.25F, 1.F, 0.5F), error);
    ASSERT_TRUE(form) << error;

    const auto extent = [&](float state, bool x_axis) {
        std::vector<uint8_t> bytes;
        Forma::Element element;
        form->geometry(state, bytes, element);
        return extent_along(read_vertices<Kakshya::MeshVertex>(bytes), x_axis);
    };

    const auto half = extent(0.5F, true);
    EXPECT_NEAR(half.first, -0.5F, 1e-5F);
    EXPECT_NEAR(half.second, 0.F, 1e-5F);
    EXPECT_NEAR(extent(2.F, true).second, 0.5F, 1e-5F);
    EXPECT_NEAR(extent(-1.F, true).second, -0.5F, 1e-5F);

    const auto height = extent(0.5F, false);
    EXPECT_NEAR(height.first, -0.25F, 1e-5F);
    EXPECT_NEAR(height.second, 0.25F, 1e-5F);
}

TEST(FormaManifestDragGeometryTest, AVerticalScaleGrowsFromTheBottomEdge)
{
    std::string error;
    const auto form = Forma::scale_form(bar_input(false), rect_placement(-0.5F, -0.25F, 1.F, 0.5F), error);
    ASSERT_TRUE(form) << error;

    std::vector<uint8_t> bytes;
    Forma::Element element;
    form->geometry(0.5F, bytes, element);
    const auto vertices = read_vertices<Kakshya::MeshVertex>(bytes);

    const auto vertical = extent_along(vertices, false);
    EXPECT_NEAR(vertical.first, -0.25F, 1e-5F);
    EXPECT_NEAR(vertical.second, 0.F, 1e-5F);
    const auto horizontal = extent_along(vertices, true);
    EXPECT_NEAR(horizontal.first, -0.5F, 1e-5F);
    EXPECT_NEAR(horizontal.second, 0.5F, 1e-5F);
}

TEST(FormaManifestDragGeometryTest, TheScaleHitRegionStaysThePlacementRectAtZero)
{
    std::string error;
    const auto form = Forma::scale_form(bar_input(), rect_placement(-0.5F, -0.25F, 1.F, 0.5F), error);
    ASSERT_TRUE(form) << error;

    std::vector<uint8_t> bytes;
    Forma::Element element;
    form->geometry(0.F, bytes, element);

    expect_bounds(element, { -0.5F, -0.25F }, { 0.5F, 0.25F });
    EXPECT_FALSE(static_cast<bool>(element.contains));
    EXPECT_TRUE(element.hits({ 0.4F, 0.1F }));
}

TEST(FormaManifestDragGeometryTest, ScaleOutlinesThatAreNotValidAreRefusedWithAReason)
{
    const auto placement = rect_placement(0.F, 0.F, 1.F, 1.F);
    std::string error;

    auto few = bar_input();
    few.points.resize(2);
    EXPECT_FALSE(Forma::scale_form(few, placement, error));
    EXPECT_NE(error.find("three"), std::string::npos) << error;

    auto bowtie = bar_input();
    bowtie.points = { { 0.F, 0.F }, { 1.F, 1.F }, { 1.F, 0.F }, { 0.F, 1.F } };
    EXPECT_FALSE(Forma::scale_form(bowtie, placement, error));
    EXPECT_NE(error.find("valid region"), std::string::npos) << error;

    EXPECT_FALSE(Forma::scale_form(bar_input(), rect_placement(0.F, 0.F, 0.F, 1.F), error));
    EXPECT_NE(error.find("rect"), std::string::npos) << error;
}

TEST(FormaManifestDragGeometryTest, AHandlePathFormCarriesItsCapacityAndWire)
{
    std::string error;
    const auto form = Forma::handle_path_form(curve_input(), rect_placement(-0.5F, -0.5F, 1.F, 1.F), error);
    ASSERT_TRUE(form) << error;

    EXPECT_EQ(form->topology, PrimitiveTopology::LINE_LIST);
    EXPECT_EQ(form->capacity, (2U * 2U + 3U * 8U) * sizeof(Kakshya::LineVertex));
    EXPECT_TRUE(static_cast<bool>(form->wire));
}

TEST(FormaManifestDragGeometryTest, AHandlePathHitRegionIsThePlacementRect)
{
    std::string error;
    const auto form = Forma::handle_path_form(curve_input(), rect_placement(-0.5F, -0.5F, 1.F, 1.F), error);
    ASSERT_TRUE(form) << error;

    std::vector<uint8_t> bytes;
    Forma::Element element;
    form->geometry(curve_input().points, bytes, element);

    expect_bounds(element, { -0.5F, -0.5F }, { 0.5F, 0.5F });
    EXPECT_FALSE(static_cast<bool>(element.contains));
}

TEST(FormaManifestDragGeometryTest, HandlePathsThatAreNotValidAreRefusedWithAReason)
{
    const auto placement = rect_placement(0.F, 0.F, 1.F, 1.F);
    std::string error;

    auto few = curve_input();
    few.points.resize(1);
    EXPECT_FALSE(Forma::handle_path_form(few, placement, error));
    EXPECT_NE(error.find("two"), std::string::npos) << error;

    auto outside = curve_input();
    outside.points[1] = { 0.5F, 1.5F };
    EXPECT_FALSE(Forma::handle_path_form(outside, placement, error));
    EXPECT_NE(error.find("unit square"), std::string::npos) << error;

    auto negative = curve_input();
    negative.thickness = -1.F;
    EXPECT_FALSE(Forma::handle_path_form(negative, placement, error));
    EXPECT_NE(error.find("thickness"), std::string::npos) << error;

    auto no_reach = curve_input();
    no_reach.grab_radius = 0.F;
    EXPECT_FALSE(Forma::handle_path_form(no_reach, placement, error));
    EXPECT_NE(error.find("grab_radius"), std::string::npos) << error;

    EXPECT_FALSE(Forma::handle_path_form(curve_input(), rect_placement(0.F, 0.F, 1.F, 0.F), error));
    EXPECT_NE(error.find("rect"), std::string::npos) << error;
}

class FormaManifestScaleWireTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::string error;
        const auto form = Forma::scale_form(bar_input(), rect_placement(-0.5F, -0.25F, 1.F, 0.5F), error);
        ASSERT_TRUE(form) << error;
        id = layer->add(Forma::Element {}.with_rect({ -0.5F, -0.25F }, { 0.5F, 0.25F }));
        form->wire(*ctx, id, state);
    }

    std::shared_ptr<Vruta::EventManager> manager = std::make_shared<Vruta::EventManager>();
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(200U, 100U);
    std::shared_ptr<Forma::Layer> layer = std::make_shared<Forma::Layer>();
    std::unique_ptr<Forma::Context> ctx = std::make_unique<Forma::Context>(layer, window, manager, "scale-wire");
    std::shared_ptr<Forma::MappedState<float>> state = std::make_shared<Forma::MappedState<float>>();
    uint32_t id {};
};

TEST_F(FormaManifestScaleWireTest, ADragWritesTheFractionOfTheRectUnderThePointer)
{
    window->move_to_ndc({ -0.4F, 0.F });
    window->press(IO::MouseButtons::Left);
    window->move_to_ndc({ 0.25F, 0.F });

    EXPECT_NEAR(state->value, 0.75F, 1e-3F);
}

TEST_F(FormaManifestScaleWireTest, ADragPastTheEndsStaysInTheUnitRange)
{
    window->move_to_ndc({ 0.F, 0.F });
    window->press(IO::MouseButtons::Left);

    window->move_to_ndc({ 0.9F, 0.F });
    EXPECT_NEAR(state->value, 1.F, 1e-5F);

    window->move_to_ndc({ -0.9F, 0.F });
    EXPECT_NEAR(state->value, 0.F, 1e-5F);
}

TEST_F(FormaManifestDriverTest, AHandlePathPointOutsideTheUnitSquareFailsBeforeAnythingIsPlaced)
{
    Forma::expose_handle_path(table, "path");
    const auto document = manifest_numbered(manifest_node("forma", { { "version", "1" } },
        {
            manifest_node("templates", {},
                { manifest_node("template", { { "id", "curve" } },
                    { manifest_node("path", { { "points", "0 0 2 1" } }) }) }),
            manifest_node("surface", { { "id", "main" } },
                { manifest_node("instance", { { "id", "line" }, { "template", "curve" }, { "rect", "-0.5 0 1 0.5" } }) }),
        }));

    EXPECT_FALSE(run(document));
    EXPECT_NE(error.find("unit square"), std::string::npos) << error;
    EXPECT_FALSE(surfaces.at("main").layer().find("line").has_value());
}

// =============================================================================
// Containment
// =============================================================================

TEST(FormaManifestContainmentTest, NoPositionsGiveNoRegion)
{
    const auto hit = Forma::containment_of({}, PrimitiveTopology::TRIANGLE_LIST);

    EXPECT_FALSE(hit.bounds);
    EXPECT_FALSE(hit.contains);
}

TEST(FormaManifestContainmentTest, ATriangleListHitsInsideItsTrianglesWithinTheirBox)
{
    const std::vector<glm::vec2> positions { { 0.F, 0.F }, { 2.F, 0.F }, { 0.F, 2.F } };

    const auto hit = Forma::containment_of(positions, PrimitiveTopology::TRIANGLE_LIST);

    ASSERT_TRUE(hit.bounds);
    EXPECT_EQ(hit.bounds->min, glm::vec2(0.F, 0.F));
    EXPECT_EQ(hit.bounds->max, glm::vec2(2.F, 2.F));
    ASSERT_TRUE(hit.contains);
    EXPECT_TRUE(hit.contains({ 0.5F, 0.5F }));
    EXPECT_FALSE(hit.contains({ 1.8F, 1.8F }));
}

TEST(FormaManifestContainmentTest, StripsAndFansAreExpandedIntoTriangles)
{
    const std::vector<glm::vec2> strip { { 0.F, 0.F }, { 1.F, 0.F }, { 0.F, 1.F }, { 1.F, 1.F } };
    const auto strip_hit = Forma::containment_of(strip, PrimitiveTopology::TRIANGLE_STRIP);
    ASSERT_TRUE(strip_hit.contains);
    EXPECT_TRUE(strip_hit.contains({ 0.5F, 0.5F }));
    EXPECT_FALSE(strip_hit.contains({ 2.F, 2.F }));

    const std::vector<glm::vec2> fan { { 0.F, 0.F }, { 1.F, 0.F }, { 1.F, 1.F }, { 0.F, 1.F } };
    const auto fan_hit = Forma::containment_of(fan, PrimitiveTopology::TRIANGLE_FAN);
    ASSERT_TRUE(fan_hit.contains);
    EXPECT_TRUE(fan_hit.contains({ 0.5F, 0.5F }));
    EXPECT_FALSE(fan_hit.contains({ -0.5F, -0.5F }));
}

TEST(FormaManifestContainmentTest, NonTriangleTopologiesGetOnlyTheBox)
{
    const std::vector<glm::vec2> positions { { -1.F, -1.F }, { 1.F, 1.F } };

    const auto hit = Forma::containment_of(positions, PrimitiveTopology::LINE_LIST);

    ASSERT_TRUE(hit.bounds);
    EXPECT_EQ(hit.bounds->min, glm::vec2(-1.F, -1.F));
    EXPECT_FALSE(hit.contains);
}

TEST(FormaManifestContainmentTest, ApplyingWritesBoundsAndContainsAndClearsAStalePredicate)
{
    Forma::Element element;
    element.contains = [](glm::vec2) { return true; };
    const AABB2D box { .min = { 0.F, 0.F }, .max = { 1.F, 1.F } };

    Forma::apply_containment(element, Forma::rect_containment(box));

    ASSERT_TRUE(element.bounds_hint);
    EXPECT_EQ(element.bounds_hint->max, glm::vec2(1.F, 1.F));
    EXPECT_FALSE(element.contains);
}

TEST(FormaManifestContainmentTest, ApplyingKeepsAClipSetByAContainerUnlessTheRegionCarriesOne)
{
    Forma::Element element;
    const AABB2D viewport { .min = { -1.F, -1.F }, .max = { 1.F, 1.F } };
    element.clip = viewport;

    Forma::apply_containment(element, Forma::rect_containment({ .min = { 0.F, 0.F }, .max = { 1.F, 1.F } }));
    ASSERT_TRUE(element.clip);
    EXPECT_EQ(element.clip->min, viewport.min);

    const AABB2D tighter { .min = { 0.F, 0.F }, .max = { 0.5F, 0.5F } };
    Forma::Containment with_clip;
    with_clip.clip = tighter;
    Forma::apply_containment(element, with_clip);
    ASSERT_TRUE(element.clip);
    EXPECT_EQ(element.clip->max, tighter.max);
}

TEST(FormaManifestContainmentTest, TheTableResolvesRegisteredNamesAndReportsUnknownOnes)
{
    Forma::ContainmentTable table;
    table.expose("all", [](const Forma::ContainmentSource&, std::string&) -> std::optional<Forma::Containment> {
        return Forma::rect_containment({ .min = { 0.F, 0.F }, .max = { 1.F, 1.F } });
    });

    std::string error;
    const auto found = table.resolve("all", {}, error);
    ASSERT_TRUE(found) << error;
    EXPECT_TRUE(found->bounds);

    EXPECT_FALSE(table.resolve("none", {}, error));
    EXPECT_NE(error.find("none"), std::string::npos) << error;
    EXPECT_TRUE(table.contains("all"));
    EXPECT_EQ(table.names(), (std::vector<std::string> { "all" }));
}

TEST(FormaManifestContainmentTest, TablesMergeUnderAPrefixAndRefuseCollisions)
{
    Forma::ContainmentTable first;
    first.expose("all", [](const Forma::ContainmentSource&, std::string&) -> std::optional<Forma::Containment> {
        return Forma::Containment {};
    });
    Forma::ContainmentTable second;

    std::string error;
    ASSERT_TRUE(second.merge(first, "a.", error)) << error;
    EXPECT_TRUE(second.contains("a.all"));

    EXPECT_FALSE(second.merge(first, "a.", error));
    EXPECT_NE(error.find("a.all"), std::string::npos) << error;
    EXPECT_TRUE(second.merge(first, "a.", error, true)) << error;
}

// =============================================================================
// Interaction
// =============================================================================

TEST(FormaManifestInteractionTest, TheTableResolvesRegisteredNamesAndReportsUnknownOnes)
{
    Forma::InteractionTable<bool> table;
    table.expose("flip", Forma::press_flip_interaction());

    std::string error;
    const auto found = table.resolve("flip", {}, error);
    ASSERT_TRUE(found) << error;
    EXPECT_TRUE(static_cast<bool>(*found));

    EXPECT_FALSE(table.resolve("none", {}, error));
    EXPECT_NE(error.find("none"), std::string::npos) << error;
    EXPECT_TRUE(table.contains("flip"));
    EXPECT_EQ(table.names(), (std::vector<std::string> { "flip" }));
}

TEST(FormaManifestInteractionTest, ProvidersReceiveThePlacementBounds)
{
    std::optional<AABB2D> seen;
    Forma::InteractionTable<float> table;
    table.expose("probe",
        [&seen](const Forma::InteractionSource& source, std::string&) -> std::optional<Forma::InteractionFn<float>> {
            seen = source.placement;
            return Forma::InteractionFn<float> {};
        });
    const AABB2D bounds { .min = { -1.F, -1.F }, .max = { 1.F, 1.F } };

    std::string error;
    ASSERT_TRUE(table.resolve("probe", Forma::InteractionSource { .placement = bounds }, error)) << error;

    ASSERT_TRUE(seen);
    EXPECT_EQ(seen->min, bounds.min);
    EXPECT_EQ(seen->max, bounds.max);
}

TEST(FormaManifestInteractionTest, TablesMergeUnderAPrefixAndRefuseCollisions)
{
    Forma::InteractionTable<bool> first;
    first.expose("flip", Forma::press_flip_interaction());
    Forma::InteractionTable<bool> second;

    std::string error;
    ASSERT_TRUE(second.merge(first, "ui.", error)) << error;
    EXPECT_TRUE(second.contains("ui.flip"));

    EXPECT_FALSE(second.merge(first, "ui.", error));
    EXPECT_NE(error.find("ui.flip"), std::string::npos) << error;
}

TEST(FormaManifestInteractionTest, AttachLeavesTheFormAloneForAnEmptyName)
{
    const Forma::InteractionTable<bool> table;
    Forma::Geometry::Form<bool> form;

    std::string error;
    EXPECT_TRUE(table.attach(form, "", std::nullopt, error));

    EXPECT_FALSE(static_cast<bool>(form.wire));
}

TEST(FormaManifestInteractionTest, AttachSetsTheWireFromARegisteredName)
{
    Forma::InteractionTable<bool> table;
    table.expose("flip", Forma::press_flip_interaction());
    Forma::Geometry::Form<bool> form;

    std::string error;
    EXPECT_TRUE(table.attach(form, "flip", std::nullopt, error)) << error;

    EXPECT_TRUE(static_cast<bool>(form.wire));
}

TEST(FormaManifestInteractionTest, AttachReportsAnUnknownNameAndLeavesTheFormUnwired)
{
    Forma::InteractionTable<bool> table;
    Forma::Geometry::Form<bool> form;

    std::string error;
    EXPECT_FALSE(table.attach(form, "none", std::nullopt, error));

    EXPECT_NE(error.find("none"), std::string::npos) << error;
    EXPECT_FALSE(static_cast<bool>(form.wire));
}

TEST(FormaManifestInteractionTest, AttachPassesThePlacementBoundsToTheProvider)
{
    std::optional<AABB2D> seen;
    Forma::InteractionTable<float> table;
    table.expose("probe",
        [&seen](const Forma::InteractionSource& source, std::string&) -> std::optional<Forma::InteractionFn<float>> {
            seen = source.placement;
            return Forma::InteractionFn<float> {};
        });
    Forma::Geometry::Form<float> form;
    const AABB2D bounds { .min = { 0.F, 0.F }, .max = { 2.F, 3.F } };

    std::string error;
    ASSERT_TRUE(table.attach(form, "probe", bounds, error)) << error;

    ASSERT_TRUE(seen);
    EXPECT_EQ(seen->max, bounds.max);
}

TEST(FormaManifestInteractionTest, AFixedInteractionIgnoresThePlacement)
{
    const auto provider = Forma::fixed_interaction<float>(Forma::InteractionFn<float>(
        [](Forma::Context&, uint32_t, const std::shared_ptr<Forma::MappedState<float>>&) { }));
    const AABB2D bounds { .min = { 0.F, 0.F }, .max = { 1.F, 1.F } };

    std::string error;
    const auto without = provider(Forma::InteractionSource {}, error);
    const auto with = provider(Forma::InteractionSource { .placement = bounds }, error);

    ASSERT_TRUE(without) << error;
    ASSERT_TRUE(with) << error;
    EXPECT_TRUE(static_cast<bool>(*without));
    EXPECT_TRUE(static_cast<bool>(*with));
}

TEST(FormaManifestInteractionTest, ABoundedInteractionReportsMissingBoundsAndOtherwiseReceivesThem)
{
    std::optional<AABB2D> seen;
    const auto provider = Forma::bounded_interaction<float>(
        [&seen](const AABB2D& bounds) {
            seen = bounds;
            return Forma::InteractionFn<float> {};
        });

    std::string error;
    EXPECT_FALSE(provider(Forma::InteractionSource {}, error));
    EXPECT_NE(error.find("placement bounds"), std::string::npos) << error;
    EXPECT_FALSE(seen);

    const AABB2D bounds { .min = { -1.F, -2.F }, .max = { 1.F, 2.F } };
    ASSERT_TRUE(provider(Forma::InteractionSource { .placement = bounds }, error)) << error;
    ASSERT_TRUE(seen);
    EXPECT_EQ(seen->min, bounds.min);
}

TEST(FormaManifestContainmentTest, ResolveOrUsesTheFallbackForAnEmptyName)
{
    const Forma::ContainmentTable table;
    const auto fallback = Forma::rect_containment({ .min = { 0.F, 0.F }, .max = { 1.F, 1.F } });

    std::string error;
    const auto hit = table.resolve_or("", {}, fallback, error);

    ASSERT_TRUE(hit) << error;
    ASSERT_TRUE(hit->bounds);
    EXPECT_EQ(hit->bounds->max, glm::vec2(1.F, 1.F));
}

TEST(FormaManifestContainmentTest, ResolveOrRunsANamedProviderAndReportsAnUnknownOne)
{
    Forma::ContainmentTable table;
    table.expose("wide", [](const Forma::ContainmentSource&, std::string&) -> std::optional<Forma::Containment> {
        return Forma::rect_containment({ .min = { 0.F, 0.F }, .max = { 2.F, 2.F } });
    });
    const auto fallback = Forma::rect_containment({ .min = { 0.F, 0.F }, .max = { 1.F, 1.F } });

    std::string error;
    const auto named = table.resolve_or("wide", {}, fallback, error);
    ASSERT_TRUE(named) << error;
    EXPECT_EQ(named->bounds->max, glm::vec2(2.F, 2.F));

    EXPECT_FALSE(table.resolve_or("none", {}, fallback, error));
    EXPECT_NE(error.find("none"), std::string::npos) << error;
}

TEST(FormaManifestContainmentTest, GeometryWithContainmentDrawsTheValueAndWritesTheRegionEachTime)
{
    std::vector<float> seen;
    const AABB2D box { .min = { 0.F, 0.F }, .max = { 1.F, 1.F } };
    const auto fn = Forma::geometry_with_containment<float>(
        [&seen](const float& value, std::vector<uint8_t>& out) {
            seen.push_back(value);
            out.assign(2, 7);
        },
        Forma::rect_containment(box));

    Forma::Element element;
    std::vector<uint8_t> bytes;
    fn(1.5F, bytes, element);
    EXPECT_EQ(bytes, (std::vector<uint8_t> { 7, 7 }));
    ASSERT_TRUE(element.bounds_hint);
    EXPECT_EQ(element.bounds_hint->max, box.max);

    element.bounds_hint.reset();
    fn(2.5F, bytes, element);
    EXPECT_EQ(seen, (std::vector<float> { 1.5F, 2.5F }));
    EXPECT_TRUE(element.bounds_hint);
}

// =============================================================================
// Placements
// =============================================================================

TEST(FormaManifestPlacementTest, ARectPlacementCoversItsOriginAndSize)
{
    std::string error;
    const auto region = Forma::region_of(rect_placement(-0.5F, -0.25F, 1.F, 0.5F), error);

    ASSERT_TRUE(region) << error;
    EXPECT_EQ(region->min, glm::vec2(-0.5F, -0.25F));
    EXPECT_EQ(region->max, glm::vec2(0.5F, 0.25F));
}

TEST(FormaManifestPlacementTest, ARectWithoutAreaIsRefusedWithItsReason)
{
    std::string error;

    EXPECT_FALSE(Forma::region_of(rect_placement(0.F, 0.F, 0.F, 1.F), error));
    EXPECT_EQ(error, "rect needs a positive width and height");

    error.clear();
    EXPECT_FALSE(Forma::region_of(rect_placement(0.F, 0.F, 1.F, -1.F), error));
    EXPECT_EQ(error, "rect needs a positive width and height");
}

TEST(FormaManifestPlacementTest, ARadialPlacementCoversTheBoxAroundItsCircle)
{
    std::string error;
    const auto region = Forma::region_of(
        Forma::RadialPlacementInputs { .center = { 0.5F, -0.5F }, .radius = 0.25F }, error);

    ASSERT_TRUE(region) << error;
    EXPECT_EQ(region->min, glm::vec2(0.25F, -0.75F));
    EXPECT_EQ(region->max, glm::vec2(0.75F, -0.25F));
}

TEST(FormaManifestPlacementTest, ARadialPlacementNeedsAPositiveRadius)
{
    std::string error;

    EXPECT_FALSE(Forma::region_of(Forma::RadialPlacementInputs {}, error));
    EXPECT_NE(error.find("radius"), std::string::npos) << error;

    error.clear();
    EXPECT_FALSE(Forma::region_of(Forma::RadialPlacementInputs { .radius = -1.F }, error));
    EXPECT_NE(error.find("radius"), std::string::npos) << error;
}

TEST(FormaManifestPlacementTest, TheRectBindingRequiresRectAndDecodesIt)
{
    const auto binding = Forma::rect_placement_binding<FormaManifestElement>();

    std::string error;
    EXPECT_FALSE(binding.read(manifest_node("instance"), error));
    EXPECT_NE(error.find("rect"), std::string::npos) << error;

    const auto decoded = binding.read(manifest_node("instance", { { "rect", "1 2 3 4" } }), error);
    ASSERT_TRUE(decoded) << error;
    EXPECT_EQ(decoded->rect, glm::vec4(1.F, 2.F, 3.F, 4.F));
}

TEST(FormaManifestPlacementTest, TheRadialBindingRequiresCenterAndRadiusAndDecodesThem)
{
    const auto binding = Forma::radial_placement_binding<FormaManifestElement>();

    std::string error;
    EXPECT_FALSE(binding.read(manifest_node("instance", { { "center", "1 2" } }), error));
    EXPECT_NE(error.find("radius"), std::string::npos) << error;

    error.clear();
    EXPECT_FALSE(binding.read(manifest_node("instance", { { "radius", "3" } }), error));
    EXPECT_NE(error.find("center"), std::string::npos) << error;

    const auto decoded = binding.read(
        manifest_node("instance", { { "center", "1 2" }, { "radius", "3" } }), error);
    ASSERT_TRUE(decoded) << error;
    EXPECT_EQ(decoded->center, glm::vec2(1.F, 2.F));
    EXPECT_FLOAT_EQ(decoded->radius, 3.F);
}

// =============================================================================
// Initial state from a member
// =============================================================================

TEST(FormaManifestTableTest, AMemberPointerOfTheInputOrThePlacementCanSupplyTheInitialState)
{
    Reflect::RecordBinding<FormaManifestProbe, FormaManifestElement> input;
    Reflect::RecordBinding<FormaManifestSpot, FormaManifestElement> placement;
    const auto make_form = [](const FormaManifestProbe&, const FormaManifestSpot&,
                               std::string&) -> std::optional<Forma::Geometry::Form<float>> {
        return Forma::Geometry::Form<float> {
            [](float, std::vector<uint8_t>& out, Forma::Element&) { out.assign(4, 0); },
            PrimitiveTopology::TRIANGLE_LIST,
            64,
        };
    };

    ProbeTable table;
    table.expose_typed<float>("from_input", input, placement, make_form, &FormaManifestProbe::gain);
    table.expose_typed<float>("from_placement", input, placement, make_form, &FormaManifestSpot::scale);

    const auto pair = make_probe_pair({ { "gain", "0.5" } }, { { "at", "0 0" }, { "scale", "2" } });
    std::string error;
    for (const char* name : { "from_input", "from_placement" }) {
        const auto* instantiator = table.find(name);
        ASSERT_NE(instantiator, nullptr) << name;
        EXPECT_TRUE(std::invoke(*instantiator, pair.factory, pair.instance, error)) << name << ": " << error;
    }
}

// =============================================================================
// ActionTable
// =============================================================================

TEST(FormaManifestActionTableTest, AnActionWithoutParametersRejectsAnyExtraAttribute)
{
    Forma::ActionTable<FormaManifestElement> actions;
    actions.expose("quiet", Forma::Action([](const Forma::ActionEvent&) { }));
    const auto* make = actions.find("quiet");
    ASSERT_NE(make, nullptr);
    auto on = manifest_node("on", { { "event", "MOUSE_BUTTON_PRESSED" }, { "button", "Left" }, { "action", "quiet" } });
    on.row = 5;

    std::string error;
    EXPECT_TRUE(std::invoke(*make, on, error)) << error;

    on.attributes.emplace_back("message", "x");
    EXPECT_FALSE(std::invoke(*make, on, error));
    EXPECT_EQ(error, "row 5: <on> has an unsupported attribute message");
}

TEST(FormaManifestActionTableTest, ATypedActionDecodesItsParametersAndReportsErrorsAtTheOnRow)
{
    std::vector<FormaManifestNote> made;
    Reflect::RecordBinding<FormaManifestNote, FormaManifestElement> binding;
    binding.required("text");
    Forma::ActionTable<FormaManifestElement> actions;
    actions.expose_typed<FormaManifestNote>("note", binding, [&made](const FormaManifestNote& note) {
        made.push_back(note);
        return Forma::Action([](const Forma::ActionEvent&) { });
    });
    const auto* make = actions.find("note");
    ASSERT_NE(make, nullptr);

    auto on = manifest_node("on", { { "event", "MOUSE_MOTION" }, { "button", "Left" }, { "action", "note" }, { "text", "hi" } });
    on.row = 9;
    std::string error;
    ASSERT_TRUE(std::invoke(*make, on, error)) << error;
    ASSERT_EQ(made.size(), 1U);
    EXPECT_EQ(made[0].text, "hi");
    EXPECT_EQ(made[0].times, 1);

    on.attributes.pop_back();
    EXPECT_FALSE(std::invoke(*make, on, error));
    EXPECT_TRUE(error.starts_with(row_prefix(9))) << error;
    EXPECT_NE(error.find("text"), std::string::npos) << error;
    EXPECT_EQ(made.size(), 1U);
}

TEST(FormaManifestActionTableTest, NamesAreSortedAndTablesMergeUnderAPrefix)
{
    Forma::ActionTable<FormaManifestElement> first;
    first.expose("b", Forma::Action([](const Forma::ActionEvent&) { }));
    first.expose("a", Forma::Action([](const Forma::ActionEvent&) { }));
    EXPECT_EQ(first.names(), (std::vector<std::string> { "a", "b" }));
    EXPECT_TRUE(first.contains("a"));
    EXPECT_FALSE(first.contains("c"));
    EXPECT_EQ(first.find("c"), nullptr);

    Forma::ActionTable<FormaManifestElement> second;
    std::string error;
    ASSERT_TRUE(second.merge(first, "ui.", error)) << error;
    EXPECT_TRUE(second.contains("ui.a"));
    EXPECT_FALSE(second.merge(first, "ui.", error));
    EXPECT_NE(error.find("ui.a"), std::string::npos) << error;
    EXPECT_TRUE(second.merge(first, "ui.", error, true)) << error;
}

// =============================================================================
// CallTable: described functions called by name
// =============================================================================

namespace {

    using CallAttributes = std::vector<std::pair<std::string, std::string>>;
    using TestCallTable = Forma::CallTable<FormaManifestElement>;

    TestCallTable make_call_table()
    {
        TestCallTable calls;
        calls.expose_form<&forma_call_fader>();
        calls.expose_form<&forma_call_triple>();
        calls.expose_form<&forma_call_samples>();
        calls.expose_form<&forma_call_flag>();
        return calls;
    }

    FormaManifestElement call_form(std::string call, CallAttributes extra = {})
    {
        CallAttributes attributes { { "call", std::move(call) } };
        attributes.insert(attributes.end(), extra.begin(), extra.end());
        return manifest_node("form", std::move(attributes));
    }

    std::optional<TestCallTable::Placer> make_placer(const TestCallTable& calls, const std::string& name,
        const FormaManifestElement& form, const FormaManifestElement& element, std::string& error)
    {
        const TestCallTable::Maker* make = calls.find(name);
        if (make == nullptr) {
            error = "no registered call " + name;
            return std::nullopt;
        }
        return std::invoke(*make, form, element, Forma::ElementHit {}, error);
    }

    const std::string k_fader_bounds = "-0.5 -0.1 0.5 0.1";

}

TEST(FormaManifestCallTableTest, AFunctionRegistersUnderItsOwnNameOrOneGiven)
{
    TestCallTable calls;
    calls.expose_form<&forma_call_fader>();
    calls.expose_form<&forma_call_flag>("lamp");

    EXPECT_TRUE(calls.contains("forma_call_fader"));
    EXPECT_TRUE(calls.contains("lamp"));
    EXPECT_FALSE(calls.contains("forma_call_flag"));
    EXPECT_EQ(calls.names(), (std::vector<std::string> { "forma_call_fader", "lamp" }));
}

TEST(FormaManifestCallTableTest, AttributesAreDecodedByParameterNameAndTheFunctionsDefaultsApply)
{
    const auto calls = make_call_table();
    forma_call_log().clear();

    std::string error;
    const auto placer = make_placer(calls, "forma_call_fader",
        call_form("forma_call_fader", { { "bounds", k_fader_bounds }, { "handle_w", "0.04" } }),
        manifest_node("element", { { "name", "f" } }), error);

    ASSERT_TRUE(placer) << error;
    ASSERT_EQ(forma_call_log().size(), 1U);
    EXPECT_EQ(forma_call_log()[0], "fader -0.500000 0.100000 0.040000 0.300000 plain");
}

TEST(FormaManifestCallTableTest, AnArgumentGivenAfterTheRequiredOnesReplacesItsDefault)
{
    const auto calls = make_call_table();
    forma_call_log().clear();

    std::string error;
    ASSERT_TRUE(make_placer(calls, "forma_call_fader",
        call_form("forma_call_fader", { { "bounds", k_fader_bounds }, { "handle_w", "0.04" }, { "track", "0.7 0 0" } }),
        manifest_node("element", { { "name", "f" } }), error))
        << error;

    EXPECT_EQ(forma_call_log().back(), "fader -0.500000 0.100000 0.040000 0.700000 plain");
}

TEST(FormaManifestCallTableTest, ALaterParameterCannotBeGivenWhileAnEarlierOneIsSkipped)
{
    const auto calls = make_call_table();
    forma_call_log().clear();

    std::string error;
    auto form = call_form("forma_call_triple", { { "a", "1" }, { "c", "3" } });
    form.row = 6;
    EXPECT_FALSE(make_placer(calls, "forma_call_triple", form, manifest_node("element"), error));
    EXPECT_TRUE(error.starts_with(row_prefix(6))) << error;
    EXPECT_NE(error.find("c is given without b"), std::string::npos) << error;
    EXPECT_TRUE(forma_call_log().empty());

    error.clear();
    EXPECT_TRUE(make_placer(calls, "forma_call_triple", call_form("forma_call_triple", { { "a", "1" }, { "b", "5" } }),
        manifest_node("element"), error))
        << error;
    EXPECT_EQ(forma_call_log().back(), "triple 1.000000 5.000000 2.000000");
}

TEST(FormaManifestCallTableTest, ARequiredParameterThatIsMissingIsNamed)
{
    const auto calls = make_call_table();

    std::string error;
    EXPECT_FALSE(make_placer(calls, "forma_call_fader",
        call_form("forma_call_fader", { { "bounds", k_fader_bounds } }), manifest_node("element"), error));
    EXPECT_NE(error.find("forma_call_fader needs handle_w"), std::string::npos) << error;

    error.clear();
    EXPECT_FALSE(make_placer(calls, "forma_call_samples", call_form("forma_call_samples"), manifest_node("element"), error));
    EXPECT_NE(error.find("needs count"), std::string::npos) << error;
}

TEST(FormaManifestCallTableTest, AnAttributeThatIsNoParameterIsRejectedWithItsRow)
{
    const auto calls = make_call_table();

    std::string error;
    auto form = call_form("forma_call_flag", { { "bogus", "1" } });
    form.row = 9;
    EXPECT_FALSE(make_placer(calls, "forma_call_flag", form, manifest_node("element"), error));
    EXPECT_TRUE(error.starts_with(row_prefix(9))) << error;
    EXPECT_NE(error.find("unsupported attribute bogus"), std::string::npos) << error;
}

TEST(FormaManifestCallTableTest, AValueThatDoesNotDecodeNamesTheParameterAndItsRow)
{
    const auto calls = make_call_table();

    std::string error;
    auto form = call_form("forma_call_fader", { { "bounds", k_fader_bounds }, { "handle_w", "wide" } });
    form.row = 4;
    EXPECT_FALSE(make_placer(calls, "forma_call_fader", form, manifest_node("element"), error));
    EXPECT_TRUE(error.starts_with(row_prefix(4))) << error;
    EXPECT_NE(error.find("invalid handle_w of wide"), std::string::npos) << error;

    error.clear();
    EXPECT_FALSE(make_placer(calls, "forma_call_fader",
        call_form("forma_call_fader", { { "bounds", "0 0 1" }, { "handle_w", "0.04" } }), manifest_node("element"), error));
    EXPECT_NE(error.find("invalid bounds"), std::string::npos) << error;
}

TEST(FormaManifestCallTableTest, AParameterThatHasNoTextFormCannotBeWrittenInADocument)
{
    const auto calls = make_call_table();

    std::string error;
    EXPECT_FALSE(make_placer(calls, "forma_call_fader",
        call_form("forma_call_fader",
            { { "bounds", k_fader_bounds }, { "handle_w", "0.04" }, { "track", "0.7 0 0" }, { "live_offset", "1" } }),
        manifest_node("element"), error));
    EXPECT_NE(error.find("forma_call_fader cannot be written in a document"), std::string::npos) << error;
}

TEST(FormaManifestCallTableTest, AFunctionWithNoParametersTakesNone)
{
    const auto calls = make_call_table();
    forma_call_log().clear();

    std::string error;
    EXPECT_TRUE(make_placer(calls, "forma_call_flag", call_form("forma_call_flag"), manifest_node("element"), error)) << error;
    EXPECT_EQ(forma_call_log(), (std::vector<std::string> { "flag" }));
}

TEST(FormaManifestCallTableTest, TheStateIsDecodedAsTheTypeTheFormReturns)
{
    const auto calls = make_call_table();

    std::string error;
    EXPECT_TRUE(make_placer(calls, "forma_call_samples", call_form("forma_call_samples", { { "count", "3" } }),
        manifest_node("element", { { "name", "s" }, { "state", "0.1 0.2 0.3" } }), error))
        << error;

    EXPECT_FALSE(make_placer(calls, "forma_call_samples", call_form("forma_call_samples", { { "count", "3" } }),
        manifest_node("element", { { "name", "s" }, { "state", "0.1 many" } }), error));
    EXPECT_NE(error.find("invalid state of 0.1 many"), std::string::npos) << error;

    error.clear();
    EXPECT_FALSE(make_placer(calls, "forma_call_flag", call_form("forma_call_flag"),
        manifest_node("element", { { "name", "l" }, { "state", "perhaps" } }), error));
    EXPECT_NE(error.find("invalid state of perhaps"), std::string::npos) << error;
}

TEST(FormaManifestCallTableTest, TablesMergeUnderAPrefixAndRefuseCollisions)
{
    TestCallTable first;
    first.expose_form<&forma_call_flag>();
    TestCallTable second;
    second.expose_form<&forma_call_flag>();

    std::string error;
    EXPECT_FALSE(second.merge(first, "", error));
    EXPECT_NE(error.find("forma_call_flag"), std::string::npos) << error;

    EXPECT_TRUE(second.merge(first, "ui.", error)) << error;
    EXPECT_TRUE(second.contains("ui.forma_call_flag"));
}

// =============================================================================
// instantiate_document: elements
// =============================================================================

namespace {

    FormaManifestElement element_document(std::vector<FormaManifestElement> children)
    {
        return manifest_numbered(manifest_node("forma", { { "version", "1" } },
            {
                manifest_node("templates", {},
                    { manifest_node("template", { { "id", "first" } },
                        { manifest_node("stub", { { "size", "3" } }) }) }),
                manifest_node("surface", { { "id", "main" } }, std::move(children)),
            }));
    }

    FormaManifestElement fader_element(std::string name, CallAttributes extra = {})
    {
        CallAttributes attributes { { "name", std::move(name) } };
        attributes.insert(attributes.end(), extra.begin(), extra.end());
        return manifest_node("element", std::move(attributes),
            { call_form("forma_call_fader", { { "bounds", k_fader_bounds }, { "handle_w", "0.04" } }) });
    }

    FormaManifestElement stub_instance(std::string id)
    {
        return manifest_node("instance", { { "id", std::move(id) }, { "template", "first" } });
    }

}

class FormaManifestElementTest : public FormaManifestDriverTest {
protected:
    std::optional<Forma::PlacedInstances> run_elements(const FormaManifestElement& document)
    {
        Forma::ManifestTables<FormaManifestElement> tables;
        tables.factories = table;
        tables.calls = make_call_table();
        tables.actions = actions;
        return Forma::instantiate_document(tables, document, surfaces, error);
    }

    void expect_element_rejected(const FormaManifestElement& document, std::string_view fragment)
    {
        EXPECT_FALSE(run_elements(document));
        EXPECT_NE(error.find(fragment), std::string::npos) << error;
        EXPECT_TRUE(log->placed.empty());
        EXPECT_TRUE(surfaces.at("main").layer().empty());
    }
};

TEST_F(FormaManifestElementTest, AnElementNeedsANameAndOneForm)
{
    expect_element_rejected(element_document({ manifest_node("element", {},
                                {}) }),
        "needs a non-empty name");
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "" } }) }),
        "needs a non-empty name");
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" } }) }),
        "<element> needs a <form>");
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" } },
                                { call_form("forma_call_flag"), call_form("forma_call_flag") }) }),
        "may hold only one <form>");
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" } },
                                { call_form("forma_call_flag"), manifest_node("legend") }) }),
        "<legend> is not allowed in <element>");
}

TEST_F(FormaManifestElementTest, AFormNamesARegisteredCall)
{
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" } }, { manifest_node("form") }) }),
        "<form> needs a call or a topology attribute");
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" } }, { call_form("ghost") }) }),
        "no registered call ghost");
}

TEST_F(FormaManifestElementTest, ANameSharesTheNamespaceOfIds)
{
    expect_element_rejected(element_document({ fader_element("a"), fader_element("a") }), "name a is already used");

    auto instance_first = element_document({ stub_instance("a"), fader_element("a") });
    expect_element_rejected(instance_first, "is already used as an id");

    auto element_first = element_document({ fader_element("a"), stub_instance("a") });
    expect_element_rejected(element_first, "is already used by an element");
}

TEST_F(FormaManifestElementTest, TheLayerAttributesAreCheckedBeforeAnythingIsPlaced)
{
    expect_element_rejected(element_document({ fader_element("a", { { "bogus", "1" } }) }),
        "<element> has an unsupported attribute bogus");
    expect_element_rejected(element_document({ fader_element("a", { { "visible", "maybe" } }) }),
        "<element> has an invalid visible attribute");
    expect_element_rejected(element_document({ fader_element("a", { { "front", "true" }, { "back", "true" } }) }),
        "front and back cannot both be set");
    expect_element_rejected(element_document({ fader_element("a", { { "clip", "0 0 1" } }) }),
        "needs four numbers in clip");
    expect_element_rejected(element_document({ fader_element("a", { { "clip", "0 0 0 1" } }) }),
        "clip needs a positive width and height");
    expect_element_rejected(element_document({ fader_element("a", { { "relate", "" } }) }),
        "needs an element name in relate");
}

TEST_F(FormaManifestElementTest, RelateNamesAnotherElementOrInstanceOfTheSameSurface)
{
    expect_element_rejected(element_document({ fader_element("a", { { "relate", "a" } }) }),
        "relate may not name the element itself");
    expect_element_rejected(element_document({ fader_element("a", { { "relate", "ghost" } }) }),
        "relate refers to ghost, which is not an element or instance of surface main");
}

TEST_F(FormaManifestElementTest, ADecodeFailureInAnElementStopsBeforeAnInstanceIsPlaced)
{
    auto bad = manifest_node("element", { { "name", "b" } },
        { call_form("forma_call_fader", { { "bounds", k_fader_bounds }, { "handle_w", "wide" } }) });
    expect_element_rejected(element_document({ stub_instance("a"), bad }), "invalid handle_w of wide");
}

TEST_F(FormaManifestElementTest, AnOnChildOfAnElementNeedsAnActionTheSameWayAnInstancesDoes)
{
    auto element = fader_element("a");
    element.children.push_back(manifest_node("on", { { "event", "MOUSE_BUTTON_PRESSED" }, { "action", "ghost" } }));
    expect_element_rejected(element_document({ element }), "no registered action ghost");
}

TEST_F(FormaManifestElementTest, RowsAreCheckedAgainstADocumentThatHoldsElementsAndOnChildren)
{
    Forma::ManifestTables<FormaManifestElement> tables;
    tables.factories = table;
    tables.calls = make_call_table();
    tables.actions = actions;
    actions.expose("log", Forma::Action([](const Forma::ActionEvent&) { }));
    tables.actions = actions;

    auto element = fader_element("f");
    element.children.push_back(manifest_node("on", { { "event", "MOUSE_BUTTON_PRESSED" }, { "action", "log" } }));
    const auto document = element_document({ element });
    Kakshya::CompositeArray rows(Kakshya::described_composite_layout<Forma::RectPlacementInputs>());

    EXPECT_FALSE(Forma::instantiate_rows(tables, document, "first", rows, surfaces.at("main"), error));

    EXPECT_NE(error.find("has no bulk placement"), std::string::npos) << error;
    EXPECT_EQ(error.find("needs a call table"), std::string::npos) << error;
    EXPECT_EQ(error.find("needs an action table"), std::string::npos) << error;
}

TEST_F(FormaManifestElementTest, ASurfaceHoldsOnlyInstancesAndElements)
{
    expect_element_rejected(element_document({ stub_instance("a"), manifest_node("legend") }),
        "<legend> is not allowed in <surface>");
}

// =============================================================================
// Elements: spatial children and vertices written in the document
// =============================================================================

namespace {

    FormaManifestElement circle_node(std::string center, std::string radius)
    {
        return manifest_node("circle", { { "center", std::move(center) }, { "radius", std::move(radius) } });
    }

    std::optional<Forma::ElementHit> read_hit(const std::vector<FormaManifestElement>& children, std::string& error)
    {
        Forma::ElementHit hit;
        for (const FormaManifestElement& child : children) {
            if (!Forma::read_hit_child<FormaManifestElement>(child, hit, error))
                return std::nullopt;
        }
        return hit;
    }

    FormaManifestElement vertex_node(std::string position, CallAttributes extra = {})
    {
        CallAttributes attributes { { "position", std::move(position) } };
        attributes.insert(attributes.end(), extra.begin(), extra.end());
        return manifest_node("vertex", std::move(attributes));
    }

    FormaManifestElement triangle_form(std::string topology = "TRIANGLE_LIST")
    {
        return manifest_node("form", { { "topology", std::move(topology) } },
            { vertex_node("-0.5 -0.5 0"), vertex_node("0.5 -0.5 0", { { "color", "1 0 0 1" } }), vertex_node("0 0.5 0") });
    }

}

TEST(FormaManifestHitTest, ACircleBecomesTheContainmentOfThatCircle)
{
    std::string error;
    const auto hit = read_hit({ circle_node("0 0", "0.5") }, error);

    ASSERT_TRUE(hit) << error;
    EXPECT_FALSE(hit->bounds.has_value());
    ASSERT_TRUE(static_cast<bool>(hit->contains));
    EXPECT_TRUE(hit->contains({ 0.3F, 0.F }));
    EXPECT_FALSE(hit->contains({ 0.6F, 0.F }));
}

TEST(FormaManifestHitTest, APolygonAndAStrokeAreReadAsPointLists)
{
    std::string error;
    const auto polygon = read_hit({ manifest_node("polygon", { { "points", "0 0 1 0 1 1 0 1" } }) }, error);
    ASSERT_TRUE(polygon) << error;
    EXPECT_TRUE(polygon->contains({ 0.5F, 0.5F }));
    EXPECT_FALSE(polygon->contains({ 1.5F, 0.5F }));

    const auto stroke = read_hit(
        { manifest_node("stroke", { { "points", "0 0 1 0" }, { "half_thickness", "0.1" } }) }, error);
    ASSERT_TRUE(stroke) << error;
    EXPECT_TRUE(stroke->contains({ 0.5F, 0.05F }));
    EXPECT_FALSE(stroke->contains({ 0.5F, 0.5F }));
}

TEST(FormaManifestHitTest, BoundsAreTheFastRejectBoxAndNeedAMaxAboveTheirMin)
{
    std::string error;
    const auto hit = read_hit({ manifest_node("bounds", { { "min", "-1 -2" }, { "max", "3 4" } }) }, error);

    ASSERT_TRUE(hit) << error;
    ASSERT_TRUE(hit->bounds.has_value());
    EXPECT_EQ(hit->bounds->min, glm::vec2(-1.F, -2.F));
    EXPECT_EQ(hit->bounds->max, glm::vec2(3.F, 4.F));
    EXPECT_FALSE(static_cast<bool>(hit->contains));

    EXPECT_FALSE(read_hit({ manifest_node("bounds", { { "min", "1 1" }, { "max", "0 2" } }) }, error));
    EXPECT_NE(error.find("needs a max above its min"), std::string::npos) << error;
}

TEST(FormaManifestHitTest, TwoRegionsCombineAsTheKinesisFunctionsOfTheSameName)
{
    std::string error;
    const auto ring = read_hit({ manifest_node("subtract_bounds", {},
                                  { circle_node("0 0", "0.5"), circle_node("0 0", "0.2") }) },
        error);
    ASSERT_TRUE(ring) << error;
    EXPECT_TRUE(ring->contains({ 0.35F, 0.F }));
    EXPECT_FALSE(ring->contains({ 0.F, 0.F }));
    EXPECT_FALSE(ring->contains({ 0.9F, 0.F }));

    const auto either = read_hit({ manifest_node("union_bounds", {},
                                     { circle_node("-1 0", "0.3"), circle_node("1 0", "0.3") }) },
        error);
    ASSERT_TRUE(either) << error;
    EXPECT_TRUE(either->contains({ -1.F, 0.F }));
    EXPECT_TRUE(either->contains({ 1.F, 0.F }));
    EXPECT_FALSE(either->contains({ 0.F, 0.F }));

    const auto both = read_hit({ manifest_node("intersect_bounds", {},
                                    { circle_node("-0.2 0", "0.5"), circle_node("0.2 0", "0.5") }) },
        error);
    ASSERT_TRUE(both) << error;
    EXPECT_TRUE(both->contains({ 0.F, 0.F }));
    EXPECT_FALSE(both->contains({ -0.6F, 0.F }));
}

TEST(FormaManifestHitTest, CombinationsNest)
{
    std::string error;
    const auto hit = read_hit({ manifest_node("union_bounds", {},
                                  { manifest_node("subtract_bounds", {}, { circle_node("0 0", "0.5"), circle_node("0 0", "0.2") }),
                                      circle_node("2 0", "0.3") }) },
        error);

    ASSERT_TRUE(hit) << error;
    EXPECT_TRUE(hit->contains({ 0.35F, 0.F }));
    EXPECT_FALSE(hit->contains({ 0.F, 0.F }));
    EXPECT_TRUE(hit->contains({ 2.F, 0.F }));
}

TEST(FormaManifestHitTest, ARegionThatIsNotValidIsRefusedWithItsRow)
{
    const auto refused = [](FormaManifestElement node, std::string_view fragment) {
        node.row = 12;
        std::string error;
        EXPECT_FALSE(read_hit({ node }, error));
        EXPECT_TRUE(error.starts_with(row_prefix(12))) << error;
        EXPECT_NE(error.find(fragment), std::string::npos) << error;
    };

    refused(manifest_node("circle", { { "center", "0 0" } }), "needs a radius attribute");
    refused(circle_node("0 0", "wide"), "invalid radius of wide");
    refused(circle_node("0 0", "0"), "needs a positive radius");
    refused(manifest_node("circle", { { "center", "0 0" }, { "radius", "1" }, { "bogus", "1" } }),
        "unsupported attribute bogus");
    refused(manifest_node("polygon", { { "points", "0 0 1 0" } }), "needs at least three x y pairs");
    refused(manifest_node("stroke", { { "points", "0 0" }, { "half_thickness", "0.1" } }), "needs at least two x y pairs");
    refused(manifest_node("stroke", { { "points", "0 0 1 0" }, { "half_thickness", "0" } }), "needs a positive half_thickness");
    refused(manifest_node("union_bounds", {}, { circle_node("0 0", "1") }), "needs exactly two regions");
    refused(manifest_node("union_bounds", {}, { circle_node("0 0", "1"), circle_node("0 0", "2"), circle_node("0 0", "3") }),
        "needs exactly two regions");
    auto not_a_region = manifest_node("bounds");
    not_a_region.row = 12;
    refused(manifest_node("subtract_bounds", {}, { circle_node("0 0", "1"), not_a_region }),
        "is not a region");
    refused(manifest_node("subtract_bounds", { { "x", "1" } }, { circle_node("0 0", "1"), circle_node("0 0", "2") }),
        "unsupported attribute x");
}

TEST(FormaManifestHitTest, AnElementHasOneBoundsAndOneRegion)
{
    std::string error;
    EXPECT_FALSE(read_hit({ circle_node("0 0", "1"), circle_node("0 0", "2") }, error));
    EXPECT_NE(error.find("is a second region"), std::string::npos) << error;

    const auto box = manifest_node("bounds", { { "min", "0 0" }, { "max", "1 1" } });
    EXPECT_FALSE(read_hit({ box, box }, error));
    EXPECT_NE(error.find("may appear only once"), std::string::npos) << error;

    EXPECT_TRUE(read_hit({ box, circle_node("0 0", "1") }, error)) << error;
}

TEST(FormaManifestHitTest, TheDocumentsFieldsAreWrittenAfterTheFormsOwn)
{
    Forma::Geometry::Form<float> form;
    form.geometry = [](float, std::vector<uint8_t>&, Forma::Element& element) {
        element.bounds_hint = Kinesis::AABB2D { .min = { 0.F, 0.F }, .max = { 1.F, 1.F } };
        element.contains = {};
    };
    Forma::Element untouched;
    std::vector<uint8_t> bytes;

    auto unchanged = form;
    Forma::apply_hit(unchanged, Forma::ElementHit {});
    unchanged.geometry(0.F, bytes, untouched);
    EXPECT_EQ(untouched.bounds_hint->max, glm::vec2(1.F));
    EXPECT_FALSE(static_cast<bool>(untouched.contains));

    Forma::ElementHit hit;
    hit.bounds = Kinesis::AABB2D { .min = { -2.F, -2.F }, .max = { 2.F, 2.F } };
    hit.contains = Kinesis::circular_bounds({ 0.F, 0.F }, 1.F);
    Forma::Element written;
    Forma::apply_hit(form, hit);
    form.geometry(0.F, bytes, written);

    EXPECT_EQ(written.bounds_hint->max, glm::vec2(2.F));
    ASSERT_TRUE(static_cast<bool>(written.contains));
    EXPECT_TRUE(written.contains({ 0.5F, 0.F }));
    EXPECT_FALSE(written.contains({ 1.5F, 0.F }));
}

TEST(FormaManifestGeometryTest, AVertexFormDrawsTheVerticesAsWrittenWithNothingMappingThem)
{
    const auto input = triangle_input();
    const auto form = Forma::vertex_form(input);

    EXPECT_EQ(form.topology, PrimitiveTopology::TRIANGLE_LIST);
    EXPECT_EQ(form.capacity, 3 * sizeof(Kakshya::Vertex));

    std::vector<uint8_t> bytes;
    Forma::Element element;
    form.geometry(std::monostate {}, bytes, element);
    const auto vertices = read_vertices<Kakshya::Vertex>(bytes);

    ASSERT_EQ(vertices.size(), 3U);
    EXPECT_EQ(vertices[2].position, glm::vec3(0.5F, 1.F, 0.25F));
    EXPECT_EQ(vertices[1].color, glm::vec4(0.F, 1.F, 0.F, 1.F));
    expect_bounds(element, { 0.F, 0.F }, { 1.F, 1.F });
    EXPECT_TRUE(element.hits({ 0.5F, 0.4F }));
    EXPECT_FALSE(element.hits({ 0.05F, 0.9F }));
}

TEST_F(FormaManifestElementTest, AFormEitherCallsAFunctionOrGivesATopologyNotBothAndNotNeither)
{
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" } },
                                { manifest_node("form", { { "call", "forma_call_flag" }, { "topology", "POINT_LIST" } }) }) }),
        "may call a function or give a topology, not both");
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" } },
                                { manifest_node("form", { { "bogus", "1" } }) }) }),
        "<form> needs a call or a topology attribute");
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" } },
                                { manifest_node("form", { { "call", "forma_call_flag" } }, { vertex_node("0 0 0") }) }) }),
        "that calls a function takes no children");
}

TEST_F(FormaManifestElementTest, AFormOfVerticesIsDecodedStrictlyBeforeAnythingIsPlaced)
{
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" } }, { triangle_form("BANANA") }) }),
        "unsupported topology BANANA");
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" } },
                                { manifest_node("form", { { "topology", "POINT_LIST" } }) }) }),
        "needs at least 1 vertex children");
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" }, { "state", "1" } }, { triangle_form() }) }),
        "a form of vertices has no state");
}

TEST_F(FormaManifestElementTest, SpatialChildrenAreCheckedBeforeAnythingIsPlaced)
{
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" } },
                                { triangle_form(), circle_node("0 0", "0") }) }),
        "needs a positive radius");
    expect_element_rejected(element_document({ manifest_node("element", { { "name", "a" } },
                                { triangle_form(), circle_node("0 0", "1"), circle_node("0 0", "2") }) }),
        "is a second region");
}


}
