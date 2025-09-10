#include "util/https_download.h"
#include "util/extract_tar_xz.h"
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <archive.h>
#include <archive_entry.h>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

// -------------------------------------------------------------------
// Run `corepack install` using the Node.js distribution we installed
// -------------------------------------------------------------------
bool run_corepack_install(const fs::path& projectNodeBin, const fs::path& projectNodeDir)
{
    fs::path corepackJs = projectNodeDir / "lib" / "node_modules" / "corepack" / "dist" / "corepack.js";
    if (!fs::exists(corepackJs)) {
        std::cerr << "[DEBUG] corepack.js not found at " << corepackJs << "\n";
        return false;
    }

    std::cout << "[DEBUG] About to run: " << projectNodeBin << " " << corepackJs << " install\n";

    pid_t pid = fork();
    if (pid == 0) {
        // ---- CHILD ----
        std::cerr << "[DEBUG:child] Forked successfully, PID=" << getpid() << "\n";

        char* args[] = {
            const_cast<char*>(projectNodeBin.c_str()),  // ./node
            const_cast<char*>(corepackJs.c_str()),      // ./corepack.js
            const_cast<char*>("install"),
            nullptr
        };

        // Show exec args
        std::cerr << "[DEBUG:child] execv() with args:\n";
        for (int i = 0; args[i] != nullptr; i++) {
            std::cerr << "   args[" << i << "] = " << args[i] << "\n";
        }

        execv(projectNodeBin.c_str(), args);

        // Only reached if execv fails:
        perror("[DEBUG:child] execv corepack.js failed");
        _exit(127);
    }

    // ---- PARENT ----
    if (pid < 0) {
        perror("[DEBUG] fork() failed");
        return false;
    }

    std::cout << "[DEBUG] Waiting on child " << pid << "...\n";

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("[DEBUG] waitpid failed");
        return false;
    }

    std::cout << "[DEBUG] Child finished. Status=" << status
              << " (WIFEXITED=" << WIFEXITED(status)
              << ", exit=" << (WIFEXITED(status) ? WEXITSTATUS(status) : -1)
              << ", WIFSIGNALED=" << WIFSIGNALED(status)
              << ", signal=" << (WIFSIGNALED(status) ? WTERMSIG(status) : -1)
              << ")\n";

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
bool run_package_manager_install(const fs::path& projectNodeBin, const fs::path& projectRoot) {
    fs::path pkgJson = projectRoot / "package.json";

    if (!fs::exists(pkgJson)) {
        std::cerr << "[DEBUG] No package.json found, skipping dependency install.\n";
        return true;
    }

    // Parse package.json
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(pkgJson.string(), pt);

    std::string pmRef;
    try {
        pmRef = pt.get<std::string>("packageManager");
    } catch (...) {
        std::cerr << "[DEBUG] packageManager not specified in package.json.\n";
        // fallback? npm is default
        pmRef = "npm@latest";
    }

    std::cout << "[DEBUG] packageManager reference: " << pmRef << "\n";

    auto atPos = pmRef.find('@');
    std::string pmName = (atPos != std::string::npos) ? pmRef.substr(0, atPos) : pmRef;
    std::string pmVersion = (atPos != std::string::npos) ? pmRef.substr(atPos + 1) : "latest";

    std::cout << "[DEBUG] Will run install via: corepack " << pmName << " install\n";

    pid_t pid = fork();
    if (pid == 0) {
        // In child process
        // Call node binary -> corepack.js <pmName> install
        fs::path corepackJs = projectRoot / ".node/lib/node_modules/corepack/dist/corepack.js";
        char* args[] = {
            const_cast<char*>(projectNodeBin.c_str()),
            const_cast<char*>(corepackJs.c_str()),
            const_cast<char*>(pmName.c_str()),
            const_cast<char*>("install"),
            nullptr
        };

        execv(projectNodeBin.c_str(), args);
        perror("[DEBUG:child] execv failed running package manager install");
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("[DEBUG] waitpid failed");
        return false;
    }

    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        std::cerr << "[DEBUG] Package manager install failed with exit status "
                  << (WIFEXITED(status) ? WEXITSTATUS(status) : -1) << "\n";
        return false;
    }

    return true;
}
// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------
int main(int argc, char* argv[])
{
    try {
        fs::path projectRoot    = fs::current_path();
        fs::path projectNodeDir = projectRoot / ".node";
        fs::create_directories(projectNodeDir);
        fs::path versionFile    = projectNodeDir / "version.txt";

        // Central Store paths
        fs::path storeDir    = fs::path(getenv("HOME")) / ".local/share/run-node";
        fs::path archivesDir = storeDir / "archives";
        fs::path versionsDir = storeDir / "versions";
        fs::create_directories(archivesDir);
        fs::create_directories(versionsDir);

        // Fetch index.json
        std::string jsonStr = https_get_string("nodejs.org", "/dist/index.json");
        std::stringstream ss(jsonStr);
        boost::property_tree::ptree pt;
        boost::property_tree::read_json(ss, pt);

        std::string targetVersion;
        int cachedMajor = -1;
        if (fs::exists(versionFile)) {
            std::ifstream fin(versionFile);
            fin >> cachedMajor;
            std::cout << "Cached major version: " << cachedMajor << "\n";
        }

        for (auto& entry : pt) {
            auto& obj = entry.second;
            try {
                bool isLTS = !obj.get<std::string>("lts").empty();
                if (isLTS) {
                    std::string v = obj.get<std::string>("version"); // "v20.11.1"
                    int major = std::stoi(v.substr(1, v.find('.', 1) - 1));

                    if (cachedMajor == -1) {
                        targetVersion = v;
                        cachedMajor   = major;
                        std::ofstream fout(versionFile);
                        fout << major;
                        break;
                    } else if (major == cachedMajor) {
                        targetVersion = v;
                        break;
                    }
                }
            } catch (...) {}
        }

        if (targetVersion.empty()) {
            std::cerr << "No suitable Node.js LTS found.\n";
            return 1;
        }
        std::cout << "Using Node.js version: " << targetVersion << "\n";

        std::string filename = "node-" + targetVersion + "-linux-x64.tar.xz";
        std::string target   = "/dist/" + targetVersion + "/" + filename;

        fs::path archivePath = archivesDir / filename;
        fs::path extractDir  = versionsDir / targetVersion;
        fs::path nodeBin     = extractDir / "bin" / "node";

        if (!fs::exists(archivePath)) {
            std::cout << "Downloading " << target << "\n";
            https_download("nodejs.org", target, archivePath);
        }

        if (!fs::exists(nodeBin)) {
            fs::create_directories(extractDir);
            std::cout << "Extracting to " << extractDir << "\n";
            if (!extract_tar_xz(archivePath.string(), extractDir.string())) {
                std::cerr << "Extraction failed\n";
                return 1;
            }
        } else {
            std::cout << "Using cached extraction: " << extractDir << "\n";
        }

        // --- CLEANUP PROJECT .node ---
        for (auto& e : fs::directory_iterator(projectNodeDir)) {
            if (e.path() == versionFile) continue; // keep physical version.txt
            fs::remove_all(e.path());
        }

        // Symlink store contents into .node (except version.txt)
        for (auto& entry : fs::directory_iterator(extractDir)) {
            fs::path name = entry.path().filename();
            fs::path dest = projectNodeDir / name;

            if (fs::exists(dest) || fs::is_symlink(dest))
                fs::remove_all(dest);

            fs::create_symlink(entry.path(), dest);
        }

        fs::path projectNodeBin = projectNodeDir / "bin" / "node";
        if (!fs::exists(projectNodeBin)) {
            std::cerr << "Node binary not found in .node/bin\n";
            return 1;
        }

        // --- Always run corepack install if package.json exists ---
        fs::path pkgJson = projectRoot / "package.json";
        if (fs::exists(pkgJson)) {
            if (!run_corepack_install(projectNodeBin, projectNodeDir)) {
                std::cerr << "corepack install failed\n";
                return 1;
            } 
            if (!run_package_manager_install(projectNodeBin, projectRoot)) {
                std::cerr << "dependency install failed\n";
                return 1;
            }
        }

        if (argc < 2) {
            std::cerr << "Usage: " << argv[0] << " <args to node>\n";
            return 1;
        }

        // Build argv for execv
        std::vector<char*> newArgs;
        newArgs.push_back(const_cast<char*>(projectNodeBin.c_str()));
        for (int i = 1; i < argc; i++)
            newArgs.push_back(argv[i]);
        newArgs.push_back(nullptr);

        std::cout << "Running " << projectNodeBin << "\n";
        execv(projectNodeBin.c_str(), newArgs.data());

        perror("execv failed");
        return 1;
    }
    catch(const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}