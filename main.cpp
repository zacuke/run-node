#include "https_download.h"
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

namespace fs   = std::filesystem;

// -------------------------------------------------------------------
// Extraction with libarchive
// -------------------------------------------------------------------
bool extract_tar_xz(const std::string& archivePath, const std::string& destDir) {
    struct archive *a;
    struct archive *ext;
    struct archive_entry *entry;
    int r;

    a = archive_read_new();
    archive_read_support_filter_xz(a);
    archive_read_support_format_tar(a);

    if ((r = archive_read_open_filename(a, archivePath.c_str(), 10240)) != ARCHIVE_OK) {
        std::cerr << "archive_read_open_filename failed: "
                  << archive_error_string(a) << "\n";
        return false;
    }

    ext = archive_write_disk_new();
    archive_write_disk_set_options(ext,
        ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
        ARCHIVE_EXTRACT_ACL  | ARCHIVE_EXTRACT_FFLAGS);
    archive_write_disk_set_standard_lookup(ext);

    while (true) {
        r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_OK)
            std::cerr << "Warning: " << archive_error_string(a) << "\n";
        if (r < ARCHIVE_WARN) {
            archive_read_close(a);
            archive_read_free(a);
            archive_write_close(ext);
            archive_write_free(ext);
            return false;
        }

        // --strip-components=1
        std::string path = archive_entry_pathname(entry);
        auto slashPos = path.find('/');
        if (slashPos != std::string::npos)
            path = path.substr(slashPos + 1);
        if (path.empty()) continue;

        std::string fullOutputPath = destDir + "/" + path;
        archive_entry_set_pathname(entry, fullOutputPath.c_str());

        r = archive_write_header(ext, entry);
        if (r >= ARCHIVE_OK && archive_entry_size(entry) > 0) {
            const void *buff;
            size_t size;
            la_int64_t offset;
            while (true) {
                r = archive_read_data_block(a, &buff, &size, &offset);
                if (r == ARCHIVE_EOF) break;
                if (r < ARCHIVE_OK) break;
                r = archive_write_data_block(ext, buff, size, offset);
                if (r < ARCHIVE_OK) break;
            }
        }
        archive_write_finish_entry(ext);
    }

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
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

        if (argc < 2) {
            std::cerr << "Usage: " << argv[0] << " <args to node>\n";
            return 1;
        }

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