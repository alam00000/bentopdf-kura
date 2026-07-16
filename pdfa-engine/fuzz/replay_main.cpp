

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    FILE* f = std::fopen(argv[i], "rb");
    if (!f) {
      std::fprintf(stderr, "cannot open %s\n", argv[i]);
      return 1;
    }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(n > 0 ? static_cast<size_t>(n) : 0);
    if (!buf.empty() && std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
      std::fclose(f);
      std::fprintf(stderr, "short read %s\n", argv[i]);
      return 1;
    }
    std::fclose(f);
    LLVMFuzzerTestOneInput(buf.data(), buf.size());
    std::fprintf(stderr, "ok  %s (%zu bytes)\n", argv[i], buf.size());
  }
  return 0;
}
