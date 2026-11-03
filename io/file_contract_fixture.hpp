#pragma once

#include "gtest/gtest.h"

#include "../support/scoped_working_dir.hpp"

namespace MayaFlux::Test {

class FileContractFixture : public ::testing::Test {
protected:
    [[nodiscard]] std::filesystem::path root() const
    {
        return std::filesystem::current_path();
    }

    void write(const std::filesystem::path& relative, const std::string& content = "data") const
    {
        m_files.write(relative, content);
    }

private:
    ScopedWorkingDir m_files;
};

}
