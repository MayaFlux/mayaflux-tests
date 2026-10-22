#pragma once

#include <memory>
#include <vector>

#include "MayaFlux/Vruta/Scheduler.hpp"

namespace MayaFlux::Test {

/**
 * @brief A TaskScheduler whose FRAME_ACCURATE domain is driven by hand.
 *
 * Registers the same frame processor GraphicsSubsystem installs, so
 * GraphicsRoutine tasks resume once per tick() instead of through the
 * sample-based default.
 */
struct FrameScheduler {
    std::shared_ptr<Vruta::TaskScheduler> scheduler;
    uint64_t frame {};

    explicit FrameScheduler(uint32_t sample_rate = 48000)
        : scheduler(std::make_shared<Vruta::TaskScheduler>(sample_rate))
    {
        scheduler->register_token_processor(Vruta::ProcessingToken::FRAME_ACCURATE,
            [this](const std::vector<std::shared_ptr<Vruta::Routine>>& tasks, uint64_t units) {
                for (uint64_t i = 0; i < units; ++i) {
                    for (const auto& routine : tasks) {
                        if (!routine || !routine->is_active())
                            continue;
                        if (!routine->requires_clock_sync() || frame + i >= routine->next_execution())
                            routine->try_resume(frame + i);
                    }
                }
            });
    }

    FrameScheduler(const FrameScheduler&) = delete;
    FrameScheduler& operator=(const FrameScheduler&) = delete;
    FrameScheduler(FrameScheduler&&) = delete;
    FrameScheduler& operator=(FrameScheduler&&) = delete;

    void tick(uint64_t count = 1)
    {
        scheduler->process_token(Vruta::ProcessingToken::FRAME_ACCURATE, count);
        frame += count;
    }
};

}
