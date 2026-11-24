#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/audio_container.hpp"
#include "io_manager_fixture.hpp"

#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"
#include "MayaFlux/Kakshya/Source/SoundFileContainer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {

namespace {

    class IOManagerAudioLoadTest : public IOManagerFixture { };
    class IOManagerAudioBoundedTest : public IOManagerFixture { };
    class IOManagerAudioHookTest : public IOManagerFixture { };
    class IOManagerAudioWriterTest : public IOManagerFixture { };
    class IOManagerAudioCaptureTest : public IOManagerFixture { };

    std::vector<double> ramp(size_t count)
    {
        std::vector<double> samples(count);
        for (size_t i = 0; i < count; ++i) {
            samples[i] = (static_cast<double>(i) / static_cast<double>(count)) - 0.5;
        }
        return samples;
    }

}

// =========================================================================
// load_audio
// =========================================================================

TEST_F(IOManagerAudioLoadTest, MissingFileGivesNullAndRetainsNothing)
{
    EXPECT_EQ(manager->load_audio(path_of("missing.wav")), nullptr);
    EXPECT_TRUE(manager->get_audio_readers().empty());
}

TEST_F(IOManagerAudioLoadTest, EmptyPathGivesNull)
{
    EXPECT_EQ(manager->load_audio(""), nullptr);
    EXPECT_TRUE(manager->get_audio_readers().empty());
}

TEST_F(IOManagerAudioLoadTest, DirectoryPathGivesNull)
{
    EXPECT_EQ(manager->load_audio(root().string()), nullptr);
    EXPECT_TRUE(manager->get_audio_readers().empty());
}

TEST_F(IOManagerAudioLoadTest, NonAudioBytesUnderAnAudioExtensionGiveNull)
{
    FFmpegFixture::write_bytes(root() / "noise.wav", FFmpegFixture::noise_bytes(4096));

    EXPECT_EQ(manager->load_audio(path_of("noise.wav")), nullptr);
    EXPECT_TRUE(manager->get_audio_readers().empty());
}

TEST_F(IOManagerAudioLoadTest, TruncatedWavGivesNullOrAContainerNeverACrash)
{
    const auto bytes = FFmpegFixture::make_wav(2, k_rate, 480);
    FFmpegFixture::write_bytes(root() / "cut.wav", bytes.substr(0, 30));

    EXPECT_NO_THROW({
        const auto container = manager->load_audio(path_of("cut.wav"));
        static_cast<void>(container);
    });
}

TEST_F(IOManagerAudioLoadTest, EmptyFileGivesNull)
{
    FFmpegFixture::write_bytes(root() / "empty.wav", "");

    EXPECT_EQ(manager->load_audio(path_of("empty.wav")), nullptr);
    EXPECT_TRUE(manager->get_audio_readers().empty());
}

TEST_F(IOManagerAudioLoadTest, AFileShorterThanOneBlockGivesNullInsteadOfThrowing)
{
    write_wav("click.wav", 1, k_block - 1);

    std::shared_ptr<Kakshya::SoundFileContainer> container;
    EXPECT_NO_THROW(container = manager->load_audio(path_of("click.wav")));

    EXPECT_EQ(container, nullptr);
    EXPECT_TRUE(manager->get_audio_readers().empty());
}

TEST_F(IOManagerAudioLoadTest, AFileExactlyOneBlockLongLoads)
{
    write_wav("block.wav", 1, k_block);

    const auto container = manager->load_audio(path_of("block.wav"));

    ASSERT_NE(container, nullptr);
    EXPECT_EQ(manager->get_audio_readers().size(), 1U);
}

TEST_F(IOManagerAudioLoadTest, AShortFileDoesNotPoisonTheNextLoad)
{
    write_wav("click.wav", 1, 100);
    write_wav("long.wav", 2);

    EXPECT_NO_THROW(static_cast<void>(manager->load_audio(path_of("click.wav"))));

    EXPECT_NE(manager->load_audio(path_of("long.wav")), nullptr);
}

TEST_F(IOManagerAudioLoadTest, ValidFileLoadsWithItsChannelCount)
{
    write_wav("stereo.wav", 2);

    const auto container = manager->load_audio(path_of("stereo.wav"));

    ASSERT_NE(container, nullptr);
    EXPECT_EQ(container->get_num_channels(), 2U);
}

TEST_F(IOManagerAudioLoadTest, MonoAndMultichannelFilesKeepTheirChannelCounts)
{
    write_wav("mono.wav", 1);
    write_wav("quad.wav", 4);

    const auto mono = manager->load_audio(path_of("mono.wav"));
    const auto quad = manager->load_audio(path_of("quad.wav"));

    ASSERT_NE(mono, nullptr);
    ASSERT_NE(quad, nullptr);
    EXPECT_EQ(mono->get_num_channels(), 1U);
    EXPECT_EQ(quad->get_num_channels(), 4U);
}

TEST_F(IOManagerAudioLoadTest, LoadedContainerHasItsDefaultProcessorConfigured)
{
    write_wav("stereo.wav", 2);

    const auto container = manager->load_audio(path_of("stereo.wav"));

    ASSERT_NE(container, nullptr);
    EXPECT_NE(container->get_default_processor(), nullptr);
}

TEST_F(IOManagerAudioLoadTest, EachSuccessfulLoadRetainsOneReader)
{
    write_wav("a.wav");
    write_wav("b.wav");

    ASSERT_NE(manager->load_audio(path_of("a.wav")), nullptr);
    EXPECT_EQ(manager->get_audio_readers().size(), 1U);
    ASSERT_NE(manager->load_audio(path_of("b.wav")), nullptr);
    EXPECT_EQ(manager->get_audio_readers().size(), 2U);
}

TEST_F(IOManagerAudioLoadTest, AFailedLoadBetweenSuccessesRetainsNothing)
{
    write_wav("a.wav");
    ASSERT_NE(manager->load_audio(path_of("a.wav")), nullptr);

    EXPECT_EQ(manager->load_audio(path_of("missing.wav")), nullptr);

    EXPECT_EQ(manager->get_audio_readers().size(), 1U);
}

TEST_F(IOManagerAudioLoadTest, LoadingTheSameFileTwiceGivesIndependentContainers)
{
    write_wav("a.wav");

    const auto first = manager->load_audio(path_of("a.wav"));
    const auto second = manager->load_audio(path_of("a.wav"));

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);
    EXPECT_EQ(manager->get_audio_readers().size(), 2U);
}

TEST_F(IOManagerAudioLoadTest, ShutdownReleasesRetainedAudioReaders)
{
    write_wav("a.wav");
    ASSERT_NE(manager->load_audio(path_of("a.wav")), nullptr);

    manager->shutdown();

    EXPECT_TRUE(manager->get_audio_readers().empty());
}

TEST_F(IOManagerAudioLoadTest, AContainerOutlivesTheManagerThatLoadedIt)
{
    write_wav("a.wav");
    const auto container = manager->load_audio(path_of("a.wav"));
    ASSERT_NE(container, nullptr);

    manager.reset();

    EXPECT_EQ(container->get_num_channels(), 2U);
}

// =========================================================================
// load_audio_bounded
// =========================================================================

TEST_F(IOManagerAudioBoundedTest, MissingFileGivesNull)
{
    EXPECT_EQ(manager->load_audio_bounded(path_of("missing.wav")), nullptr);
    EXPECT_TRUE(manager->get_audio_readers().empty());
}

TEST_F(IOManagerAudioBoundedTest, NonAudioBytesGiveNull)
{
    FFmpegFixture::write_bytes(root() / "noise.wav", FFmpegFixture::noise_bytes(4096));

    EXPECT_EQ(manager->load_audio_bounded(path_of("noise.wav")), nullptr);
    EXPECT_TRUE(manager->get_audio_readers().empty());
}

TEST_F(IOManagerAudioBoundedTest, ShortFileLoadsUnderTheDefaultBound)
{
    write_wav("short.wav", 2, 480);

    const auto stream = manager->load_audio_bounded(path_of("short.wav"));

    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->get_num_channels(), 2U);
    EXPECT_EQ(manager->get_audio_readers().size(), 1U);
}

TEST_F(IOManagerAudioBoundedTest, FileOverTheBoundIsRefusedWithoutTruncation)
{
    write_wav("long.wav", 2, 480);

    EXPECT_EQ(manager->load_audio_bounded(path_of("long.wav"), 10, false), nullptr);
    EXPECT_TRUE(manager->get_audio_readers().empty());
}

TEST_F(IOManagerAudioBoundedTest, FileOverTheBoundLoadsWhenTruncationIsAllowed)
{
    write_wav("long.wav", 2, 480);

    const auto stream = manager->load_audio_bounded(path_of("long.wav"), 10, true);

    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->get_num_channels(), 2U);
    EXPECT_EQ(manager->get_audio_readers().size(), 1U);
}

TEST_F(IOManagerAudioBoundedTest, AFileExactlyAtTheBoundLoadsWithoutTruncation)
{
    write_wav("exact.wav", 1, 480);

    EXPECT_NE(manager->load_audio_bounded(path_of("exact.wav"), 480, false), nullptr);
}

// =========================================================================
// Hooking audio containers to buffers
// =========================================================================

TEST_F(IOManagerAudioHookTest, HookCreatesOneBufferPerChannel)
{
    write_wav("quad.wav", 4);
    const auto container = manager->load_audio(path_of("quad.wav"));
    ASSERT_NE(container, nullptr);

    const auto created = manager->hook_audio_container_to_buffers(container);

    ASSERT_EQ(created.size(), 4U);
    for (const auto& buffer : created) {
        EXPECT_NE(buffer, nullptr);
    }
}

TEST_F(IOManagerAudioHookTest, RetrievalReturnsTheHookedBuffers)
{
    write_wav("stereo.wav", 2);
    const auto container = manager->load_audio(path_of("stereo.wav"));
    ASSERT_NE(container, nullptr);

    const auto created = manager->hook_audio_container_to_buffers(container);
    const auto fetched = manager->get_audio_buffers(container);

    EXPECT_EQ(fetched, created);
}

TEST_F(IOManagerAudioHookTest, AnUnhookedContainerHasNoBuffers)
{
    write_wav("stereo.wav", 2);
    const auto container = manager->load_audio(path_of("stereo.wav"));
    ASSERT_NE(container, nullptr);

    EXPECT_TRUE(manager->get_audio_buffers(container).empty());
}

TEST_F(IOManagerAudioHookTest, BuffersAreKeyedByContainerNotShared)
{
    write_wav("a.wav", 2);
    write_wav("b.wav", 1);
    const auto first = manager->load_audio(path_of("a.wav"));
    const auto second = manager->load_audio(path_of("b.wav"));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    static_cast<void>(manager->hook_audio_container_to_buffers(first));
    static_cast<void>(manager->hook_audio_container_to_buffers(second));

    EXPECT_EQ(manager->get_audio_buffers(first).size(), 2U);
    EXPECT_EQ(manager->get_audio_buffers(second).size(), 1U);
}

TEST_F(IOManagerAudioHookTest, HookingTwiceReplacesTheStoredBuffers)
{
    write_wav("stereo.wav", 2);
    const auto container = manager->load_audio(path_of("stereo.wav"));
    ASSERT_NE(container, nullptr);

    const auto first = manager->hook_audio_container_to_buffers(container);
    const auto second = manager->hook_audio_container_to_buffers(container);

    ASSERT_EQ(second.size(), 2U);
    EXPECT_EQ(manager->get_audio_buffers(container), second);
    EXPECT_NE(first[0], second[0]);
}

TEST_F(IOManagerAudioHookTest, ShutdownReleasesHookedBuffers)
{
    write_wav("stereo.wav", 2);
    const auto container = manager->load_audio(path_of("stereo.wav"));
    ASSERT_NE(container, nullptr);
    static_cast<void>(manager->hook_audio_container_to_buffers(container));

    manager->shutdown();

    EXPECT_TRUE(manager->get_audio_buffers(container).empty());
}

// =========================================================================
// Sound writers
// =========================================================================

TEST_F(IOManagerAudioWriterTest, ValidPathGivesAnOpenWriterTheManagerTracks)
{
    const auto writer = manager->create_writer(path_of("out.wav"), 2, k_rate);

    ASSERT_NE(writer, nullptr);
    EXPECT_TRUE(writer->is_open());
    ASSERT_EQ(manager->get_sound_writers().size(), 1U);
    EXPECT_EQ(manager->get_sound_writers()[0], writer);
}

TEST_F(IOManagerAudioWriterTest, UnwritableDirectoryGivesNullAndTracksNothing)
{
    const auto writer = manager->create_writer(path_of("no_such_dir/out.wav"), 2, k_rate);

    EXPECT_EQ(writer, nullptr);
    EXPECT_TRUE(manager->get_sound_writers().empty());
}

TEST_F(IOManagerAudioWriterTest, UnknownExtensionGivesNullAndTracksNothing)
{
    const auto writer = manager->create_writer(path_of("out.notaformat"), 2, k_rate);

    EXPECT_EQ(writer, nullptr);
    EXPECT_TRUE(manager->get_sound_writers().empty());
}

TEST_F(IOManagerAudioWriterTest, SeveralWritersAreAllTrackedInOrder)
{
    const auto a = manager->create_writer(path_of("a.wav"), 1, k_rate);
    const auto b = manager->create_writer(path_of("b.wav"), 2, k_rate);
    const auto c = manager->create_writer(path_of("c.wav"), 2, 44100);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    EXPECT_EQ(manager->get_sound_writers(), (std::vector<std::shared_ptr<IO::SoundFileWriter>> { a, b, c }));
}

TEST_F(IOManagerAudioWriterTest, ACallerClosedWriterStaysListedAndStopDoesNotCloseItAgain)
{
    const auto writer = manager->create_writer(path_of("out.wav"), 2, k_rate);
    ASSERT_NE(writer, nullptr);
    writer->close().wait();
    ASSERT_FALSE(writer->is_open());

    EXPECT_NO_THROW(manager->stop());
    EXPECT_EQ(manager->get_sound_writers().size(), 1U);
}

TEST_F(IOManagerAudioWriterTest, WriterDrivenByTheCallerProducesAFile)
{
    const auto writer = manager->create_writer(path_of("out.wav"), 1, k_rate);
    ASSERT_NE(writer, nullptr);
    writer->write(std::vector<Kakshya::DataVariant> { Kakshya::DataVariant { ramp(4800) } });
    ASSERT_TRUE(writer->close().get());

    EXPECT_GT(size_of("out.wav"), 44U);
}

TEST_F(IOManagerAudioWriterTest, WriteOfAContainerProducesAReadableFile)
{
    const auto container = make_audio_container(ramp(4800), k_rate);

    manager->write(container, path_of("saved.wav"));
    manager->wait_for_pending_saves();

    ASSERT_TRUE(exists("saved.wav"));
    EXPECT_GT(size_of("saved.wav"), 44U);
    const auto back = manager->load_audio(path_of("saved.wav"));
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->get_num_channels(), 1U);
}

TEST_F(IOManagerAudioWriterTest, WriteOfAStereoContainerKeepsBothChannels)
{
    const auto container = make_audio_container(
        std::vector<std::vector<double>> { ramp(4800), ramp(4800) }, k_rate);

    manager->write(container, path_of("saved.wav"));
    manager->wait_for_pending_saves();

    const auto back = manager->load_audio(path_of("saved.wav"));
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->get_num_channels(), 2U);
}

TEST_F(IOManagerAudioWriterTest, WriteToAnUnknownExtensionLeavesNoFileAndNoWriter)
{
    const auto container = make_audio_container(ramp(4800), k_rate);

    EXPECT_NO_THROW(manager->write(container, path_of("saved.notaformat")));
    manager->wait_for_pending_saves();

    EXPECT_FALSE(exists("saved.notaformat"));
    EXPECT_TRUE(manager->get_sound_writers().empty());
}

TEST_F(IOManagerAudioWriterTest, WriteToAMissingDirectoryLeavesNothing)
{
    const auto container = make_audio_container(ramp(4800), k_rate);

    EXPECT_NO_THROW(manager->write(container, path_of("no_such_dir/saved.wav")));
    manager->wait_for_pending_saves();

    EXPECT_FALSE(std::filesystem::exists(root() / "no_such_dir"));
}

TEST_F(IOManagerAudioWriterTest, WriteDoesNotRegisterATrackedWriter)
{
    const auto container = make_audio_container(ramp(4800), k_rate);

    manager->write(container, path_of("saved.wav"));
    manager->wait_for_pending_saves();

    EXPECT_TRUE(manager->get_sound_writers().empty());
}

TEST_F(IOManagerAudioWriterTest, ManyWritesInFlightAllFinishOnWait)
{
    const auto container = make_audio_container(ramp(4800), k_rate);
    constexpr int count = 8;

    for (int i = 0; i < count; ++i) {
        manager->write(container, path_of("many_" + std::to_string(i) + ".wav"));
    }
    manager->wait_for_pending_saves();

    for (int i = 0; i < count; ++i) {
        EXPECT_GT(size_of("many_" + std::to_string(i) + ".wav"), 44U) << i;
    }
}

TEST_F(IOManagerAudioWriterTest, WritingTheSameContainerToTheSamePathTwiceSettles)
{
    const auto container = make_audio_container(ramp(4800), k_rate);

    manager->write(container, path_of("same.wav"));
    manager->wait_for_pending_saves();
    manager->write(container, path_of("same.wav"));
    manager->wait_for_pending_saves();

    EXPECT_GT(size_of("same.wav"), 44U);
}

// =========================================================================
// Audio capture against a stand-in AudioBackendService
// =========================================================================

TEST_F(IOManagerAudioCaptureTest, WithoutAnAudioBackendServiceCaptureGivesZeroAndTouchesNothing)
{
    uint32_t id = 1;

    EXPECT_NO_THROW(id = manager->capture_output(path_of("take.wav")));

    EXPECT_EQ(id, 0U);
    EXPECT_TRUE(manager->get_audio_capture_ids().empty());
    EXPECT_TRUE(manager->get_sound_writers().empty());
    EXPECT_FALSE(exists("take.wav"));
}

TEST_F(IOManagerAudioCaptureTest, CaptureWorksOnceTheServiceAppearsAfterAFailedAttempt)
{
    ASSERT_EQ(manager->capture_output(path_of("first.wav")), 0U);

    FakeAudioBackend backend;
    const auto id = manager->capture_output(path_of("second.wav"));

    EXPECT_NE(id, 0U);
    EXPECT_EQ(backend.observer_count(), 1U);
}

TEST_F(IOManagerAudioCaptureTest, CaptureRegistersOneObserverAndATrackedWriter)
{
    FakeAudioBackend backend;

    const auto id = manager->capture_output(path_of("take.wav"));

    ASSERT_NE(id, 0U);
    EXPECT_EQ(backend.observer_count(), 1U);
    EXPECT_EQ(manager->get_audio_capture_ids(), std::vector<uint32_t> { id });
    ASSERT_EQ(manager->get_sound_writers().size(), 1U);
    EXPECT_TRUE(manager->get_sound_writers()[0]->is_open());
}

TEST_F(IOManagerAudioCaptureTest, TheCaptureIdIsTheObserverId)
{
    FakeAudioBackend backend;

    const auto id = manager->capture_output(path_of("take.wav"));

    manager->stop_audio_capture(id);
    EXPECT_EQ(backend.unregistered_ids(), std::vector<uint32_t> { id });
}

TEST_F(IOManagerAudioCaptureTest, TwoCapturesGetDistinctIdsAndObservers)
{
    FakeAudioBackend backend;

    const auto first = manager->capture_output(path_of("a.wav"));
    const auto second = manager->capture_output(path_of("b.wav"));

    ASSERT_NE(first, 0U);
    ASSERT_NE(second, 0U);
    EXPECT_NE(first, second);
    EXPECT_EQ(backend.observer_count(), 2U);
    auto ids = manager->get_audio_capture_ids();
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<uint32_t> { std::min(first, second), std::max(first, second) }));
}

TEST_F(IOManagerAudioCaptureTest, StoppingOneCaptureLeavesTheOtherRunning)
{
    FakeAudioBackend backend;
    const auto first = manager->capture_output(path_of("a.wav"));
    const auto second = manager->capture_output(path_of("b.wav"));

    manager->stop_audio_capture(first);

    EXPECT_EQ(backend.observer_count(), 1U);
    EXPECT_EQ(manager->get_audio_capture_ids(), std::vector<uint32_t> { second });
}

TEST_F(IOManagerAudioCaptureTest, StopFinalisesTheFileAndClosesTheWriter)
{
    FakeAudioBackend backend;
    const auto id = manager->capture_output(path_of("take.wav"));
    ASSERT_NE(id, 0U);
    const auto writer = manager->get_sound_writers().at(0);

    manager->stop_audio_capture(id);
    manager->wait_for_pending_saves();

    EXPECT_FALSE(writer->is_open());
    EXPECT_TRUE(exists("take.wav"));
    EXPECT_TRUE(manager->get_audio_capture_ids().empty());
    EXPECT_EQ(backend.observer_count(), 0U);
}

TEST_F(IOManagerAudioCaptureTest, StoppingTwiceUnregistersOnce)
{
    FakeAudioBackend backend;
    const auto id = manager->capture_output(path_of("take.wav"));

    manager->stop_audio_capture(id);
    EXPECT_NO_THROW(manager->stop_audio_capture(id));

    EXPECT_EQ(backend.unregistered_ids().size(), 1U);
}

TEST_F(IOManagerAudioCaptureTest, StoppingAnIdThatWasNeverIssuedLeavesCapturesAlone)
{
    FakeAudioBackend backend;
    const auto id = manager->capture_output(path_of("take.wav"));

    manager->stop_audio_capture(id + 1000);

    EXPECT_EQ(manager->get_audio_capture_ids(), std::vector<uint32_t> { id });
    EXPECT_EQ(backend.observer_count(), 1U);
    EXPECT_TRUE(backend.unregistered_ids().empty());
}

TEST_F(IOManagerAudioCaptureTest, AnUnwritablePathGivesZeroAndRegistersNoObserver)
{
    FakeAudioBackend backend;

    const auto id = manager->capture_output(path_of("no_such_dir/take.wav"));

    EXPECT_EQ(id, 0U);
    EXPECT_EQ(backend.observer_count(), 0U);
    EXPECT_TRUE(manager->get_audio_capture_ids().empty());
    EXPECT_TRUE(manager->get_sound_writers().empty());
}

TEST_F(IOManagerAudioCaptureTest, AnUnknownExtensionGivesZeroAndRegistersNoObserver)
{
    FakeAudioBackend backend;

    const auto id = manager->capture_output(path_of("take.notaformat"));

    EXPECT_EQ(id, 0U);
    EXPECT_EQ(backend.observer_count(), 0U);
}

TEST_F(IOManagerAudioCaptureTest, StopAfterTheBackendServiceVanishedStillFinalisesTheCapture)
{
    std::unique_ptr<FakeAudioBackend> backend = std::make_unique<FakeAudioBackend>();
    const auto id = manager->capture_output(path_of("take.wav"));
    ASSERT_NE(id, 0U);
    backend.reset();

    EXPECT_NO_THROW(manager->stop_audio_capture(id));
    manager->wait_for_pending_saves();

    EXPECT_TRUE(manager->get_audio_capture_ids().empty());
    EXPECT_TRUE(exists("take.wav"));
}

TEST_F(IOManagerAudioCaptureTest, ShutdownAfterTheBackendServiceVanishedIsClean)
{
    std::unique_ptr<FakeAudioBackend> backend = std::make_unique<FakeAudioBackend>();
    ASSERT_NE(manager->capture_output(path_of("take.wav")), 0U);
    backend.reset();

    EXPECT_NO_THROW(manager->shutdown());
    EXPECT_TRUE(manager->get_audio_capture_ids().empty());
}

TEST_F(IOManagerAudioCaptureTest, ACaptureStartedBeforeStopIsNotRestartedByIt)
{
    FakeAudioBackend backend;
    ASSERT_NE(manager->capture_output(path_of("take.wav")), 0U);

    manager->stop();
    manager->stop();

    EXPECT_EQ(backend.unregistered_ids().size(), 1U);
}

}
