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
        std::cerr <<    msg << std::endl;
}

// -------------------------------------------------------------------
// Fork/exec wrapper with logging
// -------------------------------------------------------------------
static bool run_process(const fs::path& exe, char* const argv[], const std::string& label) {
    pid_t pid = fork();
    if (pid == 0) {
        execv(exe.c_str(), argv);
        perror("[child] execv failed");
        _exit(127);
    }

    if (pid < 0) {
        perror("fork failed");
        return false;
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid failed");
        return false;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        return code == 0;
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        return false;
    } else {
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

        // Optional pinned version
        std::string pinnedVersion;
        int nodeArgStart = 1; 

        if (argc > 1) {
            std::string arg1 = argv[1];
            // crude check: starts with "v" and digit
            if (arg1.size() > 1 && arg1[0] == 'v' && isdigit(arg1[1])) {
                pinnedVersion = arg1;
                nodeArgStart = 2;  // node gets args starting from argv[2]
            }
        }

        // Fetch index.json (we still need metadata for downloading tarballs)
        std::string jsonStr = https_get_string("nodejs.org", "/dist/index.json");
        std::stringstream ss(jsonStr);
        boost::property_tree::ptree pt;
        boost::property_tree::read_json(ss, pt);

        std::string targetVersion;
        if (!pinnedVersion.empty()) {
            // Just use pinned version, skip LTS logic, no update
            targetVersion = pinnedVersion;
            log("Pinned Node.js version: " + targetVersion);
        } else {
            // ---- EXISTING LTS SELECTION LOGIC ----
            int cachedMajor = -1;
            if (fs::exists(versionFile)) {
                std::ifstream fin(versionFile);
                fin >> cachedMajor;
            }

            int bestMajor = -1;
            std::string bestVersion;

            for (auto& entry : pt) {
                auto& obj = entry.second;
                try {
                    auto ltsOpt = obj.get_optional<std::string>("lts");
                    if (ltsOpt && *ltsOpt != "false" && !ltsOpt->empty()) {
                        std::string v = obj.get<std::string>("version");
                        int major = std::stoi(v.substr(1, v.find('.', 1) - 1));

                        if (cachedMajor != -1) {
                            if (major == cachedMajor && bestVersion.empty())
                                bestVersion = v;
                        } else {
                            if (major > bestMajor) {
                                bestMajor = major;
                                bestVersion = v;
                            }
                        }
                    }
                } catch (...) {}
            }

            if (cachedMajor == -1 && bestMajor != -1) {
                cachedMajor = bestMajor;
                std::ofstream fout(versionFile);
                fout << cachedMajor;
            }

            targetVersion = bestVersion;
            log("Using Node.js version: " + targetVersion);
        }

        // --- rest of your logic (downloading, extracting, symlinking, installs) ---

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
        }

        // cleanup `.node` contents except versionFile
        for (auto& e : fs::directory_iterator(projectNodeDir)) {
            if (e.path() == versionFile) continue;
            fs::remove_all(e.path());
        }
        for (auto& entry : fs::directory_iterator(extractDir)) {
            fs::path name = entry.path().filename();
            fs::path dest = projectNodeDir / name;
            if (fs::exists(dest) || fs::is_symlink(dest)) fs::remove_all(dest);
            fs::create_symlink(entry.path(), dest);
        }

        fs::path projectNodeBin = projectNodeDir / "bin" / "node";
        if (!fs::exists(projectNodeBin)) {
            log("Node binary not found in .node/bin");
            return 1;
        }

        // check for package.json
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

        if (argc <= nodeArgStart) {
            log(std::string("Usage: ") + argv[0] + " [vX.Y.Z] <args to node>");
            return 1;
        }

        // build argv for node
        std::vector<char*> newArgs;
        newArgs.push_back(const_cast<char*>(projectNodeBin.c_str()));
        for (int i = nodeArgStart; i < argc; i++)
            newArgs.push_back(argv[i]);
        newArgs.push_back(nullptr);

        bool ok = run_process(projectNodeBin, newArgs.data(), "node main");
        return ok ? 0 : 1;
    }
    catch(const std::exception& e) {
        log(std::string("Error: ") + e.what());
        return 1;
    }
}