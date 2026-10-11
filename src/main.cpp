#include <cstdlib>
#include <iostream>
#include <sstream>

#include "minild.h"

using namespace minild;

static const char* kUsage =
    "minild - a small static linker and object-file analyzer for Windows x64 (COFF -> PE)\n"
    "\n"
    "usage:\n"
    "  minild inspect FILE.o...                 show sections, symbols and relocations\n"
    "  minild resolve FILE.o... [options]       show how every global symbol is resolved\n"
    "  minild link FILE.o... -o OUT.exe [options]\n"
    "\n"
    "options:\n"
    "  -o FILE            output executable (default a.exe)\n"
    "  -e NAME            entry symbol (default start)\n"
    "  --explain          print the calculation for every relocation\n"
    "  --map              print the link map\n"
    "  --import DLL:FUNC  treat FUNC as a function imported from DLL (e.g. kernel32.dll:Beep)\n"
    "  --version          print the version\n";

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << kUsage;
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "--help" || cmd == "-h" || cmd == "help") { std::cout << kUsage; return 0; }
  if (cmd == "--version") { std::cout << "minild 0.1.0\n"; return 0; }
  if (cmd != "inspect" && cmd != "resolve" && cmd != "link") {
    std::cerr << "minild: unknown command '" << cmd << "'\n\n" << kUsage;
    return 2;
  }

  LinkOptions opts;
  std::vector<std::string> inputs;
  for (int i = 2; i < argc; i++) {
    const std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) { std::cerr << "minild: " << a << " needs a value\n"; std::exit(2); }
      return argv[++i];
    };
    if (a == "-o") opts.output = value();
    else if (a == "-e") opts.entry = value();
    else if (a == "--explain") opts.explain = true;
    else if (a == "--map") opts.map = true;
    else if (a == "--import") {
      const std::string v = value();
      const size_t colon = v.find(':');
      if (colon == std::string::npos) { std::cerr << "minild: --import expects DLL:FUNC\n"; return 2; }
      opts.extraImports.push_back({v.substr(0, colon), v.substr(colon + 1)});
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "minild: unknown option '" << a << "'\n";
      return 2;
    } else inputs.push_back(a);
  }
  if (inputs.empty()) { std::cerr << "minild: no input files\n"; return 2; }

  try {
    std::vector<ObjectFile> objs;
    for (auto& path : inputs) objs.push_back(readObject(path));

    if (cmd == "inspect") {
      for (size_t i = 0; i < objs.size(); i++) {
        if (i) std::cout << "\n";
        inspect(objs[i], std::cout);
      }
      return 0;
    }

    Resolution res = resolveSymbols(objs, opts);
    if (cmd == "resolve") {
      printResolution(objs, res, std::cout);
      for (auto& e : res.errors) std::cout << "  error: " << e << "\n";
      return res.errors.empty() ? 0 : 1;
    }

    if (!res.errors.empty()) {
      for (auto& e : res.errors) std::cerr << "minild: error: " << e << "\n";
      std::cerr << "minild: link failed (" << res.errors.size() << " error" << (res.errors.size() > 1 ? "s" : "") << ")\n";
      return 1;
    }
    Image img = layout(objs, res, opts);
    applyRelocations(objs, res, img, opts.explain, std::cout);
    writePE(img, opts.output);
    if (opts.map) printMap(objs, res, img, std::cout);
    std::cerr << "minild: wrote " << opts.output << "\n";
    return 0;
  } catch (const LinkError& e) {
    std::cerr << "minild: error: " << e.what() << "\n";
    return 1;
  }
}
