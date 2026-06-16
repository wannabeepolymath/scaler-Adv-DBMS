#include <iostream>
#include <fstream>
#include <string>

// Lab 1: minimal file reader used to trace the kernel I/O path.
//   Build : g++ -std=c++17 -o reader reader.cpp
//   Linux : strace -e trace=openat,read,close,fstat,mmap ./reader
//   macOS : sudo dtruss -t open,read,close,fstat ./reader
int main() {
    std::ifstream file("test.txt");
    if (!file.is_open()) {
        std::cerr << "Failed to open file\n";
        return 1;
    }
    std::string line;
    while (std::getline(file, line)) {
        std::cout << line << "\n";
    }
    return 0;
}
