#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "io_manager_fixture.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {

namespace {

    class IOManagerLifecycleTest : public IOManagerFixture { };
    class IOManagerRegistryTest : public IOManagerFixture { };
    class IOManagerDispatchTest : public IOManagerFixture { };
    class IOManagerNullArgumentTest : public IOManagerFixture { };
    class IOManagerConcurrencyTest : public IOManagerFixture { };

    bool service_registered()
    {
        return Registry::BackendRegistry::instance().has_service<Registry::Service::IOService>();
    }

}

// =========================================================================
// Construction and the IOService registration
// =========================================================================

TEST_F(IOManagerLifecycleTest, ConstructionRegistersTheIOServiceWithBothDispatchers)
{
    ASSERT_TRUE(service_registered());
    auto* service = io_service();
    ASSERT_NE(service, nullptr);
    EXPECT_TRUE(static_cast<bool>(service->request_decode));
    EXPECT_TRUE(static_cast<bool>(service->request_frame));
}

TEST_F(IOManagerLifecycleTest, ANewManagerHoldsNothing)
{
    EXPECT_TRUE(manager->get_video_reader_ids().empty());
    EXPECT_TRUE(manager->get_camera_reader_ids().empty());
    EXPECT_TRUE(manager->get_audio_capture_ids().empty());
    EXPECT_TRUE(manager->get_video_capture_ids().empty());
    EXPECT_TRUE(manager->get_volume_capture_ids().empty());
    EXPECT_TRUE(manager->get_spatial_capture_ids().empty());
    EXPECT_TRUE(manager->get_audio_readers().empty());
    EXPECT_TRUE(manager->get_image_readers().empty());
    EXPECT_TRUE(manager->get_model_readers().empty());
    EXPECT_TRUE(manager->get_sound_writers().empty());
    EXPECT_TRUE(manager->get_video_writers().empty());
    EXPECT_TRUE(manager->get_loaded_volumes().empty());
}

TEST_F(IOManagerLifecycleTest, ShutdownUnregistersTheIOService)
{
    manager->shutdown();

    EXPECT_FALSE(service_registered());
    EXPECT_EQ(io_service(), nullptr);
}

TEST_F(IOManagerLifecycleTest, StopLeavesTheIOServiceRegistered)
{
    manager->stop();

    EXPECT_TRUE(service_registered());
    ASSERT_NE(io_service(), nullptr);
    EXPECT_TRUE(static_cast<bool>(io_service()->request_decode));
}

TEST_F(IOManagerLifecycleTest, StopIsIdempotent)
{
    manager->stop();
    manager->stop();
    manager->stop();

    EXPECT_TRUE(service_registered());
}

TEST_F(IOManagerLifecycleTest, ShutdownIsIdempotent)
{
    manager->shutdown();
    manager->shutdown();

    EXPECT_FALSE(service_registered());
}

TEST_F(IOManagerLifecycleTest, ShutdownAfterStopStillUnregisters)
{
    manager->stop();
    manager->shutdown();

    EXPECT_FALSE(service_registered());
}

TEST_F(IOManagerLifecycleTest, StopAfterShutdownIsHarmless)
{
    manager->shutdown();
    EXPECT_NO_THROW(manager->stop());
    EXPECT_FALSE(service_registered());
}

TEST_F(IOManagerLifecycleTest, DestructionWithoutShutdownUnregistersTheIOService)
{
    ASSERT_TRUE(service_registered());

    manager.reset();

    EXPECT_FALSE(service_registered());
}

TEST_F(IOManagerLifecycleTest, DestructionAfterExplicitShutdownIsClean)
{
    manager->shutdown();

    EXPECT_NO_THROW(manager.reset());
    EXPECT_FALSE(service_registered());
}

TEST_F(IOManagerLifecycleTest, ADroppedManagerCanBeFollowedByAnother)
{
    rebuild();

    ASSERT_TRUE(service_registered());
    ASSERT_NE(io_service(), nullptr);
    EXPECT_TRUE(static_cast<bool>(io_service()->request_decode));
    EXPECT_TRUE(manager->get_video_reader_ids().empty());
}

TEST_F(IOManagerLifecycleTest, ReaderIdsRestartForAFreshManager)
{
    const auto first = manager->register_camera_source(std::make_shared<CountingCameraSource>());
    rebuild();
    const auto second = manager->register_camera_source(std::make_shared<CountingCameraSource>());

    EXPECT_EQ(first, second);
}

TEST_F(IOManagerLifecycleTest, ShutdownReleasesEveryReader)
{
    const auto video = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());
    const auto camera = manager->register_camera_source(std::make_shared<CountingCameraSource>());
    ASSERT_NE(manager->get_video_reader(video), nullptr);
    ASSERT_NE(manager->get_camera_reader(camera), nullptr);

    manager->shutdown();

    EXPECT_TRUE(manager->get_video_reader_ids().empty());
    EXPECT_TRUE(manager->get_camera_reader_ids().empty());
    EXPECT_EQ(manager->get_video_reader(video), nullptr);
    EXPECT_EQ(manager->get_camera_reader(camera), nullptr);
}

TEST_F(IOManagerLifecycleTest, StopKeepsReadersAliveForTheThreadsThatConsumeThem)
{
    const auto video = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());
    const auto camera = manager->register_camera_source(std::make_shared<CountingCameraSource>());

    manager->stop();

    EXPECT_NE(manager->get_video_reader(video), nullptr);
    EXPECT_NE(manager->get_camera_reader(camera), nullptr);
}

TEST_F(IOManagerLifecycleTest, AReaderHandedOutBeforeShutdownOutlivesIt)
{
    auto camera = std::make_shared<CountingCameraSource>();
    const auto id = manager->register_camera_source(camera);
    const std::weak_ptr<CountingCameraSource> weak = camera;
    camera.reset();
    auto held = std::static_pointer_cast<CountingCameraSource>(manager->get_camera_reader(id));
    ASSERT_NE(held, nullptr);

    manager->shutdown();

    EXPECT_FALSE(weak.expired());
    held->pull_frame_all();
    EXPECT_EQ(held->pulls.load(), 1);
}

TEST_F(IOManagerLifecycleTest, ShutdownReleasesAReaderNobodyElseHolds)
{
    auto camera = std::make_shared<CountingCameraSource>();
    const std::weak_ptr<CountingCameraSource> weak = camera;
    static_cast<void>(manager->register_camera_source(std::move(camera)));
    ASSERT_FALSE(weak.expired());

    manager->shutdown();

    EXPECT_TRUE(weak.expired());
}

TEST_F(IOManagerLifecycleTest, ShutdownReleasesSoundWritersAndTheirFiles)
{
    auto writer = manager->create_writer(path_of("held.wav"), 2, k_rate);
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(manager->get_sound_writers().size(), 1U);

    manager->shutdown();

    EXPECT_TRUE(manager->get_sound_writers().empty());
    EXPECT_FALSE(writer->is_open());
}

TEST_F(IOManagerLifecycleTest, StopClosesWritersButKeepsThemListed)
{
    auto writer = manager->create_writer(path_of("held.wav"), 2, k_rate);
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(writer->is_open());

    manager->stop();

    EXPECT_FALSE(writer->is_open());
    EXPECT_EQ(manager->get_sound_writers().size(), 1U);
}

TEST_F(IOManagerLifecycleTest, StopFinalisesAnOutstandingAudioCapture)
{
    FakeAudioBackend backend;
    const auto id = manager->capture_output(path_of("take.wav"));
    ASSERT_NE(id, 0U);
    ASSERT_EQ(backend.observer_count(), 1U);

    manager->stop();

    EXPECT_TRUE(manager->get_audio_capture_ids().empty());
    EXPECT_EQ(backend.observer_count(), 0U);
    EXPECT_EQ(backend.unregistered_ids(), std::vector<uint32_t> { id });
    EXPECT_TRUE(exists("take.wav"));
}

TEST_F(IOManagerLifecycleTest, ShutdownFinalisesAnOutstandingAudioCapture)
{
    FakeAudioBackend backend;
    const auto id = manager->capture_output(path_of("take.wav"));
    ASSERT_NE(id, 0U);

    manager->shutdown();

    EXPECT_TRUE(manager->get_audio_capture_ids().empty());
    EXPECT_EQ(backend.observer_count(), 0U);
    EXPECT_TRUE(exists("take.wav"));
}

TEST_F(IOManagerLifecycleTest, DestructionFinalisesAnOutstandingAudioCapture)
{
    FakeAudioBackend backend;
    ASSERT_NE(manager->capture_output(path_of("take.wav")), 0U);

    manager.reset();

    EXPECT_EQ(backend.observer_count(), 0U);
    EXPECT_TRUE(exists("take.wav"));
}

TEST_F(IOManagerLifecycleTest, StopWaitsForQueuedSaves)
{
    Kakshya::ImageData image;
    image.pixels = std::vector<uint8_t>(4U * 3U * 4U, 200);
    image.width = 4;
    image.height = 3;
    image.channels = 4;
    image.format = Kakshya::ImageFormat::RGBA8;
    ASSERT_TRUE(manager->save_image(std::move(image), path_of("queued.png")));

    manager->stop();

    EXPECT_TRUE(exists("queued.png"));
    EXPECT_GT(size_of("queued.png"), 0U);
}

// =========================================================================
// Reader registry
// =========================================================================

TEST_F(IOManagerRegistryTest, VideoReaderIdsAreNonZeroAndDistinct)
{
    std::set<uint64_t> ids;
    for (int i = 0; i < 16; ++i) {
        const auto id = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());
        EXPECT_NE(id, 0U);
        EXPECT_TRUE(ids.insert(id).second) << id;
    }

    EXPECT_EQ(manager->get_video_reader_ids().size(), 16U);
}

TEST_F(IOManagerRegistryTest, VideoAndCameraSourcesDrawFromOneCounter)
{
    const auto video = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());
    const auto camera = manager->register_camera_source(std::make_shared<CountingCameraSource>());
    const auto video_again = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());

    EXPECT_EQ(camera, video + 1);
    EXPECT_EQ(video_again, camera + 1);
}

TEST_F(IOManagerRegistryTest, TheFirstIdIsOne)
{
    EXPECT_EQ(manager->register_video_reader(std::make_shared<IO::VideoFileReader>()), 1U);
}

TEST_F(IOManagerRegistryTest, RegisteringAVideoReaderStampsItsIdOnTheReader)
{
    auto reader = std::make_shared<IO::VideoFileReader>();
    EXPECT_EQ(reader->get_reader_id(), 0U);

    const auto id = manager->register_video_reader(reader);

    EXPECT_EQ(reader->get_reader_id(), id);
    EXPECT_EQ(manager->get_video_reader(id), reader);
}

TEST_F(IOManagerRegistryTest, ANullVideoReaderIsRefusedWithIdZero)
{
    EXPECT_EQ(manager->register_video_reader(nullptr), 0U);
    EXPECT_TRUE(manager->get_video_reader_ids().empty());
}

TEST_F(IOManagerRegistryTest, ANullCameraSourceIsRefusedWithIdZero)
{
    EXPECT_EQ(manager->register_camera_source(nullptr), 0U);
    EXPECT_TRUE(manager->get_camera_reader_ids().empty());
}

TEST_F(IOManagerRegistryTest, ARefusedRegistrationDoesNotConsumeAnId)
{
    static_cast<void>(manager->register_video_reader(nullptr));
    static_cast<void>(manager->register_camera_source(nullptr));

    EXPECT_EQ(manager->register_video_reader(std::make_shared<IO::VideoFileReader>()), 1U);
}

TEST_F(IOManagerRegistryTest, ReleasingAVideoReaderRemovesOnlyThatReader)
{
    const auto first = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());
    const auto second = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());

    manager->release_video_reader(first);

    EXPECT_EQ(manager->get_video_reader(first), nullptr);
    EXPECT_NE(manager->get_video_reader(second), nullptr);
    EXPECT_EQ(manager->get_video_reader_ids(), std::vector<uint64_t> { second });
}

TEST_F(IOManagerRegistryTest, ReleasedIdsAreNeverReissued)
{
    const auto first = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());
    manager->release_video_reader(first);

    const auto second = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());

    EXPECT_NE(first, second);
    EXPECT_GT(second, first);
}

TEST_F(IOManagerRegistryTest, ReleasingTwiceIsANoOp)
{
    const auto id = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());

    manager->release_video_reader(id);
    EXPECT_NO_THROW(manager->release_video_reader(id));
    EXPECT_TRUE(manager->get_video_reader_ids().empty());
}

TEST_F(IOManagerRegistryTest, ReleasingUnknownIdsIsANoOp)
{
    const auto keep = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());

    EXPECT_NO_THROW(manager->release_video_reader(0));
    EXPECT_NO_THROW(manager->release_video_reader(keep + 1000));
    EXPECT_NO_THROW(manager->release_video_reader(std::numeric_limits<uint64_t>::max()));
    EXPECT_NO_THROW(manager->release_camera_source(0));
    EXPECT_NO_THROW(manager->release_camera_source(std::numeric_limits<uint64_t>::max()));

    EXPECT_NE(manager->get_video_reader(keep), nullptr);
}

TEST_F(IOManagerRegistryTest, AVideoIdIsNotACameraIdAndTheOtherWayRound)
{
    const auto video = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());
    const auto camera = manager->register_camera_source(std::make_shared<CountingCameraSource>());

    EXPECT_EQ(manager->get_camera_reader(video), nullptr);
    EXPECT_EQ(manager->get_video_reader(camera), nullptr);

    manager->release_camera_source(video);
    manager->release_video_reader(camera);

    EXPECT_NE(manager->get_video_reader(video), nullptr);
    EXPECT_NE(manager->get_camera_reader(camera), nullptr);
}

TEST_F(IOManagerRegistryTest, LookupsOfUnknownIdsReturnNull)
{
    EXPECT_EQ(manager->get_video_reader(0), nullptr);
    EXPECT_EQ(manager->get_video_reader(7), nullptr);
    EXPECT_EQ(manager->get_camera_reader(0), nullptr);
    EXPECT_EQ(manager->get_camera_reader(7), nullptr);
}

TEST_F(IOManagerRegistryTest, ReleasingACameraSourceDropsTheManagersReference)
{
    auto camera = std::make_shared<CountingCameraSource>();
    const std::weak_ptr<CountingCameraSource> weak = camera;
    const auto id = manager->register_camera_source(std::move(camera));

    manager->release_camera_source(id);

    EXPECT_TRUE(weak.expired());
    EXPECT_TRUE(manager->get_camera_reader_ids().empty());
}

TEST_F(IOManagerRegistryTest, ReleasingAVideoReaderDropsTheManagersReference)
{
    auto reader = std::make_shared<IO::VideoFileReader>();
    const std::weak_ptr<IO::VideoFileReader> weak = reader;
    const auto id = manager->register_video_reader(std::move(reader));

    manager->release_video_reader(id);

    EXPECT_TRUE(weak.expired());
}

TEST_F(IOManagerRegistryTest, RegisteringTheSameSourceTwiceGivesTwoIds)
{
    auto camera = std::make_shared<CountingCameraSource>();

    const auto first = manager->register_camera_source(camera);
    const auto second = manager->register_camera_source(camera);

    EXPECT_NE(first, second);
    EXPECT_EQ(manager->get_camera_reader_ids().size(), 2U);
    manager->release_camera_source(first);
    EXPECT_NE(manager->get_camera_reader(second), nullptr);
}

TEST_F(IOManagerRegistryTest, RegisteringTheSameVideoReaderTwiceRestampsItsId)
{
    auto reader = std::make_shared<IO::VideoFileReader>();

    const auto first = manager->register_video_reader(reader);
    const auto second = manager->register_video_reader(reader);

    EXPECT_NE(first, second);
    EXPECT_EQ(reader->get_reader_id(), second);
    EXPECT_EQ(manager->get_video_reader(first), reader);
    EXPECT_EQ(manager->get_video_reader(second), reader);
}

TEST_F(IOManagerRegistryTest, RegistryDoesNotLeakBetweenManagers)
{
    const auto id = manager->register_camera_source(std::make_shared<CountingCameraSource>());
    rebuild();

    EXPECT_EQ(manager->get_camera_reader(id), nullptr);
    EXPECT_TRUE(manager->get_camera_reader_ids().empty());
}

// =========================================================================
// IOService dispatch
// =========================================================================

TEST_F(IOManagerDispatchTest, FrameRequestReachesTheRegisteredCameraSource)
{
    auto camera = std::make_shared<CountingCameraSource>();
    const auto id = manager->register_camera_source(camera);

    io_service()->request_frame(id);

    EXPECT_EQ(camera->pulls.load(), 1);
}

TEST_F(IOManagerDispatchTest, EachFrameRequestPullsExactlyOnce)
{
    auto camera = std::make_shared<CountingCameraSource>();
    const auto id = manager->register_camera_source(camera);

    for (int i = 0; i < 25; ++i) {
        io_service()->request_frame(id);
    }

    EXPECT_EQ(camera->pulls.load(), 25);
}

TEST_F(IOManagerDispatchTest, FrameRequestsReachOnlyTheirOwnSource)
{
    auto first = std::make_shared<CountingCameraSource>();
    auto second = std::make_shared<CountingCameraSource>();
    const auto first_id = manager->register_camera_source(first);
    const auto second_id = manager->register_camera_source(second);

    io_service()->request_frame(second_id);
    io_service()->request_frame(second_id);
    io_service()->request_frame(first_id);

    EXPECT_EQ(first->pulls.load(), 1);
    EXPECT_EQ(second->pulls.load(), 2);
}

TEST_F(IOManagerDispatchTest, FrameRequestForAnUnknownIdIsDroppedSilently)
{
    auto camera = std::make_shared<CountingCameraSource>();
    static_cast<void>(manager->register_camera_source(camera));

    EXPECT_NO_THROW(io_service()->request_frame(0));
    EXPECT_NO_THROW(io_service()->request_frame(9999));

    EXPECT_EQ(camera->pulls.load(), 0);
}

TEST_F(IOManagerDispatchTest, FrameRequestForAVideoReaderIdDoesNotReachCameras)
{
    auto camera = std::make_shared<CountingCameraSource>();
    static_cast<void>(manager->register_camera_source(camera));
    const auto video = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());

    EXPECT_NO_THROW(io_service()->request_frame(video));

    EXPECT_EQ(camera->pulls.load(), 0);
}

TEST_F(IOManagerDispatchTest, FrameRequestAfterReleaseIsDropped)
{
    auto camera = std::make_shared<CountingCameraSource>();
    const auto id = manager->register_camera_source(camera);
    manager->release_camera_source(id);

    EXPECT_NO_THROW(io_service()->request_frame(id));

    EXPECT_EQ(camera->pulls.load(), 0);
}

TEST_F(IOManagerDispatchTest, DecodeRequestForAnUnknownIdIsDroppedSilently)
{
    EXPECT_NO_THROW(io_service()->request_decode(0));
    EXPECT_NO_THROW(io_service()->request_decode(9999));
}

TEST_F(IOManagerDispatchTest, DecodeRequestForACameraIdIsDroppedSilently)
{
    const auto camera = manager->register_camera_source(std::make_shared<CountingCameraSource>());

    EXPECT_NO_THROW(io_service()->request_decode(camera));
}

TEST_F(IOManagerDispatchTest, DecodeRequestForARegisteredReaderIsAccepted)
{
    const auto video = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());

    EXPECT_NO_THROW(io_service()->request_decode(video));
    EXPECT_NO_THROW(io_service()->request_decode(video));
}

TEST_F(IOManagerDispatchTest, DecodeRequestAfterReleaseIsDropped)
{
    const auto video = manager->register_video_reader(std::make_shared<IO::VideoFileReader>());
    manager->release_video_reader(video);

    EXPECT_NO_THROW(io_service()->request_decode(video));
}

// =========================================================================
// Null and unknown arguments: nothing throws, nothing is queued
// =========================================================================

TEST_F(IOManagerNullArgumentTest, HooksRefuseNullContainers)
{
    EXPECT_EQ(manager->hook_video_container_to_buffer(nullptr), nullptr);
    EXPECT_EQ(manager->hook_camera_to_buffer(nullptr), nullptr);
    EXPECT_TRUE(manager->hook_audio_container_to_buffers(nullptr).empty());
}

TEST_F(IOManagerNullArgumentTest, RetrievalOfAnUnhookedContainerIsEmpty)
{
    EXPECT_EQ(manager->get_video_buffer(nullptr), nullptr);
    EXPECT_EQ(manager->get_camera_buffer(nullptr), nullptr);
    EXPECT_TRUE(manager->get_audio_buffers(nullptr).empty());
    EXPECT_EQ(manager->get_extracted_audio(nullptr), nullptr);
}

TEST_F(IOManagerNullArgumentTest, WritingANullContainerIsRefusedWithoutAFile)
{
    EXPECT_NO_THROW(manager->write(nullptr, path_of("never.wav")));
    manager->wait_for_pending_saves();

    EXPECT_FALSE(exists("never.wav"));
    EXPECT_TRUE(manager->get_sound_writers().empty());
}

TEST_F(IOManagerNullArgumentTest, SavesOfNullSourcesReturnFalse)
{
    EXPECT_FALSE(manager->save_image(std::shared_ptr<Core::VKImage> {}, path_of("never.png")));
    EXPECT_FALSE(manager->save_image(std::shared_ptr<Buffers::TextureBuffer> {}, path_of("never.png")));
    EXPECT_FALSE(manager->save_image(std::shared_ptr<Buffers::TextBuffer> {}, path_of("never.png")));
    EXPECT_FALSE(manager->save_volume(std::shared_ptr<Buffers::VolumeGridBuffer> {}, path_of("never.vdb")));
    EXPECT_FALSE(manager->save_mesh(std::shared_ptr<Buffers::ComputeMeshBuffer> {}, path_of("never.obj")));
    EXPECT_FALSE(manager->save_mesh_snapshot(std::shared_ptr<Buffers::ComputeMeshBuffer> {}, path_of("never_{}.obj")));
    EXPECT_FALSE(manager->save_composite(nullptr, path_of("never.csv")));
    manager->wait_for_pending_saves();

    EXPECT_FALSE(exists("never.png"));
    EXPECT_FALSE(exists("never.vdb"));
    EXPECT_FALSE(exists("never.obj"));
    EXPECT_FALSE(exists("never.csv"));
}

TEST_F(IOManagerNullArgumentTest, CapturesOfNullSourcesReturnZero)
{
    EXPECT_EQ(manager->capture_window(nullptr, path_of("never.mp4"), 30.0), 0U);
    EXPECT_EQ(manager->capture_volume(nullptr, path_of("never.{:04}.vdb")), 0U);
    EXPECT_EQ(manager->capture_spatial(path_of("never.abc"), {}), 0U);

    EXPECT_TRUE(manager->get_video_capture_ids().empty());
    EXPECT_TRUE(manager->get_volume_capture_ids().empty());
    EXPECT_TRUE(manager->get_spatial_capture_ids().empty());
    EXPECT_FALSE(exists("never.abc"));
}

TEST_F(IOManagerNullArgumentTest, StoppingUnknownCapturesIsANoOp)
{
    EXPECT_NO_THROW(manager->stop_audio_capture(0));
    EXPECT_NO_THROW(manager->stop_audio_capture(12345));
    EXPECT_NO_THROW(manager->stop_video_capture(0));
    EXPECT_NO_THROW(manager->stop_video_capture(12345));
    EXPECT_NO_THROW(manager->stop_volume_capture(0));
    EXPECT_NO_THROW(manager->stop_volume_capture(12345));
    EXPECT_NO_THROW(manager->stop_spatial_capture(0));
    EXPECT_NO_THROW(manager->stop_spatial_capture(12345));
    EXPECT_NO_THROW(manager->stop_capture(nullptr));
}

TEST_F(IOManagerNullArgumentTest, WaitingWithNothingQueuedReturnsAtOnce)
{
    EXPECT_NO_THROW(manager->wait_for_pending_saves());
    EXPECT_NO_THROW(manager->wait_for_pending_saves());
}

TEST_F(IOManagerNullArgumentTest, AManagerBuiltWithoutASchedulerRefusesSchedulerBackedCaptures)
{
    manager.reset();
    manager = std::make_unique<IO::IOManager>(stream_info, k_fps, buffers, nullptr);

    std::vector<IO::SpatialCaptureSource> sources(1);
    sources[0].stream_name = "stream";
    sources[0].capture_frame = [](IO::SpatialCache&, const std::string&) { return true; };

    EXPECT_EQ(manager->capture_spatial(path_of("no_scheduler.abc"), std::move(sources)), 0U);
    EXPECT_TRUE(manager->get_spatial_capture_ids().empty());
}

// =========================================================================
// Concurrency
// =========================================================================

TEST_F(IOManagerConcurrencyTest, ParallelRegistrationYieldsDistinctIds)
{
    constexpr int threads = 8;
    constexpr int per_thread = 100;
    std::vector<std::vector<uint64_t>> ids(threads);

    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([this, t, &ids] {
                for (int i = 0; i < per_thread; ++i) {
                    ids[t].push_back(manager->register_camera_source(std::make_shared<CountingCameraSource>()));
                }
            });
        }
    }

    std::set<uint64_t> unique;
    for (const auto& group : ids) {
        unique.insert(group.begin(), group.end());
    }
    EXPECT_EQ(unique.size(), static_cast<size_t>(threads * per_thread));
    EXPECT_EQ(unique.count(0), 0U);
    EXPECT_EQ(manager->get_camera_reader_ids().size(), static_cast<size_t>(threads * per_thread));
}

TEST_F(IOManagerConcurrencyTest, RegisterDispatchReleaseFromManyThreadsLosesNoPull)
{
    constexpr int threads = 6;
    constexpr int rounds = 200;
    std::atomic<int> total_pulls { 0 };

    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([this, &total_pulls] {
                for (int i = 0; i < rounds; ++i) {
                    auto camera = std::make_shared<CountingCameraSource>();
                    const auto id = manager->register_camera_source(camera);
                    io_service()->request_frame(id);
                    total_pulls.fetch_add(camera->pulls.load(), std::memory_order_relaxed);
                    manager->release_camera_source(id);
                }
            });
        }
    }

    EXPECT_EQ(total_pulls.load(), threads * rounds);
    EXPECT_TRUE(manager->get_camera_reader_ids().empty());
}

TEST_F(IOManagerConcurrencyTest, DispatchRacingReleaseNeitherCrashesNorOverPulls)
{
    auto camera = std::make_shared<CountingCameraSource>();
    const auto id = manager->register_camera_source(camera);
    std::atomic<bool> stop { false };
    std::atomic<int> requested { 0 };

    {
        std::jthread dispatcher([&] {
            while (!stop.load(std::memory_order_acquire)) {
                io_service()->request_frame(id);
                requested.fetch_add(1, std::memory_order_relaxed);
            }
        });
        std::jthread releaser([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            manager->release_camera_source(id);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            stop.store(true, std::memory_order_release);
        });
    }

    EXPECT_LE(camera->pulls.load(), requested.load());
    EXPECT_GT(camera->pulls.load(), 0);
    EXPECT_EQ(manager->get_camera_reader(id), nullptr);
}

TEST_F(IOManagerConcurrencyTest, ParallelQueuedSavesAllLand)
{
    constexpr int saves = 12;
    for (int i = 0; i < saves; ++i) {
        Kakshya::ImageData image;
        image.pixels = std::vector<uint8_t>(4U * 3U * 4U, static_cast<uint8_t>(i * 10));
        image.width = 4;
        image.height = 3;
        image.channels = 4;
        image.format = Kakshya::ImageFormat::RGBA8;
        ASSERT_TRUE(manager->save_image(std::move(image), path_of("burst_" + std::to_string(i) + ".png")));
    }

    manager->wait_for_pending_saves();

    for (int i = 0; i < saves; ++i) {
        const auto name = "burst_" + std::to_string(i) + ".png";
        EXPECT_TRUE(exists(name)) << name;
        EXPECT_GT(size_of(name), 0U) << name;
    }
}

TEST_F(IOManagerConcurrencyTest, SavesQueuedFromManyThreadsAllLand)
{
    constexpr int threads = 4;
    constexpr int per_thread = 4;

    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([this, t] {
                for (int i = 0; i < per_thread; ++i) {
                    Kakshya::ImageData image;
                    image.pixels = std::vector<uint8_t>(4U * 3U * 4U, static_cast<uint8_t>(t * 16 + i));
                    image.width = 4;
                    image.height = 3;
                    image.channels = 4;
                    image.format = Kakshya::ImageFormat::RGBA8;
                    static_cast<void>(manager->save_image(
                        std::move(image), path_of("t" + std::to_string(t) + "_" + std::to_string(i) + ".png")));
                }
            });
        }
    }

    manager->wait_for_pending_saves();

    for (int t = 0; t < threads; ++t) {
        for (int i = 0; i < per_thread; ++i) {
            EXPECT_TRUE(exists("t" + std::to_string(t) + "_" + std::to_string(i) + ".png")) << t << ' ' << i;
        }
    }
}

}
