// Phase 0 executable skeleton. No recording/control functionality is implemented.
#include <iostream>
#include <string>

int main(
    int argc,
    char** argv
) {
    if (argc == 2 && std::string(argv[1]) == "--phase0-info") {
        std::cout << "{\"phase\":0,\"functional_recorder\":false,\"cpp_standard\":17}\n";
        return 0;
    }
    std::cerr << "Phase 0 skeleton only. Use --phase0-info or run the capability probes.\n";
    return 2;
}
