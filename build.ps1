$wslPrefix = if ($env:OS -eq "Windows_NT") { "wsl" } else { "" }
& $wslPrefix g++ -std=c++17 run-node.cpp -o run-node -larchive -lssl -lcrypto -lboost_system -lboost_filesystem -lpthread
& $wslPrefix strip run-node