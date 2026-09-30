// scan_main.cpp — thin launcher for the "dataflow scan" route scanner.
//
//   Windows (MinGW):
//     g++ -std=c++17 -Iinclude tools/scan_main.cpp src/scan.cpp src/dataflow.cpp -lwinhttp -lbcrypt -o dataflow-scan
//   Linux:
//     g++ -std=c++17 -Iinclude tools/scan_main.cpp src/scan.cpp src/dataflow.cpp -o dataflow-scan

#include "dataflow_scan.hpp"

int main(int argc, char** argv) {
    return dataflow::scan::scan_main(argc, argv);
}
