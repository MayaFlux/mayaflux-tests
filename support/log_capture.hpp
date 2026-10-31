#pragma once


#include "MayaFlux/Journal/Archivist.hpp"
#include "MayaFlux/Journal/Sink.hpp"

namespace MayaFlux::Test {

/**
 * @brief Records journal entries at warning severity or above for tests to read.
 *
 * install() adds one sink to the Archivist for the rest of the run and leaves
 * the others in place. messages() drains the journal first, so everything
 * logged before the call is visible.
 */
class LogCapture {
public:
    static void install()
    {
        static const bool installed = [] {
            Journal::Archivist::instance().add_sink(std::make_unique<Sink>());
            return true;
        }();
        static_cast<void>(installed);
    }

    static void clear()
    {
        Journal::Archivist::instance().flush();
        const std::lock_guard lock(store().mutex);
        store().messages.clear();
    }

    [[nodiscard]] static std::vector<std::string> messages()
    {
        Journal::Archivist::instance().flush();
        const std::lock_guard lock(store().mutex);
        return store().messages;
    }

    [[nodiscard]] static bool contains(const std::string& needle)
    {
        for (const auto& message : messages()) {
            if (message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    struct Store {
        std::mutex mutex;
        std::vector<std::string> messages;
    };

    static Store& store()
    {
        static Store instance;
        return instance;
    }

    class Sink final : public Journal::Sink {
    public:
        void write(const Journal::JournalEntry& entry) override { record(entry.severity, entry.message); }

        void write(const Journal::RealtimeEntry& entry) override { record(entry.severity, entry.message); }

        void flush() override { }

        [[nodiscard]] bool is_available() const override { return true; }

    private:
        static void record(Journal::Severity severity, std::string_view message)
        {
            if (severity < Journal::Severity::WARN) {
                return;
            }
            const std::lock_guard lock(store().mutex);
            store().messages.emplace_back(message);
        }
    };
};

}
