#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

// Download large file direct-to-disk
void https_download(const std::string& host, const std::string& target, const fs::path& outFile)
{
    net::io_context ioc;
    ssl::context ctx{ssl::context::sslv23_client};
    ctx.set_default_verify_paths();

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");

    auto const results = resolver.resolve(host, "443");
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "boost-beast-client");
    http::write(stream, req);

    beast::flat_buffer buffer;
    beast::error_code ec;

    http::response_parser<http::file_body> parser;
    parser.body_limit((std::numeric_limits<std::uint64_t>::max)());  // ❗ allow big downloads
    parser.get().body().open(outFile.string().c_str(), beast::file_mode::write, ec);
    if (ec) throw beast::system_error{ec};

    http::read(stream, buffer, parser, ec);
    if (ec && ec != http::error::end_of_stream)
        throw beast::system_error{ec};

    stream.shutdown(ec);
    if (ec == net::error::eof || ec == ssl::error::stream_truncated) ec = {};
    if (ec) throw beast::system_error{ec};
}

// Download small JSON -> string
std::string https_get_string(const std::string& host, const std::string& target)
{
    net::io_context ioc;
    ssl::context ctx{ssl::context::sslv23_client};
    ctx.set_default_verify_paths();

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");

    auto const results = resolver.resolve(host, "443");
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "boost-beast-client");
    http::write(stream, req);

    beast::flat_buffer buffer;

    http::response_parser<http::dynamic_body> parser;
    parser.body_limit((std::numeric_limits<std::uint64_t>::max)());  // important
    http::read(stream, buffer, parser);

    http::response<http::dynamic_body> res = parser.release();

    std::string body;
    for (auto const& b : res.body().data())
        body.append(static_cast<const char*>(b.data()), b.size());

    beast::error_code ec;
    stream.shutdown(ec);
    if (ec == net::error::eof || ec == ssl::error::stream_truncated) ec = {};
    if (ec) throw beast::system_error{ec};

    return body;
}

int main(int argc, char* argv[])
{
    try {
        fs::path tmp = fs::temp_directory_path() / "node-bootstrap";
        fs::create_directories(tmp);

        // 1. Fetch index.json
        std::cout << "Fetching Node.js versions index...\n";
        std::string jsonStr = https_get_string("nodejs.org", "/dist/index.json");

        // 2. Parse it
        std::stringstream ss(jsonStr);
        boost::property_tree::ptree pt;
        boost::property_tree::read_json(ss, pt);

        std::string version;
        for (auto &entry : pt) {
            auto &obj = entry.second;
            try {
                bool isLTS = !obj.get<std::string>("lts").empty();
                if (isLTS) {
                    version = obj.get<std::string>("version");
                    break; // latest LTS is first
                }
            } catch(...) {}
        }
        if (version.empty()) {
            std::cerr << "No LTS found\n";
            return 1;
        }

        std::cout << "Latest LTS = " << version << "\n";

        // 3. Build URL and download archive
        std::string filename = "node-" + version + "-linux-x64.tar.xz";
        std::string target = "/dist/" + version + "/" + filename;
        fs::path archivePath = tmp / filename;

        std::cout << "Downloading: " << target << "\n";
        https_download("nodejs.org", target, archivePath);

        // 4. Extract using system tar (quick integration)
        fs::path extractDir = tmp / "node";
        fs::create_directories(extractDir);

        std::string tarCmd = "tar -xf " + archivePath.string() + " -C " +
                             extractDir.string() + " --strip-components=1";
        if (system(tarCmd.c_str()) != 0) {
            std::cerr << "Extraction failed\n";
            return 1;
        }

        // 5. Path to node
        fs::path nodeBin = extractDir / "bin" / "node";
        if (!fs::exists(nodeBin)) {
            std::cerr << "Node binary not found\n";
            return 1;
        }

        // 6. Exec into node with args
        if (argc < 2) {
            std::cerr << "Usage: " << argv[0] << " <args to node>\n";
            return 1;
        }

        std::vector<char*> newArgs;
        newArgs.push_back(const_cast<char*>(nodeBin.c_str()));
        for (int i = 1; i < argc; i++)
            newArgs.push_back(argv[i]);
        newArgs.push_back(nullptr);

        std::cout << "Running: " << nodeBin << "\n";
        execv(nodeBin.c_str(), newArgs.data());

        perror("execv failed");
        return 1;
    }
    catch(std::exception const& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }
}