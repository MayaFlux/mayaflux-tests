#include "../../support/forma_manifest_support.hpp"
#include "../../support/forma_rig.hpp"

#include <fstream>

#include "MayaFlux/IO/Composite/XmlDocument.hpp"
#include "MayaFlux/IO/Composite/XmlRecordAccess.hpp"
#include "MayaFlux/Portal/Forma/Manifest/Instantiate.hpp"
#include "MayaFlux/Portal/Forma/Manifest/StaticGeometry.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;

namespace {

    using Attributes = std::vector<std::pair<std::string, std::string>>;
    using TreeTable = Forma::FactoryTable<FormaManifestElement>;
    using XmlTable = Forma::FactoryTable<IO::XmlElementView>;

    template <typename Table>
    void register_shapes(Table& table, Forma::PolygonFactory polygon = {})
    {
        Forma::expose_polygon(table, "polygon", std::move(polygon));
        Forma::expose_vertex_geometry(table, "vertices");
    }

    Forma::PolygonFactory flipping_polygon()
    {
        Forma::PolygonFactory polygon;
        polygon.interactions.expose("flip", Forma::press_flip_interaction());
        return polygon;
    }

    FormaManifestElement polygon_template(const std::string& id, Attributes attributes,
        std::vector<FormaManifestElement> holes = {})
    {
        return manifest_node("template", { { "id", id } },
            { manifest_node("polygon", std::move(attributes), std::move(holes)) });
    }

    FormaManifestElement square_template(const std::string& id, Attributes extra = {})
    {
        Attributes attributes { { "points", "0 0 1 0 1 1 0 1" } };
        attributes.insert(attributes.end(), extra.begin(), extra.end());
        return polygon_template(id, std::move(attributes));
    }

    FormaManifestElement instance(const std::string& id, const std::string& tmpl, const std::string& rect)
    {
        return manifest_node("instance", { { "id", id }, { "template", tmpl }, { "rect", rect } });
    }

    FormaManifestElement document(std::vector<FormaManifestElement> templates,
        std::vector<FormaManifestElement> surfaces)
    {
        std::vector<FormaManifestElement> children;
        children.push_back(manifest_node("templates", {}, std::move(templates)));
        for (FormaManifestElement& surface : surfaces)
            children.push_back(std::move(surface));
        return manifest_numbered(manifest_node("forma", { { "version", "1" } }, std::move(children)));
    }

    FormaManifestElement main_surface(std::vector<FormaManifestElement> instances)
    {
        return manifest_node("surface", { { "id", "main" } }, std::move(instances));
    }

    void expect_bounds(const Forma::Element& element, glm::vec2 min, glm::vec2 max)
    {
        ASSERT_TRUE(element.bounds_hint);
        EXPECT_NEAR(element.bounds_hint->min.x, min.x, 1e-5F);
        EXPECT_NEAR(element.bounds_hint->min.y, min.y, 1e-5F);
        EXPECT_NEAR(element.bounds_hint->max.x, max.x, 1e-5F);
        EXPECT_NEAR(element.bounds_hint->max.y, max.y, 1e-5F);
    }

    using PolygonStates = std::vector<std::shared_ptr<Forma::MappedState<bool>>>;

    void expose_capturing_polygon(TreeTable& table, Forma::PolygonFactory factory, PolygonStates& states)
    {
        Reflect::RecordBinding<Forma::PolygonInputs, FormaManifestElement> input;
        input.required("points").repeated<&Forma::PolygonInputs::holes>("hole", {}, "points");
        Reflect::RecordBinding<Forma::RectPlacementInputs, FormaManifestElement> placement;
        placement.required("rect");

        table.expose_typed<bool>("polygon", std::move(input), std::move(placement), std::move(factory),
            [](const Forma::PolygonInputs& decoded, const Forma::RectPlacementInputs&,
                std::string&) -> std::optional<bool> { return decoded.on; },
            [&states](const Forma::PolygonInputs&, const Forma::RectPlacementInputs&,
                Forma::Tend<bool>& tend) { states.push_back(tend.result->state); });
    }

}

class FormaManifestSurfaceTest : public FormaRigTest {
protected:
    void TearDown() override
    {
        FormaRigTest::TearDown();
        std::error_code ec;
        for (const auto& path : written)
            std::filesystem::remove(path, ec);
    }

    std::shared_ptr<const IO::XmlDocument> load_xml(const std::string& stem, std::string_view text)
    {
        const auto path = std::filesystem::temp_directory_path()
            / ("mf_manifest_" + stem + "_"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".xml");
        {
            std::ofstream file(path);
            file << text;
        }
        written.push_back(path);

        std::string load_error;
        auto loaded = IO::XmlDocument::load(path.string(), load_error);
        EXPECT_TRUE(loaded) << load_error;
        return loaded;
    }

    std::vector<std::filesystem::path> written;
};

// =============================================================================
// A document from a file
// =============================================================================

TEST_F(FormaManifestSurfaceTest, PlacesADocumentLoadedFromXmlOntoTheBoundSurface)
{
    const auto loaded = load_xml("places", R"(<?xml version="1.0" encoding="UTF-8"?>
<forma version="1">
  <templates>
    <template id="panel">
      <polygon points="0 0 1 0 1 1 0 1" color="1 0 0 1"/>
    </template>
    <template id="wedge">
      <vertices topology="TRIANGLE_LIST">
        <vertex position="0 0 0"/>
        <vertex position="1 0 0"/>
        <vertex position="0.5 1 0"/>
      </vertices>
    </template>
  </templates>
  <surface id="main">
    <instance id="left" template="panel" rect="-0.8 -0.5 0.6 1"/>
    <instance id="right" template="wedge" rect="0.2 -0.5 0.6 1"/>
  </surface>
</forma>
)");
    ASSERT_TRUE(loaded);
    const auto root = loaded->root_element();
    ASSERT_TRUE(root);

    XmlTable table;
    register_shapes(table);
    Forma::SurfaceMap surfaces;
    surfaces.emplace("main", make_surface("manifest-xml"));

    std::string error;
    const auto count = Forma::instantiate_document(table, *root, surfaces, error);

    ASSERT_TRUE(count) << error;
    EXPECT_EQ(count->size(), 2U);
    const auto& elements = surfaces.at("main").layer().elements();
    ASSERT_EQ(elements.size(), 2U);
    expect_bounds(elements[0], { -0.8F, -0.5F }, { -0.2F, 0.5F });
    expect_bounds(elements[1], { 0.2F, -0.5F }, { 0.8F, 0.5F });
    EXPECT_EQ(elements[0].name, "left");
    EXPECT_EQ(count->at("right").element, elements[1].id);
    EXPECT_EQ(count->at("right").surface, "main");
    EXPECT_EQ(surfaces.at("main").layer().find("right"), elements[1].id);
}

TEST_F(FormaManifestSurfaceTest, AnXmlErrorCarriesTheRowOfTheElement)
{
    const auto loaded = load_xml("rowed", R"(<?xml version="1.0" encoding="UTF-8"?>
<forma version="1">
  <templates>
    <template id="panel">
      <polygon points="0 0 1 0 1 1 0 1"/>
    </template>
  </templates>
  <surface id="main">
    <instance id="left" template="panel" rect="0 0 1 1" bogus="1"/>
  </surface>
</forma>
)");
    ASSERT_TRUE(loaded);
    const auto root = loaded->root_element();
    ASSERT_TRUE(root);

    XmlTable table;
    register_shapes(table);
    Forma::SurfaceMap surfaces;
    surfaces.emplace("main", make_surface("manifest-xml-error"));

    std::string error;
    EXPECT_FALSE(Forma::instantiate_document(table, *root, surfaces, error));

    EXPECT_TRUE(error.starts_with("row ")) << error;
    EXPECT_NE(error.find("bogus"), std::string::npos) << error;
    EXPECT_TRUE(surfaces.at("main").layer().empty());
}

// =============================================================================
// Hit testing, state and interaction
// =============================================================================

TEST_F(FormaManifestSurfaceTest, HitTestingFollowsTheDeclaredGeometryIncludingItsHole)
{
    const auto tree = document(
        { polygon_template("donut", { { "points", "0 0 1 0 1 1 0 1" } },
            { manifest_node("hole", { { "points", "0.3 0.3 0.7 0.3 0.7 0.7 0.3 0.7" } }) }) },
        { main_surface({ instance("ring", "donut", "-1 -1 2 2") }) });
    TreeTable table;
    register_shapes(table);
    Forma::SurfaceMap surfaces;
    surfaces.emplace("main", make_surface("manifest-hole"));

    std::string error;
    ASSERT_TRUE(Forma::instantiate_document(table, tree, surfaces, error)) << error;

    const Forma::Layer& layer = surfaces.at("main").layer();
    ASSERT_EQ(layer.size(), 1U);
    EXPECT_FALSE(layer.hit_test(glm::vec2(0.F, 0.F)));
    const auto ring = layer.hit_test(glm::vec2(-0.8F, -0.6F));
    ASSERT_TRUE(ring);
    EXPECT_EQ(*ring, layer.elements()[0].id);
    EXPECT_FALSE(layer.hit_test(glm::vec2(1.5F, 0.F)));
}

TEST_F(FormaManifestSurfaceTest, TheDocumentsLayerAttributesApplyToRealPlacedElements)
{
    auto hidden = instance("a", "panel", "-0.9 -0.5 0.8 1");
    hidden.attributes.emplace_back("mf:hidden", "true");
    auto tied = instance("b", "panel", "0.1 -0.5 0.8 1");
    tied.attributes.emplace_back("mf:relate", "a");
    auto locked = instance("c", "panel", "-0.5 0.6 1 0.3");
    locked.attributes.emplace_back("mf:interactive", "false");
    const auto tree = document({ square_template("panel") }, { main_surface({ hidden, tied, locked }) });
    TreeTable table;
    register_shapes(table);
    Forma::SurfaceMap surfaces;
    surfaces.emplace("main", make_surface("manifest-layer-attributes"));

    std::string error;
    const auto placed = Forma::instantiate_document(table, tree, surfaces, error);

    ASSERT_TRUE(placed) << error;
    const Forma::Layer& layer = surfaces.at("main").layer();
    ASSERT_EQ(layer.size(), 3U);
    EXPECT_EQ(layer.get(placed->at("a").element)->name, "a");
    EXPECT_FALSE(layer.get(placed->at("a").element)->visible);
    EXPECT_FALSE(layer.get(placed->at("b").element)->visible);
    EXPECT_TRUE(layer.get(placed->at("c").element)->visible);
    EXPECT_FALSE(layer.get(placed->at("c").element)->interactive);
    EXPECT_EQ(layer.find("b"), placed->at("b").element);
}

TEST_F(FormaManifestSurfaceTest, TheInitialStateWrittenInTheDocumentReachesTheState)
{
    const auto tree = document(
        { square_template("lit", { { "on", "true" } }), square_template("dark") },
        { main_surface({ instance("a", "lit", "-0.9 -0.5 0.8 1"), instance("b", "dark", "0.1 -0.5 0.8 1") }) });
    PolygonStates states;
    TreeTable table;
    expose_capturing_polygon(table, {}, states);
    Forma::SurfaceMap surfaces;
    surfaces.emplace("main", make_surface("manifest-state"));

    std::string error;
    ASSERT_TRUE(Forma::instantiate_document(table, tree, surfaces, error)) << error;

    ASSERT_EQ(states.size(), 2U);
    EXPECT_TRUE(states[0]->value);
    EXPECT_FALSE(states[1]->value);
}

TEST_F(FormaManifestSurfaceTest, ANamedInteractionInTheDocumentIsWiredToThePlacedElement)
{
    const auto tree = document(
        { square_template("toggle", { { "interaction", "flip" } }) },
        { main_surface({ instance("only", "toggle", "-0.5 -0.5 1 1") }) });
    PolygonStates states;
    TreeTable table;
    expose_capturing_polygon(table, flipping_polygon(), states);
    Forma::SurfaceMap surfaces;
    surfaces.emplace("main", make_surface("manifest-wire"));

    std::string error;
    ASSERT_TRUE(Forma::instantiate_document(table, tree, surfaces, error)) << error;
    ASSERT_EQ(states.size(), 1U);
    EXPECT_FALSE(states[0]->value);

    window->move_to_ndc({ 0.F, 0.F });
    window->press(IO::MouseButtons::Left);
    EXPECT_TRUE(states[0]->value);

    window->press(IO::MouseButtons::Left);
    EXPECT_FALSE(states[0]->value);
}

TEST_F(FormaManifestSurfaceTest, AnInteractionNameNobodyRegisteredFailsBeforeAnythingIsPlaced)
{
    const auto tree = document(
        { square_template("toggle", { { "interaction", "missing" } }) },
        { main_surface({ instance("only", "toggle", "-0.5 -0.5 1 1") }) });
    TreeTable table;
    register_shapes(table, flipping_polygon());
    Forma::SurfaceMap surfaces;
    surfaces.emplace("main", make_surface("manifest-missing-wire"));

    std::string error;
    EXPECT_FALSE(Forma::instantiate_document(table, tree, surfaces, error));

    EXPECT_TRUE(error.starts_with("row ")) << error;
    EXPECT_NE(error.find("missing"), std::string::npos) << error;
    EXPECT_TRUE(surfaces.at("main").layer().empty());
}

// =============================================================================
// Surfaces, placers and rows
// =============================================================================

TEST_F(FormaManifestSurfaceTest, EachBoundSurfaceReceivesOnlyItsOwnInstances)
{
    const auto tree = document({ square_template("panel") },
        { main_surface({ instance("a", "panel", "-0.9 -0.5 0.8 1"), instance("b", "panel", "0.1 -0.5 0.8 1") }),
            manifest_node("surface", { { "id", "side" } }, { instance("c", "panel", "-0.5 -0.5 1 1") }) });
    TreeTable table;
    register_shapes(table);
    Forma::SurfaceMap surfaces;
    surfaces.emplace("main", make_surface("manifest-two-main"));
    surfaces.emplace("side", make_surface("manifest-two-side"));

    std::string error;
    const auto count = Forma::instantiate_document(table, tree, surfaces, error);

    ASSERT_TRUE(count) << error;
    EXPECT_EQ(count->size(), 3U);
    EXPECT_EQ(surfaces.at("main").layer().size(), 2U);
    EXPECT_EQ(surfaces.at("side").layer().size(), 1U);
}

TEST_F(FormaManifestSurfaceTest, AFailingDocumentLeavesTheSurfaceUntouched)
{
    auto bad = instance("c", "panel", "0 0 1 1");
    bad.attributes.emplace_back("bogus", "1");
    const auto tree = document({ square_template("panel") },
        { main_surface({ instance("a", "panel", "-0.9 -0.5 0.8 1"), instance("b", "panel", "0.1 -0.5 0.8 1"), bad }) });
    TreeTable table;
    register_shapes(table);
    Forma::SurfaceMap surfaces;
    surfaces.emplace("main", make_surface("manifest-bad"));

    std::string error;
    EXPECT_FALSE(Forma::instantiate_document(table, tree, surfaces, error));

    EXPECT_NE(error.find("bogus"), std::string::npos) << error;
    EXPECT_TRUE(surfaces.at("main").layer().empty());
}

TEST_F(FormaManifestSurfaceTest, APreparedPlacerCanBeRunMoreThanOnce)
{
    const auto tree = document({ square_template("panel") },
        { main_surface({ instance("a", "panel", "-0.5 -0.5 1 1") }) });
    TreeTable table;
    register_shapes(table);

    const auto& factory = tree.children[0].children[0].children[0];
    const auto& placed = tree.children[1].children[0];
    std::string error;
    auto placer = std::invoke(*table.find("polygon"), factory, placed, error);
    ASSERT_TRUE(placer) << error;

    auto surface = make_surface("manifest-reuse");
    EXPECT_TRUE((*placer)(surface, error)) << error;
    EXPECT_TRUE((*placer)(surface, error)) << error;

    EXPECT_EQ(surface.layer().size(), 2U);
}

TEST_F(FormaManifestSurfaceTest, TypedRowsBecomeOneElementEachFromASingleTemplate)
{
    const auto tree = document({ square_template("panel") }, {});
    TreeTable table;
    register_shapes(table);

    Kakshya::CompositeArray rows(Kakshya::described_composite_layout<Forma::RectPlacementInputs>());
    Kakshya::CompositeInsertion insertion(rows);
    std::string error;
    for (int i = 0; i < 5; ++i) {
        const Forma::RectPlacementInputs cell {
            .rect = { -0.9F + 0.3F * static_cast<float>(i), 0.F, 0.2F, 0.5F },
        };
        ASSERT_TRUE(Kakshya::write_described(insertion, insertion.append(), cell, error)) << error;
    }

    auto surface = make_surface("manifest-rows");
    const auto count = Forma::instantiate_rows(table, tree, "panel", rows, surface, error);

    ASSERT_TRUE(count) << error;
    EXPECT_EQ(count->size(), 5U);
    const auto& elements = surface.layer().elements();
    ASSERT_EQ(elements.size(), 5U);
    for (size_t i = 0; i < elements.size(); ++i) {
        const float x = -0.9F + 0.3F * static_cast<float>(i);
        expect_bounds(elements[i], { x, 0.F }, { x + 0.2F, 0.5F });
        EXPECT_EQ((*count)[i], elements[i].id);
        EXPECT_TRUE(elements[i].name.empty());
    }
}

TEST_F(FormaManifestSurfaceTest, RowsForATemplateThatDoesNotExistPlaceNothing)
{
    const auto tree = document({ square_template("panel") }, {});
    TreeTable table;
    register_shapes(table);
    Kakshya::CompositeArray rows(Kakshya::described_composite_layout<Forma::RectPlacementInputs>());

    auto surface = make_surface("manifest-rows-missing");
    std::string error;
    EXPECT_FALSE(Forma::instantiate_rows(table, tree, "ghost", rows, surface, error));

    EXPECT_EQ(error, "document has no template ghost");
    EXPECT_TRUE(surface.layer().empty());
}

TEST_F(FormaManifestSurfaceTest, AMemberPointerSuppliesTheInitialStateOfThePlacedElement)
{
    std::vector<std::shared_ptr<Forma::MappedState<float>>> states;
    const auto make_form = [](const FormaManifestProbe&, const FormaManifestSpot&,
                               std::string&) -> std::optional<Forma::Geometry::Form<float>> {
        return Forma::Geometry::Form<float> {
            [](float, std::vector<uint8_t>& out, Forma::Element&) { out.assign(4, 0); },
            Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST,
            64,
        };
    };
    const auto hook = [&states](const FormaManifestProbe&, const FormaManifestSpot&,
                          Forma::Tend<float>& tend) { states.push_back(tend.result->state); };

    Reflect::RecordBinding<FormaManifestProbe, FormaManifestElement> input;
    Reflect::RecordBinding<FormaManifestSpot, FormaManifestElement> placement;
    TreeTable table;
    table.expose_typed<float>("from_input", input, placement, make_form, &FormaManifestProbe::gain, hook);
    table.expose_typed<float>("from_placement", input, placement, make_form, &FormaManifestSpot::scale, hook);

    const auto tree = manifest_numbered(manifest_node("forma", { { "version", "1" } },
        {
            manifest_node("templates", {},
                {
                    manifest_node("template", { { "id", "t1" } },
                        { manifest_node("from_input", { { "gain", "0.75" } }) }),
                    manifest_node("template", { { "id", "t2" } },
                        { manifest_node("from_placement") }),
                }),
            manifest_node("surface", { { "id", "main" } },
                {
                    manifest_node("instance", { { "id", "a" }, { "template", "t1" }, { "at", "0 0" } }),
                    manifest_node("instance", { { "id", "b" }, { "template", "t2" }, { "at", "0 0" }, { "scale", "4" } }),
                }),
        }));
    Forma::SurfaceMap surfaces;
    surfaces.emplace("main", make_surface("manifest-member-initial"));

    std::string error;
    ASSERT_TRUE(Forma::instantiate_document(table, tree, surfaces, error)) << error;

    ASSERT_EQ(states.size(), 2U);
    EXPECT_FLOAT_EQ(states[0]->value, 0.75F);
    EXPECT_FLOAT_EQ(states[1]->value, 4.F);
}

}
