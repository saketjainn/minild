#!/bin/sh
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O2 -Wall -Wextra -o minild src/*.cpp
echo "Built ./minild"
