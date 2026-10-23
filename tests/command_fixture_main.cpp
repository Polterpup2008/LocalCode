#include <iostream>

// A harmless native program that can be copied under arbitrary names/paths.
// Its output and status are independent of host tools, privileges and network.
int main(int argc, char** argv) {
    if (argc == 1) {
        std::cout << "localcode-command-fixture\n";
        return 0;
    }
    for (int index = 1; index < argc; ++index)
        std::cout << argv[index] << '\n';
    return 1;
}
