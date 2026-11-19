#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/scoped_env.hpp"
#include "../support/scoped_working_dir.hpp"
#include "MayaFlux/Transitive/Platform/UnixSocket.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <random>

#if !defined(MAYAFLUX_PLATFORM_WINDOWS)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;
    namespace P = MayaFlux::Platform;
    using Reason = P::AcquireFailure::Reason;

    class SocketTree {
    public:
        SocketTree()
        {
            static std::atomic<unsigned> counter { 0 };
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_root = fs::temp_directory_path()
                / ("mf-ep-" + std::to_string(stamp % 1000000007) + "-" + std::to_string(counter++));
            fs::create_directories(m_root);
        }

        ~SocketTree()
        {
            std::error_code error;
            fs::permissions(m_root, fs::perms::owner_all, fs::perm_options::replace, error);
            fs::remove_all(m_root, error);
        }

        SocketTree(const SocketTree&) = delete;
        SocketTree& operator=(const SocketTree&) = delete;
        SocketTree(SocketTree&&) = delete;
        SocketTree& operator=(SocketTree&&) = delete;

        [[nodiscard]] const fs::path& root() const { return m_root; }

        fs::path make_file(const fs::path& relative, const std::string& content = "x")
        {
            const auto path = m_root / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << content;
            return path;
        }

        fs::path make_directory(const fs::path& relative)
        {
            const auto path = m_root / relative;
            fs::create_directories(path);
            return path;
        }

    private:
        fs::path m_root;
    };

    bool make_socket_file(const fs::path& path)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        (void)path;
        return false;
#else
        const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (descriptor < 0) {
            return false;
        }
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        const std::string text = path.string();
        if (text.size() >= sizeof(address.sun_path)) {
            ::close(descriptor);
            return false;
        }
        std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
        const int bound = ::bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        ::close(descriptor);
        return bound == 0;
#endif
    }

    const char* runtime_variable()
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        return "TEMP";
#elif defined(MAYAFLUX_PLATFORM_MACOS)
        return "TMPDIR";
#else
        return "XDG_RUNTIME_DIR";
#endif
    }

    P::UnixSocketSource at(const fs::path& path)
    {
        return P::UnixSocketFromPath { .path = path };
    }

    P::UnixSocketSource named(std::string file_name)
    {
        return P::UnixSocketFromName { .file_name = std::move(file_name) };
    }

    bool never_alive(const fs::path&)
    {
        return false;
    }

    bool always_alive(const fs::path&)
    {
        return true;
    }

    template <typename Result>
    Reason reason_of(const Result& result)
    {
        return result.error().reason;
    }

    // ---------------------------------------------------------------------
    // Resolution
    // ---------------------------------------------------------------------

    TEST(UnixSocketTest, AnEmptyPathIsAnEmptyRequest)
    {
        const auto result = P::acquire_unix_socket(at({}), never_alive);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(reason_of(result), Reason::EmptyRequest);
    }

    TEST(UnixSocketTest, AFreePathIsAcceptedAndTheProbeIsNeverCalled)
    {
        SocketTree tree;
        int calls = 0;
        const auto path = tree.root() / "lila.sock";

        const auto result = P::acquire_unix_socket(at(path), [&](const fs::path&) {
            ++calls;
            return false;
        });

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, path);
        EXPECT_FALSE(result->replaced_stale);
        EXPECT_EQ(calls, 0);
        EXPECT_FALSE(fs::exists(path));
    }

    TEST(UnixSocketTest, ARelativePathResolvesAgainstTheWorkingDirectory)
    {
        MayaFlux::Test::ScopedWorkingDir cwd;
        cwd.write(fs::path("local") / "keep", "x");

        const auto result = P::acquire_unix_socket(at("local/lila.sock"), never_alive);

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->path.is_absolute());
        EXPECT_EQ(result->path, fs::current_path() / "local" / "lila.sock");
    }

    TEST(UnixSocketTest, ANameResolvesInsideTheRuntimeDirectory)
    {
        SocketTree tree;
        const std::string directory = tree.root().string();
        MayaFlux::Test::ScopedEnv variable(runtime_variable(), directory.c_str());

        const auto result = P::acquire_unix_socket(named("t.sock"), never_alive);

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path, tree.root() / "t.sock");
        EXPECT_FALSE(result->replaced_stale);
    }

    TEST(UnixSocketTest, TheDefaultNameIsAPlainFileNameThatResolves)
    {
        SocketTree tree;
        const std::string directory = tree.root().string();
        MayaFlux::Test::ScopedEnv variable(runtime_variable(), directory.c_str());

        const auto result = P::acquire_unix_socket(named(std::string(P::default_unix_socket_name)), never_alive);

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->path.filename().string(), std::string(P::default_unix_socket_name));
    }

    TEST(UnixSocketTest, ANameRefusesWhatIsNotAPlainFileName)
    {
        SocketTree tree;
        const std::string directory = tree.root().string();
        MayaFlux::Test::ScopedEnv variable(runtime_variable(), directory.c_str());

        const std::vector<std::string> hostile { "../escape.sock", "sub/lila.sock", "/abs/lila.sock", ".", "..",
            std::string("lila\0.sock", 10), "lila\n.sock", "lila\t.sock" };
        for (const auto& name : hostile) {
            const auto result = P::acquire_unix_socket(named(name), never_alive);
            ASSERT_FALSE(result.has_value()) << name;
            EXPECT_EQ(reason_of(result), Reason::InvalidName) << name;
        }
        EXPECT_EQ(P::acquire_unix_socket(named(""), never_alive).error().reason, Reason::EmptyRequest);
    }

    TEST(UnixSocketTest, ANameWithNoUsableRuntimeDirectoryListsEveryCandidate)
    {
        SocketTree tree;
        const std::string absent = (tree.root() / "absent").string();
        MayaFlux::Test::ScopedEnv variable(runtime_variable(), absent.c_str());

        const auto result = P::acquire_unix_socket(named("t.sock"), never_alive);

        if (result.has_value()) {
            GTEST_SKIP() << "a fallback runtime directory exists on this host";
        }
        EXPECT_EQ(reason_of(result), Reason::Missing);
        ASSERT_FALSE(result.error().tried.empty());
        EXPECT_EQ(result.error().tried.front(), fs::path(absent));
    }

    TEST(UnixSocketTest, RuntimeCandidatesStartWithTheEnvironmentAndFollowItOnEveryCall)
    {
        SocketTree tree;
        const std::string first = tree.make_directory("a").string();
        const std::string second = tree.make_directory("b").string();

        {
            MayaFlux::Test::ScopedEnv variable(runtime_variable(), first.c_str());
            const auto candidates = P::runtime_directory_candidates();
            ASSERT_FALSE(candidates.empty());
            EXPECT_EQ(candidates.front(), fs::path(first));
        }
        {
            MayaFlux::Test::ScopedEnv variable(runtime_variable(), second.c_str());
            const auto candidates = P::runtime_directory_candidates();
            ASSERT_FALSE(candidates.empty());
            EXPECT_EQ(candidates.front(), fs::path(second));
        }
    }

    TEST(UnixSocketTest, EveryRuntimeCandidateIsAbsoluteAndARelativeVariableIsIgnored)
    {
        MayaFlux::Test::ScopedEnv variable(runtime_variable(), "relative/dir");

        for (const auto& candidate : P::runtime_directory_candidates()) {
            EXPECT_TRUE(candidate.is_absolute()) << candidate;
            EXPECT_NE(candidate, fs::path("relative/dir"));
        }
    }

    TEST(UnixSocketTest, AnEmptyRuntimeVariableIsSkipped)
    {
        MayaFlux::Test::ScopedEnv variable(runtime_variable(), "");

        for (const auto& candidate : P::runtime_directory_candidates()) {
            EXPECT_FALSE(candidate.empty());
        }
    }

    // ---------------------------------------------------------------------
    // Path limits
    // ---------------------------------------------------------------------

    TEST(UnixSocketTest, ThePathLimitIsExactInBytes)
    {
        SocketTree tree;
        const size_t limit = P::unix_socket_path_limit;
        const size_t base = tree.root().u8string().size() + 1;
        ASSERT_LT(base, limit);

        const auto at_limit = tree.root() / std::string(limit - base, 'a');
        const auto over_limit = tree.root() / std::string(limit - base + 1, 'a');

        EXPECT_TRUE(P::acquire_unix_socket(at(at_limit), never_alive).has_value());
        const auto refused = P::acquire_unix_socket(at(over_limit), never_alive);
        ASSERT_FALSE(refused.has_value());
        EXPECT_EQ(reason_of(refused), Reason::TooLong);
        ASSERT_EQ(refused.error().tried.size(), 1U);
        EXPECT_EQ(refused.error().tried.front(), over_limit);
    }

    TEST(UnixSocketTest, TheLimitCountsBytesNotCharacters)
    {
        SocketTree tree;
        const size_t limit = P::unix_socket_path_limit;
        const size_t remaining = limit - (tree.root().u8string().size() + 1);
        std::u8string name;
        while (name.size() + 2 <= remaining) {
            name += u8"\u00e9";
        }
        const std::u8string over = name + u8"\u00e9";

        EXPECT_TRUE(P::acquire_unix_socket(at(tree.root() / fs::path(name)), never_alive).has_value());
        const auto refused = P::acquire_unix_socket(at(tree.root() / fs::path(over)), never_alive);
        ASSERT_FALSE(refused.has_value());
        EXPECT_EQ(reason_of(refused), Reason::TooLong);
    }

    TEST(UnixSocketTest, AnOverlongComponentIsRefusedWithoutThrowing)
    {
        SocketTree tree;

        P::Acquired<P::UnixSocket> result = std::unexpected(P::AcquireFailure {});
        EXPECT_NO_THROW(result = P::acquire_unix_socket(at(tree.root() / std::string(400, 'a')), never_alive));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(reason_of(result), Reason::TooLong);
    }

    // ---------------------------------------------------------------------
    // The directory
    // ---------------------------------------------------------------------

    TEST(UnixSocketTest, AMissingDirectoryIsMissingAndIsListed)
    {
        SocketTree tree;
        const auto directory = tree.root() / "absent";

        const auto result = P::acquire_unix_socket(at(directory / "lila.sock"), never_alive);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(reason_of(result), Reason::Missing);
        ASSERT_EQ(result.error().tried.size(), 1U);
        EXPECT_EQ(result.error().tried.front(), directory);
    }

    TEST(UnixSocketTest, ADirectoryThatIsAFileIsNotADirectory)
    {
        SocketTree tree;
        const auto file = tree.make_file("not-a-directory");

        const auto result = P::acquire_unix_socket(at(file / "lila.sock"), never_alive);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(reason_of(result), Reason::NotDirectory);
    }

    TEST(UnixSocketTest, ADirectoryOthersCanWriteIsUnsafeUnlessItIsSticky)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "directory permissions are not checked on Windows";
#else
        SocketTree tree;
        const auto directory = tree.make_directory("shared");
        fs::permissions(directory, fs::perms::others_write, fs::perm_options::add);

        const auto open = P::acquire_unix_socket(at(directory / "lila.sock"), never_alive);
        ASSERT_FALSE(open.has_value());
        EXPECT_EQ(reason_of(open), Reason::Unsafe);
        ASSERT_EQ(open.error().tried.size(), 1U);
        EXPECT_EQ(open.error().tried.front(), directory);

        fs::permissions(directory, fs::perms::sticky_bit, fs::perm_options::add);
        EXPECT_TRUE(P::acquire_unix_socket(at(directory / "lila.sock"), never_alive).has_value());
#endif
    }

    TEST(UnixSocketTest, AGroupWritableDirectoryIsAccepted)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "directory permissions are not checked on Windows";
#else
        SocketTree tree;
        const auto directory = tree.make_directory("group");
        fs::permissions(directory, fs::perms::group_write, fs::perm_options::add);

        EXPECT_TRUE(P::acquire_unix_socket(at(directory / "lila.sock"), never_alive).has_value());
#endif
    }

    TEST(UnixSocketTest, AnUnsafeRuntimeDirectoryIsRefusedForANameToo)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "directory permissions are not checked on Windows";
#else
        SocketTree tree;
        const auto directory = tree.make_directory("runtime");
        fs::permissions(directory, fs::perms::others_write, fs::perm_options::add);
        const std::string text = directory.string();
        MayaFlux::Test::ScopedEnv variable(runtime_variable(), text.c_str());

        const auto result = P::acquire_unix_socket(named("t.sock"), never_alive);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(reason_of(result), Reason::Unsafe);
#endif
    }

    // ---------------------------------------------------------------------
    // What is already at the path
    // ---------------------------------------------------------------------

    TEST(UnixSocketTest, ARegularFileIsNotASocketAndIsLeftAlone)
    {
        SocketTree tree;
        const auto path = tree.make_file("lila.sock", "precious");
        int calls = 0;

        const auto result = P::acquire_unix_socket(at(path), [&](const fs::path&) {
            ++calls;
            return false;
        });

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(reason_of(result), Reason::NotSocket);
        EXPECT_EQ(calls, 0);
        EXPECT_TRUE(fs::is_regular_file(path));
        EXPECT_EQ(fs::file_size(path), 8U);
    }

    TEST(UnixSocketTest, ADirectoryAtThePathIsNotASocketAndIsLeftAlone)
    {
        SocketTree tree;
        const auto path = tree.make_directory("lila.sock");
        tree.make_file("lila.sock/inner");

        const auto result = P::acquire_unix_socket(at(path), never_alive);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(reason_of(result), Reason::NotSocket);
        EXPECT_TRUE(fs::exists(path / "inner"));
    }

    TEST(UnixSocketTest, ASymbolicLinkIsNeverFollowedOrRemoved)
    {
        SocketTree tree;
        const auto target = tree.make_file("target.txt", "keep");
        const auto link = tree.root() / "lila.sock";
        std::error_code ec;
        fs::create_symlink(target, link, ec);
        if (ec) {
            GTEST_SKIP() << "symlinks unavailable";
        }

        const auto result = P::acquire_unix_socket(at(link), always_alive);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(reason_of(result), Reason::NotSocket);
        EXPECT_TRUE(fs::is_symlink(link));
        EXPECT_TRUE(fs::exists(target));
    }

    TEST(UnixSocketTest, ADanglingSymbolicLinkIsNotRemovedEither)
    {
        SocketTree tree;
        const auto link = tree.root() / "lila.sock";
        std::error_code ec;
        fs::create_symlink(tree.root() / "gone", link, ec);
        if (ec) {
            GTEST_SKIP() << "symlinks unavailable";
        }

        const auto result = P::acquire_unix_socket(at(link), never_alive);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(reason_of(result), Reason::NotSocket);
        EXPECT_TRUE(fs::is_symlink(link));
    }

    TEST(UnixSocketTest, IsUnixSocketRejectsMissingPathsFilesAndDirectories)
    {
        SocketTree tree;

        EXPECT_FALSE(P::is_unix_socket(tree.root() / "absent.sock"));
        EXPECT_FALSE(P::is_unix_socket(tree.make_file("plain.sock")));
        EXPECT_FALSE(P::is_unix_socket(tree.make_directory("dir.sock")));
    }

    TEST(UnixSocketTest, IsUnixSocketRecognisesASocketFileAndNotALinkToIt)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "socket files are not created here on Windows";
#else
        SocketTree tree;
        const auto real = tree.root() / "real.sock";
        ASSERT_TRUE(make_socket_file(real));

        EXPECT_TRUE(P::is_unix_socket(real));

        const auto link = tree.root() / "link.sock";
        std::error_code ec;
        fs::create_symlink(real, link, ec);
        if (!ec) {
            EXPECT_FALSE(P::is_unix_socket(link));
        }
#endif
    }

    TEST(UnixSocketTest, ASymbolicLinkToARealSocketLeavesTheSocketAlone)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "socket files are not created here on Windows";
#else
        SocketTree tree;
        const auto real = tree.root() / "real.sock";
        ASSERT_TRUE(make_socket_file(real));
        const auto link = tree.root() / "lila.sock";
        std::error_code ec;
        fs::create_symlink(real, link, ec);
        if (ec) {
            GTEST_SKIP() << "symlinks unavailable";
        }

        const auto result = P::acquire_unix_socket(at(link), never_alive);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(reason_of(result), Reason::NotSocket);
        EXPECT_EQ(fs::symlink_status(real).type(), fs::file_type::socket);
        EXPECT_TRUE(fs::is_symlink(link));
#endif
    }

    TEST(UnixSocketTest, AControlCharacterInAnExplicitPathIsRefusedBeforeTheFilesystemSeesIt)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "socket files are not created here on Windows";
#else
        SocketTree tree;
        const auto real = tree.root() / "lila.sock";
        ASSERT_TRUE(make_socket_file(real));
        int calls = 0;
        const fs::path cut_short(real.string() + std::string("\0junk", 5));

        const auto result = P::acquire_unix_socket(at(cut_short), [&](const fs::path&) {
            ++calls;
            return false;
        });

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(reason_of(result), Reason::InvalidName);
        EXPECT_EQ(calls, 0);
        EXPECT_EQ(fs::symlink_status(real).type(), fs::file_type::socket);

        for (const char* control : { "\n", "\t", "\x7F" }) {
            const auto refused = P::acquire_unix_socket(at(fs::path(real.string() + control)), never_alive);
            ASSERT_FALSE(refused.has_value());
            EXPECT_EQ(reason_of(refused), Reason::InvalidName);
        }
#endif
    }

    TEST(UnixSocketTest, AStaleSocketIsRemovedWhenNothingAnswers)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "socket files are not created here on Windows";
#else
        SocketTree tree;
        const auto path = tree.root() / "lila.sock";
        ASSERT_TRUE(make_socket_file(path));
        ASSERT_EQ(fs::symlink_status(path).type(), fs::file_type::socket);
        int calls = 0;
        fs::path probed;

        const auto result = P::acquire_unix_socket(at(path), [&](const fs::path& candidate) {
            ++calls;
            probed = candidate;
            return false;
        });

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->replaced_stale);
        EXPECT_EQ(result->path, path);
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(probed, path);
        EXPECT_FALSE(fs::exists(path));
        EXPECT_TRUE(make_socket_file(result->path));
#endif
    }

    TEST(UnixSocketTest, ASecondRequestAfterTheCleanupFindsNothingToProbe)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "socket files are not created here on Windows";
#else
        SocketTree tree;
        const auto path = tree.root() / "lila.sock";
        ASSERT_TRUE(make_socket_file(path));
        int calls = 0;
        const auto probe = [&](const fs::path&) {
            ++calls;
            return false;
        };

        ASSERT_TRUE(P::acquire_unix_socket(at(path), probe).has_value());
        const auto again = P::acquire_unix_socket(at(path), probe);

        ASSERT_TRUE(again.has_value());
        EXPECT_FALSE(again->replaced_stale);
        EXPECT_EQ(calls, 1);
#endif
    }

    TEST(UnixSocketTest, ALiveSocketIsInUseAndIsLeftAlone)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "socket files are not created here on Windows";
#else
        SocketTree tree;
        const auto path = tree.root() / "lila.sock";
        ASSERT_TRUE(make_socket_file(path));

        const auto result = P::acquire_unix_socket(at(path), always_alive);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(reason_of(result), Reason::InUse);
        ASSERT_EQ(result.error().tried.size(), 1U);
        EXPECT_EQ(result.error().tried.front(), path);
        EXPECT_EQ(fs::symlink_status(path).type(), fs::file_type::socket);
#endif
    }

    TEST(UnixSocketTest, AProbeThatThrowsLeavesTheSocketInPlace)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "socket files are not created here on Windows";
#else
        SocketTree tree;
        const auto path = tree.root() / "lila.sock";
        ASSERT_TRUE(make_socket_file(path));

        EXPECT_THROW(
            (void)P::acquire_unix_socket(at(path), [](const fs::path&) -> bool { throw std::runtime_error("probe failed"); }),
            std::runtime_error);

        EXPECT_EQ(fs::symlink_status(path).type(), fs::file_type::socket);
#endif
    }

    TEST(UnixSocketTest, AStaleSocketInARuntimeDirectoryIsFoundThroughAName)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "socket files are not created here on Windows";
#else
        SocketTree tree;
        const std::string directory = tree.root().string();
        MayaFlux::Test::ScopedEnv variable(runtime_variable(), directory.c_str());
        ASSERT_TRUE(make_socket_file(tree.root() / "t.sock"));

        const auto live = P::acquire_unix_socket(named("t.sock"), always_alive);
        const auto stale = P::acquire_unix_socket(named("t.sock"), never_alive);

        ASSERT_FALSE(live.has_value());
        EXPECT_EQ(reason_of(live), Reason::InUse);
        ASSERT_TRUE(stale.has_value());
        EXPECT_TRUE(stale->replaced_stale);
#endif
    }

    // ---------------------------------------------------------------------
    // Restricting the bound socket
    // ---------------------------------------------------------------------

    TEST(UnixSocketTest, RestrictingASocketLeavesOwnerReadAndWriteOnly)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "access is not changed on Windows";
#else
        SocketTree tree;
        const auto path = tree.root() / "lila.sock";
        ASSERT_TRUE(make_socket_file(path));
        fs::permissions(path, fs::perms::all, fs::perm_options::replace);

        EXPECT_TRUE(P::restrict_unix_socket_to_owner(path).has_value());

        EXPECT_EQ(fs::status(path).permissions() & fs::perms::mask,
            fs::perms::owner_read | fs::perms::owner_write);
#endif
    }

    TEST(UnixSocketTest, RestrictingAPathThatIsNotThereIsRefused)
    {
#if defined(MAYAFLUX_PLATFORM_WINDOWS)
        GTEST_SKIP() << "access is not changed on Windows";
#else
        SocketTree tree;

        const auto result = P::restrict_unix_socket_to_owner(tree.root() / "absent.sock");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().reason, Reason::Unsafe);
#endif
    }

    // ---------------------------------------------------------------------
    // Hostile input
    // ---------------------------------------------------------------------

    TEST(UnixSocketTest, RandomPathsNeverThrowAndNeverRemoveAnything)
    {
        SocketTree tree;
        const std::string alphabet = std::string("ab./\\ :\n\t\x7F\xC3\xA9", 12) + std::string(1, '\0');
        int probes = 0;

        for (uint64_t seed = 1; seed <= 500; ++seed) {
            std::mt19937_64 rng(seed);
            std::string name;
            const size_t length = static_cast<size_t>(rng() % 140);
            for (size_t index = 0; index < length; ++index) {
                name.push_back(alphabet[static_cast<size_t>(rng() % alphabet.size())]);
            }
            const fs::path path(tree.root().string() + "/" + name);

            P::Acquired<P::UnixSocket> result = std::unexpected(P::AcquireFailure {});
            ASSERT_NO_THROW(result = P::acquire_unix_socket(at(path), [&](const fs::path&) {
                ++probes;
                return true;
            })) << "seed " << seed;

            if (result.has_value()) {
                ASSERT_TRUE(result->path.is_absolute()) << "seed " << seed;
                ASSERT_LE(result->path.u8string().size(), P::unix_socket_path_limit) << "seed " << seed;
                ASSERT_FALSE(result->replaced_stale) << "seed " << seed;
            }
        }
        EXPECT_EQ(probes, 0);
    }

}
}
