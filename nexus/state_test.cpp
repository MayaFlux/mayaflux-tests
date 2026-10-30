#include "gtest/gtest.h"

#include "../support/frame_scheduler.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/IO/GlmSerializer.hpp"
#include "MayaFlux/IO/Image/EXRWriter.hpp"
#include "MayaFlux/Nexus/Principals/Locus.hpp"
#include "MayaFlux/Nexus/Principals/Presence.hpp"
#include "MayaFlux/Nexus/State/Decoder.hpp"
#include "MayaFlux/Nexus/State/Encoder.hpp"
#include "MayaFlux/Transitive/IO/JSONSerializer.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

namespace MayaFlux::Test {
namespace {

    using Kinesis::AABB3D;
    using Nexus::Agent;
    using Nexus::Emitter;
    using Nexus::Expanse;
    using Nexus::Fabric;
    using Nexus::InfluenceContext;
    using Nexus::Locus;
    using Nexus::PerceptionContext;
    using Nexus::Presence;
    using Nexus::Sensor;
    using Nexus::StateDecoder;
    using Nexus::StateEncoder;
    using Nexus::Tapestry;
    using Nexus::State::WiringKind;

    Vruta::SoundRoutine counting_routine(int* counter)
    {
        ++*counter;
        co_return;
    }

    AABB3D box(float half)
    {
        return AABB3D::from_center(glm::vec3(0.0F), glm::vec3(half));
    }

    bool has_warning(const std::vector<std::string>& warnings, const std::string& needle)
    {
        return std::ranges::any_of(warnings, [&needle](const std::string& w) { return w.find(needle) != std::string::npos; });
    }

    class NexusStateTest : public ::testing::Test {
    protected:
        void SetUp() override { IO::EXRWriter::register_with_registry(); }

        std::shared_ptr<Emitter> add_emitter(Fabric& fabric, const std::optional<glm::vec3>& at = std::nullopt)
        {
            auto emitter = std::make_shared<Emitter>();
            if (at) {
                emitter->set_position(*at);
            }
            fabric.wire(emitter).finalise();
            return emitter;
        }

        void prime_influence(Fabric& fabric, const std::string& name, Emitter::InfluenceFn fn)
        {
            auto emitter = std::make_shared<Emitter>(name, std::move(fn));
            fabric.wire(emitter).finalise();
            fabric.remove(emitter->id());
        }

        void prime_perception(Fabric& fabric, const std::string& name, Sensor::PerceptionFn fn)
        {
            auto sensor = std::make_shared<Sensor>(1.0F, name, std::move(fn));
            fabric.wire(sensor).finalise();
            fabric.remove(sensor->id());
        }

        std::optional<Nexus::State::FabricSchema> read_schema(const std::string& base)
        {
            IO::JSONSerializer ser;
            return ser.read<Nexus::State::FabricSchema>(base + ".json");
        }

        void edit_schema(const std::string& base, const std::function<void(nlohmann::json&)>& edit)
        {
            nlohmann::json json;
            {
                std::ifstream in(base + ".json");
                in >> json;
            }
            edit(json);
            std::ofstream(base + ".json") << json.dump();
        }

        void step_samples(uint64_t count)
        {
            for (uint64_t i = 0; i < count; ++i) {
                frames.scheduler->process_token(Vruta::ProcessingToken::SAMPLE_ACCURATE, 1);
            }
        }

        void restore(Fabric& fabric, const std::string& base)
        {
            static_cast<void>(decoder.reconstruct(fabric, base));
        }

        void restore(Tapestry& tapestry, const std::string& base)
        {
            static_cast<void>(decoder.reconstruct(tapestry, base));
        }

        static uint32_t first_id(const Fabric& fabric)
        {
            const auto ids = fabric.all_ids();
            return ids.empty() ? 0U : ids.front();
        }

        ScopedWorkingDir cwd;
        std::string snap_base = (std::filesystem::current_path() / "snap").string();
        std::string again_base = (std::filesystem::current_path() / "again").string();
        std::string act_dir = (std::filesystem::current_path() / "act").string();
        std::string act2_dir = (std::filesystem::current_path() / "act2").string();
        FrameScheduler frames;
        std::shared_ptr<Vruta::EventManager> events = std::make_shared<Vruta::EventManager>();
        std::shared_ptr<Buffers::BufferManager> buffers = std::make_shared<Buffers::BufferManager>();
        Fabric source { *frames.scheduler, *events, *buffers, 1.0F };
        Fabric target { *frames.scheduler, *events, *buffers, 1.0F };
        StateEncoder encoder;
        StateDecoder decoder;
        int target_pulse {};
        int target_watch {};
    };

    TEST_F(NexusStateTest, EncodeWritesTheImageAndTheSchema)
    {
        add_emitter(source, glm::vec3(1.0F, 2.0F, 3.0F));

        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        EXPECT_TRUE(std::filesystem::exists(snap_base + ".exr"));
        EXPECT_TRUE(std::filesystem::exists(snap_base + ".json"));
        EXPECT_TRUE(encoder.last_error().empty());
    }

    TEST_F(NexusStateTest, SchemaListsEntitiesInIdOrderWithTheirKinds)
    {
        Fabric local(*frames.scheduler, *events, *buffers, 2.0F);
        local.set_name("stage");
        add_emitter(local);
        add_emitter(local);
        local.wire(std::make_shared<Sensor>(1.0F)).finalise();
        local.wire(std::make_shared<Agent>(1.0F)).finalise();

        ASSERT_TRUE(encoder.encode(local, snap_base)) << encoder.last_error();
        const auto schema = read_schema(snap_base);

        ASSERT_TRUE(schema.has_value());
        EXPECT_EQ(schema->fabric_name, "stage");
        ASSERT_TRUE(schema->cell_size.has_value());
        EXPECT_FLOAT_EQ(*schema->cell_size, 2.0F);
        ASSERT_EQ(schema->entities.size(), 4U);
        EXPECT_EQ(schema->entities[0].kind, "emitter");
        EXPECT_EQ(schema->entities[1].kind, "emitter");
        EXPECT_EQ(schema->entities[2].kind, "sensor");
        EXPECT_EQ(schema->entities[3].kind, "agent");
        for (uint32_t i = 0; i < 4; ++i) {
            EXPECT_EQ(schema->entities[i].id, i + 1);
        }
    }

    TEST_F(NexusStateTest, UnsetOptionalsAreAbsentFromTheSchema)
    {
        add_emitter(source);

        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        const auto schema = read_schema(snap_base);

        ASSERT_TRUE(schema.has_value());
        EXPECT_FALSE(schema->entities[0].position.has_value());
        EXPECT_FALSE(schema->entities[0].color.has_value());
        EXPECT_FALSE(schema->entities[0].size.has_value());
        EXPECT_FALSE(schema->entities[0].orientation.has_value());
    }

    TEST_F(NexusStateTest, AFabricWithNoEntitiesEncodesTheSchemaOnly)
    {
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        const auto schema = read_schema(snap_base);
        ASSERT_TRUE(schema.has_value());
        EXPECT_TRUE(schema->entities.empty());
        EXPECT_TRUE(std::filesystem::exists(snap_base + ".json"));
        EXPECT_FALSE(std::filesystem::exists(snap_base + ".exr"));
    }

    TEST_F(NexusStateTest, AFabricWithOnlyAnExpanseKeepsItAcrossARoundTrip)
    {
        source.add_expanse(std::make_shared<Expanse>(box(2.0F)));
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_TRUE(decoder.last_error().empty());
        EXPECT_EQ(result.constructed, 1);
        ASSERT_EQ(target.all_expanse_ids().size(), 1U);
        const auto restored = target.get_expanse(target.all_expanse_ids().front());
        ASSERT_TRUE(restored->bounds().has_value());
        EXPECT_EQ(restored->bounds()->max, glm::vec3(2.0F));
    }

    TEST_F(NexusStateTest, DecodingAFabricWithNoEntitiesSucceeds)
    {
        source.add_expanse(std::make_shared<Expanse>(box(2.0F)));
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        EXPECT_TRUE(decoder.decode(target, snap_base));
        EXPECT_EQ(decoder.patched_count(), 0U);
        EXPECT_EQ(target.all_expanse_ids().size(), 1U);
    }

    TEST_F(NexusStateTest, WiringRecordsDescribeEachStrategy)
    {
        int routine_runs {};
        add_emitter(source);

        auto every = std::make_shared<Emitter>();
        source.wire(every).every(0.25).for_duration(2.0).times(4).finalise();

        auto steps = std::make_shared<Emitter>();
        source.wire(steps).move_to(glm::vec3(1.0F, 0.0F, 0.0F), 0.5).move_to(glm::vec3(2.0F, 0.0F, 0.0F), 1.5).finalise();

        auto bound = std::make_shared<Emitter>();
        source.wire(bound).bind("on", [] { }, "off", [] { }).finalise();

        auto pending = std::make_shared<Emitter>();
        [[maybe_unused]] auto unfinalised = source.wire(pending);

        auto factory = std::make_shared<Emitter>();
        source.wire(factory).use("make", [&routine_runs]() { return counting_routine(&routine_runs); }).finalise();

        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        const auto schema = read_schema(snap_base);

        ASSERT_TRUE(schema.has_value());
        ASSERT_EQ(schema->entities.size(), 6U);

        EXPECT_EQ(schema->entities[0].wiring.kind, WiringKind::CommitDriven);

        const auto& every_record = schema->entities[1].wiring;
        EXPECT_EQ(every_record.kind, WiringKind::Every);
        ASSERT_TRUE(every_record.interval.has_value());
        EXPECT_DOUBLE_EQ(*every_record.interval, 0.25);
        ASSERT_TRUE(every_record.duration.has_value());
        EXPECT_DOUBLE_EQ(*every_record.duration, 2.0);
        ASSERT_TRUE(every_record.times.has_value());
        EXPECT_EQ(*every_record.times, 4U);

        const auto& steps_record = schema->entities[2].wiring;
        EXPECT_EQ(steps_record.kind, WiringKind::MoveTo);
        ASSERT_TRUE(steps_record.steps.has_value());
        ASSERT_EQ(steps_record.steps->size(), 2U);
        EXPECT_DOUBLE_EQ((*steps_record.steps)[1].delay_seconds, 1.5);

        const auto& bind_record = schema->entities[3].wiring;
        EXPECT_EQ(bind_record.kind, WiringKind::Bind);
        EXPECT_EQ(bind_record.attach_fn_name, "on");
        EXPECT_EQ(bind_record.detach_fn_name, "off");

        EXPECT_EQ(schema->entities[4].wiring.kind, WiringKind::Unsupported);

        const auto& factory_record = schema->entities[5].wiring;
        EXPECT_EQ(factory_record.kind, WiringKind::Factory);
        EXPECT_EQ(factory_record.factory_name, "make");
    }

    TEST_F(NexusStateTest, LocusAndPresenceRecordTheirSubkinds)
    {
        Kinesis::NavigationConfig config;
        config.initial_eye = { 1.0F, 2.0F, 3.0F };
        config.initial_target = { 1.0F, 2.0F, 0.0F };
        source.wire(std::make_shared<Locus>(config, 1.0F, [](const PerceptionContext&) { }, [](const InfluenceContext&) { })).finalise();

        auto presence = std::make_shared<Presence>(4.0F, "see", [](const PerceptionContext&) { }, "act", [](const InfluenceContext&) { }, "spread", [](uint32_t, float) { });
        presence->set_falloff_curve(Presence::FalloffCurve::Cosine);
        presence->set_falloff_radius(2.0F);
        source.wire(presence).finalise();

        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        const auto schema = read_schema(snap_base);

        ASSERT_TRUE(schema.has_value());
        EXPECT_EQ(schema->entities[0].subkind, "locus");
        ASSERT_TRUE(schema->entities[0].locus_nav.has_value());
        EXPECT_EQ(schema->entities[0].locus_nav->eye, glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_EQ(schema->entities[1].subkind, "presence");
        EXPECT_EQ(schema->entities[1].radiate_fn_name, "spread");
        EXPECT_EQ(schema->entities[1].falloff_curve_name, "cosine");
        ASSERT_TRUE(schema->entities[1].falloff_radius.has_value());
        EXPECT_FLOAT_EQ(*schema->entities[1].falloff_radius, 2.0F);
    }

    TEST_F(NexusStateTest, ACustomFalloffIsNotEncodedAndRestoresAsLinear)
    {
        auto presence = std::make_shared<Presence>(4.0F, [](const PerceptionContext&) { }, [](const InfluenceContext&) { }, [](uint32_t, float) { });
        presence->set_falloff([](float) { return 0.5F; });
        source.wire(presence).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        const auto schema = read_schema(snap_base);
        ASSERT_TRUE(schema.has_value());
        EXPECT_TRUE(schema->entities[0].falloff_curve_name.empty());

        const auto result = decoder.reconstruct(target, snap_base);
        const auto restored = std::dynamic_pointer_cast<Presence>(target.get_agent(first_id(target)));
        ASSERT_NE(restored, nullptr);
        ASSERT_TRUE(restored->falloff_curve().has_value());
        EXPECT_EQ(*restored->falloff_curve(), Presence::FalloffCurve::Linear);
    }

    TEST_F(NexusStateTest, ExpanseRecordsKeepBoundsAndNamesAndSkipAnonymousPredicates)
    {
        source.add_expanse(std::make_shared<Expanse>(box(1.0F)));
        source.add_expanse(std::make_shared<Expanse>("left", [](const glm::vec3& p) { return p.x < 0.0F; }, nullptr, nullptr));
        source.add_expanse(std::make_shared<Expanse>([](const glm::vec3&) { return true; }));
        add_emitter(source);

        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        const auto schema = read_schema(snap_base);

        ASSERT_TRUE(schema.has_value());
        ASSERT_EQ(schema->expanses.size(), 2U);
        EXPECT_TRUE(schema->expanses[0].fn_name.empty());
        ASSERT_TRUE(schema->expanses[0].bounds_min.has_value());
        EXPECT_EQ(*schema->expanses[0].bounds_min, glm::vec3(-1.0F));
        EXPECT_EQ(schema->expanses[1].fn_name, "left");
        EXPECT_FALSE(schema->expanses[1].bounds_min.has_value());
    }

    TEST_F(NexusStateTest, ReconstructRebuildsPositionsAndParameters)
    {
        auto full = add_emitter(source, glm::vec3(1.0F, 2.0F, 3.0F));
        full->set_intensity(0.5F);
        full->set_radius(2.0F);
        full->set_color(glm::vec3(0.1F, 0.2F, 0.3F));
        full->set_size(4.0F);
        auto plain = add_emitter(source, glm::vec3(-4.0F, 5.0F, 6.0F));
        plain->set_intensity(2.0F);
        plain->set_radius(1.0F);
        auto sensor = std::make_shared<Sensor>(3.0F);
        sensor->set_position(glm::vec3(7.0F, 8.0F, 9.0F));
        source.wire(sensor).finalise();
        auto agent = std::make_shared<Agent>(5.0F);
        agent->set_position(glm::vec3(9.0F, -9.0F, 0.5F));
        const glm::quat spin = glm::angleAxis(glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F));
        agent->set_orientation(spin);
        agent->set_intensity(0.25F);
        agent->set_radius(7.0F);
        agent->set_color(glm::vec3(1.0F, 0.0F, 0.5F));
        agent->set_size(2.0F);
        source.wire(agent).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_EQ(result.constructed, 4);
        EXPECT_EQ(result.patched, 0);

        const auto e1 = target.get_emitter(1);
        ASSERT_NE(e1, nullptr);
        EXPECT_NEAR(e1->position()->x, 1.0F, 1e-4F);
        EXPECT_NEAR(e1->position()->y, 2.0F, 1e-4F);
        EXPECT_NEAR(e1->position()->z, 3.0F, 1e-4F);
        EXPECT_NEAR(e1->intensity(), 0.5F, 1e-4F);
        EXPECT_NEAR(e1->radius(), 2.0F, 1e-4F);
        ASSERT_TRUE(e1->color().has_value());
        EXPECT_NEAR(e1->color()->g, 0.2F, 1e-4F);
        ASSERT_TRUE(e1->size().has_value());
        EXPECT_NEAR(*e1->size(), 4.0F, 1e-4F);

        const auto e2 = target.get_emitter(2);
        ASSERT_NE(e2, nullptr);
        EXPECT_NEAR(e2->position()->x, -4.0F, 1e-4F);
        EXPECT_NEAR(e2->intensity(), 2.0F, 1e-4F);
        EXPECT_FALSE(e2->color().has_value());
        EXPECT_FALSE(e2->size().has_value());

        const auto s = target.get_sensor(3);
        ASSERT_NE(s, nullptr);
        EXPECT_NEAR(s->query_radius(), 3.0F, 1e-4F);
        EXPECT_NEAR(s->position()->y, 8.0F, 1e-4F);

        const auto a = target.get_agent(4);
        ASSERT_NE(a, nullptr);
        EXPECT_NEAR(a->query_radius(), 5.0F, 1e-4F);
        EXPECT_NEAR(a->radius(), 7.0F, 1e-4F);
        EXPECT_NEAR(a->intensity(), 0.25F, 1e-4F);
        ASSERT_TRUE(a->orientation().has_value());
        EXPECT_NEAR(std::abs(glm::dot(*a->orientation(), spin)), 1.0F, 1e-5F);
        ASSERT_TRUE(a->color().has_value());
        EXPECT_NEAR(a->color()->b, 0.5F, 1e-4F);
    }

    TEST_F(NexusStateTest, ReconstructKeepsKindsAndIds)
    {
        add_emitter(source);
        source.wire(std::make_shared<Sensor>(1.0F)).finalise();
        source.wire(std::make_shared<Agent>(1.0F)).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        restore(target, snap_base);

        EXPECT_EQ(target.all_ids(), (std::vector<uint32_t> { 1, 2, 3 }));
        EXPECT_EQ(target.kind(1), Fabric::Kind::Emitter);
        EXPECT_EQ(target.kind(2), Fabric::Kind::Sensor);
        EXPECT_EQ(target.kind(3), Fabric::Kind::Agent);
    }

    TEST_F(NexusStateTest, ReconstructWarnsWhenARuntimeIdDiffersFromTheSavedOne)
    {
        add_emitter(source);
        const auto dropped = add_emitter(source);
        add_emitter(source);
        source.remove(dropped->id());
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_EQ(result.constructed, 2);
        EXPECT_EQ(target.all_ids(), (std::vector<uint32_t> { 1, 2 }));
        EXPECT_TRUE(has_warning(result.warnings, "runtime_id=2"));
    }

    TEST_F(NexusStateTest, ReconstructResolvesNamedFunctionsFromTheRegistry)
    {
        source.wire(std::make_shared<Emitter>("pulse", [](const InfluenceContext&) { })).finalise();
        source.wire(std::make_shared<Sensor>(1.0F, "watch", [](const PerceptionContext&) { })).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        prime_influence(target, "pulse", [this](const InfluenceContext&) { ++target_pulse; });
        prime_perception(target, "watch", [this](const PerceptionContext&) { ++target_watch; });

        restore(target, snap_base);
        target.commit();

        EXPECT_EQ(target_pulse, 1);
        EXPECT_EQ(target_watch, 1);
        const auto restored = target.get_emitter(first_id(target));
        ASSERT_NE(restored, nullptr);
        EXPECT_EQ(restored->fn_name(), "pulse");
    }

    TEST_F(NexusStateTest, ReconstructLeavesUnknownFunctionsEmptyAndWarns)
    {
        source.wire(std::make_shared<Emitter>("pulse", [](const InfluenceContext&) { })).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        const auto result = decoder.reconstruct(target, snap_base);

        const auto emitter = target.get_emitter(first_id(target));
        ASSERT_NE(emitter, nullptr);
        EXPECT_EQ(emitter->fn_name(), "pulse");
        EXPECT_FALSE(emitter->fn());
        EXPECT_TRUE(has_warning(result.warnings, "unknown 'pulse'"));
    }

    TEST_F(NexusStateTest, AnEveryWiringWithTimesRestoresAndStopsAfterThatManyFirings)
    {
        source.wire(std::make_shared<Emitter>("pulse", [](const InfluenceContext&) { })).every(0.001).times(3).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        prime_influence(target, "pulse", [this](const InfluenceContext&) { ++target_pulse; });

        restore(target, snap_base);
        step_samples(48UL * 20UL);

        EXPECT_EQ(target_pulse, 3);
    }

    TEST_F(NexusStateTest, EveryTokensAndDurationSurviveARoundTrip)
    {
        auto emitter = std::make_shared<Emitter>();
        source.wire(emitter)
            .every(0.05, Vruta::ProcessingToken::FRAME_ACCURATE)
            .for_duration(1.0, Vruta::ProcessingToken::FRAME_ACCURATE)
            .finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        restore(target, snap_base);
        ASSERT_TRUE(encoder.encode(target, again_base));
        const auto schema = read_schema(again_base);

        ASSERT_TRUE(schema.has_value());
        const auto& wiring = schema->entities[0].wiring;
        EXPECT_EQ(wiring.kind, WiringKind::Every);
        EXPECT_DOUBLE_EQ(*wiring.interval, 0.05);
        EXPECT_DOUBLE_EQ(*wiring.duration, 1.0);
        ASSERT_TRUE(wiring.interval_token.has_value());
        EXPECT_EQ(*wiring.interval_token, Vruta::ProcessingToken::FRAME_ACCURATE);
        ASSERT_TRUE(wiring.duration_token.has_value());
        EXPECT_EQ(*wiring.duration_token, Vruta::ProcessingToken::FRAME_ACCURATE);
    }

    TEST_F(NexusStateTest, AChoreographyRestoresItsStepsAndRepeatCount)
    {
        auto emitter = std::make_shared<Emitter>();
        source.wire(emitter).move_to(glm::vec3(1.0F, 0.0F, 0.0F), 0.5).move_to(glm::vec3(2.0F, 0.0F, 0.0F), 1.5).times(2).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        restore(target, snap_base);

        const auto* wiring = target.wiring_for(first_id(target));
        ASSERT_NE(wiring, nullptr);
        ASSERT_EQ(wiring->move_steps().size(), 2U);
        EXPECT_EQ(wiring->move_steps()[1].position, glm::vec3(2.0F, 0.0F, 0.0F));
        EXPECT_DOUBLE_EQ(wiring->move_steps()[1].delay_seconds, 1.5);
        EXPECT_EQ(wiring->times_count(), 2U);
    }

    TEST_F(NexusStateTest, ABindWiringFallsBackToCommitDrivenWithAWarning)
    {
        source.wire(std::make_shared<Emitter>("pulse", [](const InfluenceContext&) { })).bind("on", [] { }, "off", [] { }).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        prime_influence(target, "pulse", [this](const InfluenceContext&) { ++target_pulse; });

        const auto result = decoder.reconstruct(target, snap_base);
        target.commit();

        EXPECT_TRUE(has_warning(result.warnings, "bind callables cannot be restored"));
        EXPECT_EQ(target_pulse, 1);
        const auto* wiring = target.wiring_for(first_id(target));
        ASSERT_NE(wiring, nullptr);
        EXPECT_FALSE(wiring->has_bind());
    }

    TEST_F(NexusStateTest, AFactoryWiringFallsBackToCommitDrivenWithAWarning)
    {
        int runs {};
        source.wire(std::make_shared<Emitter>()).use("make", [&runs]() { return counting_routine(&runs); }).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_TRUE(has_warning(result.warnings, "factory 'make' cannot be restored"));
    }

    TEST_F(NexusStateTest, ReconstructPatchesEntitiesThatAlreadyExistById)
    {
        auto saved = add_emitter(source, glm::vec3(1.0F, 2.0F, 3.0F));
        saved->set_intensity(0.75F);
        auto sensor = std::make_shared<Sensor>(4.0F);
        sensor->set_position(glm::vec3(5.0F));
        source.wire(sensor).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        auto live_emitter = add_emitter(target, glm::vec3(50.0F));
        auto live_sensor = std::make_shared<Sensor>(1.0F);
        live_sensor->set_position(glm::vec3(60.0F));
        target.wire(live_sensor).finalise();

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_EQ(result.patched, 2);
        EXPECT_EQ(result.constructed, 0);
        EXPECT_NEAR(live_emitter->position()->x, 1.0F, 1e-4F);
        EXPECT_NEAR(live_emitter->intensity(), 0.75F, 1e-4F);
        EXPECT_NEAR(live_sensor->query_radius(), 4.0F, 1e-4F);
        EXPECT_NEAR(live_sensor->position()->z, 5.0F, 1e-4F);
    }

    TEST_F(NexusStateTest, PatchingClearsAPositionTheSchemaDoesNotHave)
    {
        add_emitter(source);
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        auto live = add_emitter(target, glm::vec3(5.0F));

        restore(target, snap_base);

        EXPECT_FALSE(live->position().has_value());
    }

    TEST_F(NexusStateTest, AnIdHeldByAnotherKindIsSkipped)
    {
        add_emitter(source);
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        target.wire(std::make_shared<Sensor>(1.0F)).finalise();

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_EQ(result.skipped, 1);
        EXPECT_EQ(result.patched, 0);
        EXPECT_EQ(result.constructed, 0);
    }

    TEST_F(NexusStateTest, ALocusRoundTripsItsNavigation)
    {
        Kinesis::NavigationConfig config;
        config.initial_eye = { 1.0F, 2.0F, 3.0F };
        config.initial_target = { 4.0F, 2.0F, 0.0F };
        config.fov_radians = 1.0F;
        config.move_speed = 7.0F;
        auto locus = std::make_shared<Locus>(config, 2.0F, [](const PerceptionContext&) { }, [](const InfluenceContext&) { });
        locus->set_aspect(2.0F);
        source.wire(locus).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        const auto result = decoder.reconstruct(target, snap_base);

        const auto restored = std::dynamic_pointer_cast<Locus>(target.get_agent(first_id(target)));
        ASSERT_NE(restored, nullptr);
        EXPECT_NEAR(restored->nav().eye.x, 1.0F, 1e-4F);
        EXPECT_NEAR(restored->nav().eye.z, 3.0F, 1e-4F);
        EXPECT_NEAR(restored->nav().fov_radians, 1.0F, 1e-5F);
        EXPECT_NEAR(restored->nav().move_speed, 7.0F, 1e-5F);
        EXPECT_NEAR(restored->nav().yaw, locus->nav().yaw, 1e-4F);
        EXPECT_NEAR(restored->nav().pitch, locus->nav().pitch, 1e-4F);
        EXPECT_NEAR(restored->aspect(), 2.0F, 1e-5F);
        EXPECT_TRUE(has_warning(result.warnings, "view_targets must be reconnected"));
    }

    TEST_F(NexusStateTest, APresenceRoundTripsItsFalloffAndNames)
    {
        auto presence = std::make_shared<Presence>(4.0F, "see", [](const PerceptionContext&) { }, "act", [](const InfluenceContext&) { }, "spread", [](uint32_t, float) { });
        presence->set_falloff_curve(Presence::FalloffCurve::Exponential);
        presence->set_falloff_radius(2.0F);
        source.wire(presence).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        const auto result = decoder.reconstruct(target, snap_base);

        const auto restored = std::dynamic_pointer_cast<Presence>(target.get_agent(first_id(target)));
        ASSERT_NE(restored, nullptr);
        ASSERT_TRUE(restored->falloff_curve().has_value());
        EXPECT_EQ(*restored->falloff_curve(), Presence::FalloffCurve::Exponential);
        EXPECT_FLOAT_EQ(restored->falloff_radius(), 2.0F);
        EXPECT_EQ(restored->radiate_fn_name(), "spread");
        EXPECT_TRUE(has_warning(result.warnings, "radiate_fn: unknown 'spread'"));
    }

    TEST_F(NexusStateTest, APresenceRestoresItsRegisteredFunctions)
    {
        auto presence = std::make_shared<Presence>(4.0F, "see", [](const PerceptionContext&) { }, "act", [](const InfluenceContext&) { }, "spread", [](uint32_t, float) { });
        source.wire(presence).finalise();
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        int perceived = 0;
        int influenced = 0;
        int radiated = 0;
        target.register_perception("see", [&perceived](const PerceptionContext&) { ++perceived; });
        target.register_influence("act", [&influenced](const InfluenceContext&) { ++influenced; });
        target.register_radiate("spread", [&radiated](uint32_t, float) { ++radiated; });

        const auto result = decoder.reconstruct(target, snap_base);

        const auto restored = std::dynamic_pointer_cast<Presence>(target.get_agent(first_id(target)));
        ASSERT_NE(restored, nullptr);
        EXPECT_FALSE(has_warning(result.warnings, "unknown"));
        ASSERT_TRUE(static_cast<bool>(restored->radiate_fn()));
        restored->radiate_fn()(1U, 0.5F);
        EXPECT_EQ(radiated, 1);
        restored->invoke_perception(PerceptionContext {});
        restored->invoke_influence(InfluenceContext {});
        EXPECT_EQ(perceived, 1);
        EXPECT_EQ(influenced, 1);
    }

    TEST_F(NexusStateTest, WiringAPresenceRegistersItsRadiateFunction)
    {
        source.wire(std::make_shared<Presence>(4.0F, "see", [](const PerceptionContext&) { }, "act", [](const InfluenceContext&) { }, "spread", [](uint32_t, float) { })).finalise();

        EXPECT_NE(source.resolve_radiate_fn("spread"), nullptr);
        EXPECT_EQ(source.resolve_radiate_fn("other"), nullptr);
    }

    TEST_F(NexusStateTest, AddingAnExpanseRegistersItsNamedFunctions)
    {
        int entered = 0;
        int exited = 0;
        source.add_expanse(std::make_shared<Expanse>(
            "left", "arrive", "depart",
            [](const glm::vec3& p) { return p.x < 0.0F; },
            [&entered](uint32_t) { ++entered; },
            [&exited](uint32_t) { ++exited; }));

        const auto contains = source.resolve_expanse_fn("left");
        const auto arrive = source.resolve_crossing_fn("arrive");
        const auto depart = source.resolve_crossing_fn("depart");
        ASSERT_NE(contains, nullptr);
        ASSERT_NE(arrive, nullptr);
        ASSERT_NE(depart, nullptr);
        EXPECT_TRUE((*contains)(glm::vec3(-1.0F, 0.0F, 0.0F)));
        EXPECT_FALSE((*contains)(glm::vec3(1.0F, 0.0F, 0.0F)));
        (*arrive)(1U);
        (*depart)(1U);
        EXPECT_EQ(entered, 1);
        EXPECT_EQ(exited, 1);
    }

    TEST_F(NexusStateTest, AddingAnExpanseWithoutNamesOrCallablesRegistersNothing)
    {
        auto named_only = std::make_shared<Expanse>(box(1.0F));
        named_only->set_fn_name("ghost");
        named_only->set_on_enter_fn_name("phantom");
        source.add_expanse(std::make_shared<Expanse>(box(1.0F)));
        source.add_expanse(named_only);

        EXPECT_EQ(source.resolve_expanse_fn("ghost"), nullptr);
        EXPECT_EQ(source.resolve_crossing_fn("phantom"), nullptr);
    }

    TEST_F(NexusStateTest, RegisteringANameAgainReplacesTheFunction)
    {
        int first = 0;
        int second = 0;
        target.register_crossing("arrive", [&first](uint32_t) { ++first; });
        target.register_crossing("arrive", [&second](uint32_t) { ++second; });

        (*target.resolve_crossing_fn("arrive"))(1U);

        EXPECT_EQ(first, 0);
        EXPECT_EQ(second, 1);
    }

    TEST_F(NexusStateTest, RegisteringAnEmptyNameIsIgnored)
    {
        target.register_contains("", [](const glm::vec3&) { return true; });

        EXPECT_EQ(target.resolve_expanse_fn(""), nullptr);
    }

    TEST_F(NexusStateTest, ANamedPredicateExpanseRestoresWhenTheTargetRegistersIt)
    {
        source.add_expanse(std::make_shared<Expanse>("left", [](const glm::vec3& p) { return p.x < 0.0F; }, nullptr, nullptr));
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        target.register_contains("left", [](const glm::vec3& p) { return p.x < 0.0F; });

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_FALSE(has_warning(result.warnings, "not in registry"));
        ASSERT_EQ(target.all_expanse_ids().size(), 1U);
        const auto restored = target.get_expanse(target.all_expanse_ids().front());
        EXPECT_EQ(restored->fn_name(), "left");
        EXPECT_TRUE(restored->contains(glm::vec3(-1.0F, 0.0F, 0.0F)));
        EXPECT_FALSE(restored->contains(glm::vec3(1.0F, 0.0F, 0.0F)));
    }

    TEST_F(NexusStateTest, AnExpanseCrossingRestoresWhenTheTargetRegistersIt)
    {
        auto expanse = std::make_shared<Expanse>(box(1.0F));
        expanse->set_on_enter_fn_name("arrive");
        expanse->set_on_exit_fn_name("depart");
        source.add_expanse(expanse);
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        int arrived = 0;
        int departed = 0;
        target.register_crossing("arrive", [&arrived](uint32_t) { ++arrived; });
        target.register_crossing("depart", [&departed](uint32_t) { ++departed; });

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_FALSE(has_warning(result.warnings, "not in registry"));
        ASSERT_EQ(target.all_expanse_ids().size(), 1U);
        const auto restored = target.get_expanse(target.all_expanse_ids().front());
        ASSERT_TRUE(static_cast<bool>(restored->on_enter_fn()));
        ASSERT_TRUE(static_cast<bool>(restored->on_exit_fn()));
        restored->on_enter_fn()(1U);
        restored->on_exit_fn()(1U);
        EXPECT_EQ(arrived, 1);
        EXPECT_EQ(departed, 1);
    }

    TEST_F(NexusStateTest, ABoxExpanseRestoresAsAWorkingRegion)
    {
        source.add_expanse(std::make_shared<Expanse>(box(1.0F)));
        add_emitter(source);
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_EQ(result.constructed, 2);
        ASSERT_EQ(target.all_expanse_ids().size(), 1U);
        const auto restored = target.get_expanse(target.all_expanse_ids().front());
        EXPECT_TRUE(restored->fn_name().empty());
        EXPECT_TRUE(restored->contains(glm::vec3(0.5F)));
        EXPECT_FALSE(restored->contains(glm::vec3(5.0F)));
    }

    TEST_F(NexusStateTest, ANamedPredicateExpanseIsSkippedWhenNothingRegistersIt)
    {
        source.add_expanse(std::make_shared<Expanse>("left", [](const glm::vec3& p) { return p.x < 0.0F; }, nullptr, nullptr));
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_EQ(result.skipped, 1);
        EXPECT_TRUE(target.all_expanse_ids().empty());
        EXPECT_TRUE(has_warning(result.warnings, "not in registry"));
    }

    TEST_F(NexusStateTest, AnEquivalentExpanseAlreadyOnTheFabricIsPatchedNotDuplicated)
    {
        source.add_expanse(std::make_shared<Expanse>(box(1.0F)));
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        target.add_expanse(std::make_shared<Expanse>(box(1.0F)));

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_EQ(result.patched, 1);
        EXPECT_EQ(result.constructed, 0);
        EXPECT_EQ(target.all_expanse_ids().size(), 1U);
    }

    TEST_F(NexusStateTest, AnExpanseCrossingNameThatCannotBeResolvedIsKeptButEmpty)
    {
        auto expanse = std::make_shared<Expanse>(box(1.0F));
        expanse->set_on_enter_fn_name("arrive");
        source.add_expanse(expanse);
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_TRUE(has_warning(result.warnings, "crossing 'arrive' not in registry"));
        ASSERT_EQ(target.all_expanse_ids().size(), 1U);
        EXPECT_EQ(target.get_expanse(target.all_expanse_ids().front())->on_enter_fn_name(), "arrive");
    }

    TEST_F(NexusStateTest, DecodePatchesExistingEntitiesAndCountsTheMissing)
    {
        auto first = add_emitter(source, glm::vec3(1.0F));
        first->set_intensity(0.5F);
        add_emitter(source, glm::vec3(2.0F));
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        auto live = add_emitter(target, glm::vec3(50.0F));

        EXPECT_TRUE(decoder.decode(target, snap_base));

        EXPECT_EQ(decoder.patched_count(), 1U);
        EXPECT_EQ(decoder.missing_count(), 1U);
        EXPECT_NEAR(live->position()->x, 1.0F, 1e-4F);
        EXPECT_EQ(target.all_ids().size(), 1U);
    }

    TEST_F(NexusStateTest, DecodeFailsWithAnErrorWhenTheFilesAreMissing)
    {
        EXPECT_FALSE(decoder.decode(target, snap_base + "_missing"));
        EXPECT_NE(decoder.last_error().find("Failed to load schema"), std::string::npos);
    }

    TEST_F(NexusStateTest, DecodeFailsWhenTheImageIsMissing)
    {
        add_emitter(source);
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        std::filesystem::remove(snap_base + ".exr");

        EXPECT_FALSE(decoder.decode(target, snap_base));
        EXPECT_NE(decoder.last_error().find("Failed to load EXR"), std::string::npos);
    }

    TEST_F(NexusStateTest, AnUnsupportedSchemaVersionIsRejected)
    {
        add_emitter(source);
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        edit_schema(snap_base, [](nlohmann::json& json) { json["version"] = 9999; });

        EXPECT_FALSE(decoder.decode(target, snap_base));
        EXPECT_NE(decoder.last_error().find("Unsupported schema version"), std::string::npos);

        const auto result = decoder.reconstruct(target, snap_base);
        EXPECT_EQ(result.constructed, 0);
        EXPECT_FALSE(decoder.last_error().empty());
    }

    TEST_F(NexusStateTest, AnUnknownKindIsSkippedWithAWarning)
    {
        add_emitter(source);
        ASSERT_TRUE(encoder.encode(source, snap_base)) << encoder.last_error();
        edit_schema(snap_base, [](nlohmann::json& json) { json["entities"][0]["kind"] = "banana"; });

        const auto result = decoder.reconstruct(target, snap_base);

        EXPECT_EQ(result.skipped, 1);
        EXPECT_EQ(result.constructed, 0);
        EXPECT_TRUE(has_warning(result.warnings, "Unknown kind 'banana'"));
    }

    class NexusTapestryStateTest : public NexusStateTest {
    protected:
        std::shared_ptr<Tapestry> make_tapestry()
        {
            return std::make_shared<Tapestry>(frames.scheduler, events, buffers);
        }

        void SetUp() override
        {
            NexusStateTest::SetUp();
            std::filesystem::create_directories(act_dir);
        }
    };

    TEST_F(NexusTapestryStateTest, ATapestryRoundTripsFabricsExpansesAndUserState)
    {
        auto original = make_tapestry();
        auto alpha = original->create_fabric("alpha", 2.0F);
        auto beta = original->create_fabric("beta", 3.0F);
        add_emitter(*alpha, glm::vec3(1.0F));
        add_emitter(*beta, glm::vec3(2.0F));
        add_emitter(*beta, glm::vec3(3.0F));
        const auto zone = original->create_expanse("zone", box(4.0F));
        alpha->add_expanse(zone);
        beta->add_expanse(zone);

        ASSERT_TRUE(encoder.encode(*original, act_dir, nlohmann::json { { "scene", 7 } }));

        EXPECT_TRUE(std::filesystem::exists(act_dir + "/tapestry.json"));
        EXPECT_TRUE(std::filesystem::exists(act_dir + "/alpha.json"));
        EXPECT_TRUE(std::filesystem::exists(act_dir + "/beta.exr"));

        auto restored = make_tapestry();
        const auto result = decoder.reconstruct(*restored, act_dir);

        EXPECT_TRUE(decoder.last_error().empty());
        const auto restored_alpha = restored->get_fabric("alpha");
        const auto restored_beta = restored->get_fabric("beta");
        ASSERT_NE(restored_alpha, nullptr);
        ASSERT_NE(restored_beta, nullptr);
        EXPECT_EQ(restored_alpha->all_ids().size(), 1U);
        EXPECT_EQ(restored_beta->all_ids().size(), 2U);

        const auto restored_zone = restored->get_expanse("zone");
        ASSERT_NE(restored_zone, nullptr);
        ASSERT_TRUE(restored_zone->bounds().has_value());
        EXPECT_EQ(restored_zone->bounds()->max, glm::vec3(4.0F));
        EXPECT_EQ(restored_alpha->get_expanse(restored_zone->id()), restored_zone);
        EXPECT_EQ(restored_beta->get_expanse(restored_zone->id()), restored_zone);
        ASSERT_TRUE(result.user_state.contains("scene"));
        EXPECT_EQ(result.user_state.at("scene").get<int>(), 7);
    }

    TEST_F(NexusTapestryStateTest, FabricCellSizesSurviveARoundTrip)
    {
        auto original = make_tapestry();
        auto alpha = original->create_fabric("alpha", 2.0F);
        auto beta = original->create_fabric("beta", 3.0F);
        add_emitter(*alpha);
        add_emitter(*beta);
        ASSERT_TRUE(encoder.encode(*original, act_dir));

        auto restored = make_tapestry();
        restore(*restored, act_dir);
        std::filesystem::create_directories(act2_dir);
        ASSERT_TRUE(encoder.encode(*restored, act2_dir));

        EXPECT_FLOAT_EQ(*read_schema(act2_dir + "/alpha")->cell_size, 2.0F);
        EXPECT_FLOAT_EQ(*read_schema(act2_dir + "/beta")->cell_size, 3.0F);
    }

    TEST_F(NexusTapestryStateTest, TapestryOwnedExpansesAreNotRepeatedInTheFabricFiles)
    {
        auto original = make_tapestry();
        auto alpha = original->create_fabric("alpha");
        add_emitter(*alpha);
        const auto zone = original->create_expanse("zone", box(1.0F));
        alpha->add_expanse(zone);
        alpha->add_expanse(std::make_shared<Expanse>(box(2.0F)));

        ASSERT_TRUE(encoder.encode(*original, act_dir));
        const auto schema = read_schema(act_dir + "/alpha");

        ASSERT_TRUE(schema.has_value());
        ASSERT_EQ(schema->expanses.size(), 1U);
        EXPECT_EQ(*schema->expanses[0].bounds_max, glm::vec3(2.0F));
    }

    TEST_F(NexusTapestryStateTest, AnEmptyFabricInATapestryStillEncodesEverything)
    {
        auto original = make_tapestry();
        add_emitter(*original->create_fabric("alpha"));
        const auto empty = original->create_fabric("beta");
        ASSERT_NE(empty, nullptr);

        ASSERT_TRUE(encoder.encode(*original, act_dir));

        EXPECT_TRUE(std::filesystem::exists(act_dir + "/tapestry.json"));
        EXPECT_TRUE(std::filesystem::exists(act_dir + "/beta.json"));
        EXPECT_FALSE(std::filesystem::exists(act_dir + "/beta.exr"));

        auto restored = make_tapestry();
        restore(*restored, act_dir);
        ASSERT_NE(restored->get_fabric("beta"), nullptr);
        EXPECT_TRUE(restored->get_fabric("beta")->all_ids().empty());
        EXPECT_EQ(restored->get_fabric("alpha")->all_ids().size(), 1U);
    }

    TEST_F(NexusTapestryStateTest, AnUnnamedFabricIsStoredUnderItsId)
    {
        auto original = make_tapestry();
        const auto unnamed = original->create_fabric(1.0F);
        add_emitter(*unnamed);

        ASSERT_TRUE(encoder.encode(*original, act_dir));

        EXPECT_TRUE(std::filesystem::exists(act_dir + "/" + std::to_string(unnamed->id()) + ".json"));
    }

    TEST_F(NexusTapestryStateTest, ReconstructPatchesAFabricAndExpanseTheTapestryAlreadyHolds)
    {
        auto original = make_tapestry();
        auto alpha = original->create_fabric("alpha");
        add_emitter(*alpha, glm::vec3(1.0F));
        const auto zone = original->create_expanse("zone", box(4.0F));
        alpha->add_expanse(zone);
        ASSERT_TRUE(encoder.encode(*original, act_dir));

        auto restored = make_tapestry();
        auto live = restored->create_fabric("alpha");
        auto live_emitter = add_emitter(*live, glm::vec3(50.0F));
        const auto live_zone = restored->create_expanse("zone", box(1.0F));

        const auto result = decoder.reconstruct(*restored, act_dir);

        EXPECT_EQ(restored->get_fabric("alpha"), live);
        EXPECT_NEAR(live_emitter->position()->x, 1.0F, 1e-4F);
        EXPECT_EQ(restored->get_expanse("zone"), live_zone);
        EXPECT_EQ(live_zone->bounds()->max, glm::vec3(4.0F));
        EXPECT_GE(result.patched, 2);
    }

    TEST_F(NexusTapestryStateTest, ReconstructFailsWhenTheTapestryFileIsMissing)
    {
        auto restored = make_tapestry();

        const auto result = decoder.reconstruct(*restored, act_dir);

        EXPECT_EQ(result.constructed, 0);
        EXPECT_NE(decoder.last_error().find("Failed to load tapestry schema"), std::string::npos);
        EXPECT_TRUE(restored->all_fabrics().empty());
    }

}
}
