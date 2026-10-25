#pragma once

#include "network_buffer_fixture.hpp"

#include <algorithm>
#include <numeric>
#include <set>

#include "MayaFlux/Buffers/Network/SpatialHashProcessor.hpp"
#include "MayaFlux/Nodes/Network/AssemblyNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/GpuFieldOperator.hpp"

namespace MayaFlux::Test {

    using Buffers::HashClearProcessor;
    using Buffers::HashCountProcessor;
    using Buffers::HashDensityColorProcessor;
    using Buffers::HashScanProcessor;
    using Buffers::HashScatterProcessor;
    using Buffers::SpatialHashConfig;
    using Nodes::Network::GpuFieldOperator;
    using Nodes::Network::SpatialFieldConfig;

    constexpr uint32_t k_particles = 300;
    constexpr float k_cell = 0.5F;
    constexpr size_t k_color_word = 3;

    struct Grid {
        glm::vec3 min { -1.0F };
        glm::uvec3 dims { 4U };
        float cell { k_cell };

        [[nodiscard]] uint32_t cell_count() const { return dims.x * dims.y * dims.z; }

        [[nodiscard]] uint32_t cell_of(const glm::vec3& p) const
        {
            glm::ivec3 c = glm::ivec3(glm::floor((p - min) / cell));
            c = glm::clamp(c, glm::ivec3(0), glm::ivec3(dims) - glm::ivec3(1));
            return static_cast<uint32_t>(c.x) + static_cast<uint32_t>(c.y) * dims.x
                + static_cast<uint32_t>(c.z) * dims.x * dims.y;
        }
    };

    inline std::vector<float> floats_of(const std::vector<uint8_t>& bytes)
    {
        std::vector<float> values(bytes.size() / sizeof(float));
        std::memcpy(values.data(), bytes.data(), values.size() * sizeof(float));
        return values;
    }

    class NetworkHashTest : public NetworkBufferDeviceTest {
    protected:
        void SetUp() override
        {
            NetworkBufferDeviceTest::SetUp();
            if (IsSkipped())
                return;

            auto particles = std::make_shared<Nodes::Network::ParticleNetwork>(
                k_particles, glm::vec3(-1.0F), glm::vec3(1.0F),
                Kinesis::SpatialDistribution::RANDOM_VOLUME);
            particles->create_operator<Nodes::Network::PhysicsOperator>();
            attach(particles, 1.0F);

            const auto made = SpatialHashConfig::from_network(buffer, k_cell);
            ASSERT_TRUE(made.has_value());
            config = *made;
            ASSERT_TRUE(buffer->ensure_cluster_ids());
            config.declare_fields(buffer);

            buffer->process_default();
            stride_words = config.stride_words;
            refresh_positions();
        }

        void refresh_positions()
        {
            const auto floats = floats_of(read_vertices(static_cast<size_t>(k_particles) * stride_words * 4));
            positions.resize(k_particles);
            for (uint32_t i = 0; i < k_particles; ++i) {
                const size_t base = static_cast<size_t>(i) * stride_words;
                positions[i] = { floats[base], floats[base + 1], floats[base + 2] };
            }
        }

        void add(const std::shared_ptr<Buffers::BufferProcessor>& processor)
        {
            ASSERT_TRUE(buffer->get_processing_chain()->add_processor(processor, buffer));
        }

        void run_stages()
        {
            buffer->get_processing_chain()->process_complete(buffer);
        }

        void add_hash_build(bool gate_alive = false)
        {
            add(std::make_shared<HashClearProcessor>(config));
            add(std::make_shared<HashCountProcessor>(config, gate_alive));
            add(std::make_shared<HashScanProcessor>(config));
            add(std::make_shared<HashScatterProcessor>(config, gate_alive));
        }

        [[nodiscard]] std::vector<uint32_t> expected_counts(const std::vector<bool>& alive = {}) const
        {
            std::vector<uint32_t> counts(grid.cell_count(), 0U);
            for (uint32_t i = 0; i < k_particles; ++i) {
                if (alive.empty() || alive[i])
                    ++counts[grid.cell_of(positions[i])];
            }
            return counts;
        }

        void declare_alive(const std::vector<bool>& alive)
        {
            ASSERT_TRUE(buffer->declare_state("mutation_alive", k_particles, sizeof(uint32_t), false));
            std::vector<uint32_t> flags(k_particles);
            for (uint32_t i = 0; i < k_particles; ++i)
                flags[i] = alive[i] ? 1U : 0U;
            write_state<uint32_t>("mutation_alive", flags);
        }

        void poke_position(uint32_t index, const glm::vec3& value)
        {
            auto binding = buffer->get_processor()->get_binding(buffer->get_binding_name());
            Buffers::upload_to_gpu(
                &value, sizeof(value), buffer, binding->staging_buffer,
                static_cast<size_t>(index) * stride_words * 4);
            positions[index] = value;
        }

        [[nodiscard]] std::vector<glm::vec3> read_colors()
        {
            const auto floats = floats_of(read_vertices(static_cast<size_t>(k_particles) * stride_words * 4));
            std::vector<glm::vec3> colors(k_particles);
            for (uint32_t i = 0; i < k_particles; ++i) {
                const size_t base = static_cast<size_t>(i) * stride_words + k_color_word;
                colors[i] = { floats[base], floats[base + 1], floats[base + 2] };
            }
            return colors;
        }

        [[nodiscard]] uint32_t neighbours_of(uint32_t i) const
        {
            uint32_t count = 0;
            for (uint32_t j = 0; j < k_particles; ++j) {
                if (j != i && glm::length(positions[j] - positions[i]) < k_cell)
                    ++count;
            }
            return count;
        }

        [[nodiscard]] static glm::vec3 heat_of(float density)
        {
            const glm::vec3 ember { 0.03F, 0.0F, 0.06F };
            const glm::vec3 fire { 0.95F, 0.25F, 0.02F };
            const glm::vec3 white_hot { 1.0F, 0.95F, 0.7F };
            return density < 0.5F
                ? glm::mix(ember, fire, density * 2.0F)
                : glm::mix(fire, white_hot, (density - 0.5F) * 2.0F);
        }

        SpatialHashConfig config {};
        Grid grid;
        uint32_t stride_words {};
        std::vector<glm::vec3> positions;
    };

} // namespace MayaFlux::Test
