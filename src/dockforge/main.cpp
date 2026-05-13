#include <dockforge/dockforge.hpp>

auto main(int argc, char *argv[]) -> dy::i32 {
  std::unique_ptr<dy::App> app = make_app();
  return app->run(dy::i32{argc}, argv);
}