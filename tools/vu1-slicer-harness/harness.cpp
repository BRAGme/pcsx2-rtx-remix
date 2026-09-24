// Runs the REAL RemixVU1Slice::Analyze -- compiled from pcsx2/GS/Remix, not a port -- on the raw
// images PCSX2_REMIX_UCODEDUMP writes, and prints the same Describe() the backend dumps.
//
// It exists because hand-written ports of the slicer have been wrong in ways that produced false
// conclusions (a 4-bit field where the VF destination is 5-bit). Before trusting a run, check it
// reproduces the header line of the matching remix_ucode_<hash>_pc<pc>.txt exactly.
//
//   build.bat
//   slice.exe <remix_ucode_HASH.bin> <start_pc hex> [<bin> <pc> ...]
//   set SLICEW0=1   -- analyse with open_on_w, as PCSX2_REMIX_SLICEW0 does
#include "GS/Remix/RemixVU1Slice.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc, char** argv)
{
	for (int i = 1; i + 1 < argc; i += 2)
	{
		std::FILE* f = std::fopen(argv[i], "rb");
		if (!f) { std::printf("cannot open %s\n", argv[i]); continue; }
		std::vector<unsigned char> buf(0x4000, 0);
		std::fread(buf.data(), 1, buf.size(), f);
		std::fclose(f);
		const unsigned pc = static_cast<unsigned>(std::strtoul(argv[i + 1], nullptr, 16));
		RemixVU1Slice::Program prog;
		RemixVU1Slice::Analyze(buf.data(), pc, prog, std::getenv("SLICEW0") != nullptr);
		std::printf("### %s\n%s\n", argv[i], RemixVU1Slice::Describe(buf.data(), pc, prog).c_str());
	}
	return 0;
}
