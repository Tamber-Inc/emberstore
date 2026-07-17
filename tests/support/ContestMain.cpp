#include "ContestFile.h"

#include <NanoTest/NanoTest.h>

// Stands in for NanoTestMain: runs the tests as usual, except the binary can
// also be asked to re-invoke itself as a real second process contending for a
// file (see ContestFile.h), so lock tests can cross a genuine process boundary.
int main(int argc, char* argv[])
{
    emberstore::testing::rememberSelfExecutable(argv[0]);

    const auto contested = emberstore::testing::runIfContestInvocation(argc, argv);
    if (contested >= 0)
        return contested;

    return nano::run(argc, argv);
}
