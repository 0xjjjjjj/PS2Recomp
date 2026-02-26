namespace
{
    constexpr char kMc0Prefix[] = "mc0:";

    std::string toLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::string stripIsoVersionSuffix(std::string value)
    {
        const std::size_t semicolon = value.find(';');
        if (semicolon == std::string::npos)
        {
            return value;
        }

        bool numericSuffix = semicolon + 1 < value.size();
        for (std::size_t i = semicolon + 1; i < value.size(); ++i)
        {
            if (!std::isdigit(static_cast<unsigned char>(value[i])))
            {
                numericSuffix = false;
                break;
            }
        }

        if (numericSuffix)
        {
            value.erase(semicolon);
        }
        return value;
    }

    std::string normalizePs2PathSuffix(std::string suffix)
    {
        std::replace(suffix.begin(), suffix.end(), '\\', '/');
        suffix = stripIsoVersionSuffix(std::move(suffix));
        while (!suffix.empty() && (suffix.front() == '/' || suffix.front() == '\\'))
        {
            suffix.erase(suffix.begin());
        }
        return suffix;
    }

    std::filesystem::path getConfiguredHostRoot()
    {
        const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
        if (!paths.hostRoot.empty())
        {
            return paths.hostRoot;
        }
        if (!paths.elfDirectory.empty())
        {
            return paths.elfDirectory;
        }

        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        return ec ? std::filesystem::path(".") : cwd.lexically_normal();
    }

    std::filesystem::path getConfiguredCdRoot()
    {
        const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
        if (!paths.cdRoot.empty())
        {
            return paths.cdRoot;
        }
        if (!paths.elfDirectory.empty())
        {
            std::error_code ec;
            // Check if elfDirectory itself contains SYSTEM.CNF (disc root = elfDirectory)
            if (std::filesystem::exists(paths.elfDirectory / "SYSTEM.CNF", ec) && !ec)
            {
                return paths.elfDirectory;
            }
            // Check subdirectories for SYSTEM.CNF (e.g. bin/disc/)
            for (auto &entry : std::filesystem::directory_iterator(paths.elfDirectory, ec))
            {
                if (ec)
                    break;
                if (!entry.is_directory(ec) || ec)
                    continue;
                std::error_code ec2;
                if (std::filesystem::exists(entry.path() / "SYSTEM.CNF", ec2) && !ec2)
                {
                    std::cerr << "[cdRoot] auto-detected disc root: " << entry.path().string() << std::endl;
                    return entry.path();
                }
            }
            return paths.elfDirectory;
        }

        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        return ec ? std::filesystem::path(".") : cwd.lexically_normal();
    }

    std::filesystem::path getConfiguredMcRoot()
    {
        const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
        if (!paths.mcRoot.empty())
        {
            return paths.mcRoot;
        }
        if (!paths.elfDirectory.empty())
        {
            return paths.elfDirectory / "mc0";
        }

        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        return ec ? std::filesystem::path("mc0") : (cwd / "mc0").lexically_normal();
    }
}
