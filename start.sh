#!/usr/bin/bash

/home/yanniks/Apps/CLion-2024.3/clion-2024.3/bin/cmake/linux/x64/bin/cmake --build /home/yanniks/Dev/asyncio-benchmark/cmake-build-release --target server -j 20
/home/yanniks/Apps/CLion-2024.3/clion-2024.3/bin/cmake/linux/x64/bin/cmake --build /home/yanniks/Dev/asyncio-benchmark/cmake-build-release --target client -j 20

for c in blocking asio; do
  for n in 1 10 100 1000 10000; do
    # Start the server in the background
    ./cmake-build-release/server $n 60 &
    server_pid=$!
    # Give the server a moment to start
    sleep 1
    # Run the client in the foreground with performance metrics
    (echo ''; set -x; perf stat -e cycles,instructions,context-switches,cache-references,cache-misses ./cmake-build-release/client $c $n 60)
    # Wait for the server to finish
    wait $server_pid
  done
done
