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
// Logging helper
// -------------------------------------------------------------------
static void log(const std::string& msg) {
    std::cerr << "[run-node] PID=" << getpid() << " " << msg << std::endl;
}

// -------------------------------------------------------------------
// Fork/exec wrapper with logging
// -------------------------------------------------------------------
static bool run_process(const fs::path& exe, char* const argv[], const std::string& label) {
    pid_t pid = fork();
    if (pid == 0) {
        // ---- CHILD ----
        log("Child for " + label + " starting exec: " + exe.string());
        execv(exe.c_str(), argv);
        perror("[child] execv failed");
        _exit(127);
    }

    if (pid < 0) {
        perror("fork failed");
        return false;
    }

    log("Waiting for " + label + " pid=" + std::to_string(pid));
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid failed");
        return false;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        log(label + " exited with code " + std::to_string(code));
        return code == 0;
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        log(label + " killed by signal " + std::to_string(sig));
        return false;
    } else {
        log(label + " ended unexpectedly");
        return false;
    }
}

// -------------------------------------------------------------------
// Run `corepack install`
// -------------------------------------------------------------------
bool run_corepack_install(const fs::path& projectNodeBin, const fs::path& projectNodeDir) {
    fs::path corepackJs = projectNodeDir / "lib" / "node_modules" / "corepack" / "dist" / "corepack.js";
    if (!fs::exists(corepackJs)) {
        log("corepack.js not found at " + corepackJs.string());
        return false;
    }

    log("About to run corepack install via: " + projectNodeBin.string());

    char* args[] = {
        const_cast<char*>(projectNodeBin.c_str()),
        const_cast<char*>(corepackJs.c_str()),
        const_cast<char*>("install"),
        nullptr
    };

    return run_process(projectNodeBin, args, "corepack install");
}

// -------------------------------------------------------------------
// Run package manager install (npm/pnpm/yarn via corepack)
// -------------------------------------------------------------------
bool run_package_manager_install(const fs::path& projectNodeBin, const fs::path& projectRoot) {
    fs::path pkgJson = projectRoot / "package.json";

    if (!fs::exists(pkgJson)) {
        log("No package.json found, skipping dependency install.");
        return true;
    }

    boost::property_tree::ptree pt;
    boost::property_tree::read_json(pkgJson.string(), pt);

    std::string pmRef;
    try {
        pmRef = pt.get<std::string>("packageManager");
    } catch (...) {
        log("packageManager not specified in package.json, defaulting to npm@latest");
        pmRef = "npm@latest";
    }

    log("packageManager reference: " + pmRef);

    auto atPos = pmRef.find('@');
    std::string pmName = (atPos != std::string::npos) ? pmRef.substr(0, atPos) : pmRef;
    std::string pmVersion = (atPos != std::string::npos) ? pmRef.substr(atPos + 1) : "latest";

    (void)pmVersion; // unused for now

    fs::path corepackJs = projectRoot / ".node" / "lib" / "node_modules" / "corepack" / "dist" / "corepack.js";

    char* args[] = {
        const_cast<char*>(projectNodeBin.c_str()),
        const_cast<char*>(corepackJs.c_str()),
        const_cast<char*>(pmName.c_str()),
        const_cast<char*>("install"),
        nullptr
    };

    return run_process(projectNodeBin, args, "package manager install");
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------
int main(int argc, char* argv[]) {
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
        log("Downloading Node.js index.json");
        std::string jsonStr = https_get_string("nodejs.org", "/dist/index.json");
        std::stringstream ss(jsonStr);
        boost::property_tree::ptree pt;
        boost::property_tree::read_json(ss, pt);

        std::string targetVersion;
        int cachedMajor = -1;
        if (fs::exists(versionFile)) {
            std::ifstream fin(versionFile);
            fin >> cachedMajor;
            log("Cached major version: " + std::to_string(cachedMajor));
        }

        for (auto& entry : pt) {
            auto& obj = entry.second;
            try {
                std::string lts = obj.get<std::string>("lts");
                bool isLTS = !lts.empty();
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
            log("No suitable Node.js LTS found.");
            return 1;
        }
        log("Using Node.js version: " + targetVersion);

        std::string filename = "node-" + targetVersion + "-linux-x64.tar.xz";
        std::string target   = "/dist/" + targetVersion + "/" + filename;

        fs::path archivePath = archivesDir / filename;
        fs::path extractDir  = versionsDir / targetVersion;
        fs::path nodeBin     = extractDir / "bin" / "node";

        if (!fs::exists(archivePath)) {
            log("Downloading " + target);
            https_download("nodejs.org", target, archivePath);
        }

        if (!fs::exists(nodeBin)) {
            fs::create_directories(extractDir);
            log("Extracting to " + extractDir.string());
            if (!extract_tar_xz(archivePath.string(), extractDir.string())) {
                log("Extraction failed");
                return 1;
            }
        } else {
            log("Using cached extraction: " + extractDir.string());
        }

        // --- CLEANUP PROJECT .node ---
        for (auto& e : fs::directory_iterator(projectNodeDir)) {
            if (e.path() == versionFile) continue; // keep version.txt
            fs::remove_all(e.path());
        }

        // Symlink store contents into .node
        for (auto& entry : fs::directory_iterator(extractDir)) {
            fs::path name = entry.path().filename();
            fs::path dest = projectNodeDir / name;

            if (fs::exists(dest) || fs::is_symlink(dest))
                fs::remove_all(dest);

            fs::create_symlink(entry.path(), dest);
        }

        fs::path projectNodeBin = projectNodeDir / "bin" / "node";
        if (!fs::exists(projectNodeBin)) {
            log("Node binary not found in .node/bin");
            return 1;
        }

        // Run installs if needed
        fs::path pkgJson = projectRoot / "package.json";
        if (fs::exists(pkgJson)) {
            if (!run_corepack_install(projectNodeBin, projectNodeDir)) {
                log("corepack install failed");
                return 1;
            }
            if (!run_package_manager_install(projectNodeBin, projectRoot)) {
                log("dependency install failed");
                return 1;
            }
        }

        if (argc < 2) {
            log(std::string("Usage: ") + argv[0] + " <args to node>");
            return 1;
        }

        // Build argv
        std::vector<char*> newArgs;
        newArgs.push_back(const_cast<char*>(projectNodeBin.c_str()));
        for (int i = 1; i < argc; i++)
            newArgs.push_back(argv[i]);
        newArgs.push_back(nullptr);

        log("Launching Node…");

        bool ok = run_process(projectNodeBin, newArgs.data(), "node main");
        return ok ? 0 : 1;
    }
    catch(const std::exception& e) {
        log(std::string("Error: ") + e.what());
        return 1;
    }
}