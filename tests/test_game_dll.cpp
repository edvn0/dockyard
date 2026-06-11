#include <dockyard/game_dll.hpp>
#include <dockyard/vfs.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <fstream>

namespace {

// Ensure the VFS singleton is ready. Called once per process.
void init_vfs_once() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;

    auto& vfs = dy::VFS::get();
    if (!vfs.initialised()) {
        vfs.initialize(std::filesystem::temp_directory_path());
    }
}

// RAII temp directory with an isolated VFS mount.
struct TempDllDir {
    std::filesystem::path dir;
    std::string           scheme;

    explicit TempDllDir(std::string_view test_id) {
        init_vfs_once();

        dir    = std::filesystem::temp_directory_path() / ("dy_test_" + std::string{test_id});
        scheme = "dy_test_" + std::string{test_id};

        std::filesystem::create_directories(dir);
        dy::VFS::get().mount(scheme, dir);
    }

    ~TempDllDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // Copy a DLL from the build output to this temp dir and return its VFS path.
    [[nodiscard]] auto stage(const std::filesystem::path& src,
                             const std::string& dest_name) const -> dy::VFSPath {
        std::error_code ec;
        std::filesystem::copy_file(
            src, dir / dest_name,
            std::filesystem::copy_options::overwrite_existing,
            ec);
        return dy::VFSPath::create("{}://{}", scheme, dest_name);
    }

    // Overwrite the named file with the contents of src.
    void overwrite(const std::filesystem::path& src,
                   const std::string& dest_name) const {
        std::error_code ec;
        std::filesystem::copy_file(
            src, dir / dest_name,
            std::filesystem::copy_options::overwrite_existing,
            ec);
    }

    // Remove the named file so subsequent operations see it as missing.
    void remove(const std::string& name) const {
        std::error_code ec;
        std::filesystem::remove(dir / name, ec);
    }
};

// Paths injected by CMake at compile time (see CMakeLists.txt).
const std::filesystem::path k_dll_valid       {TEST_DLL_DIR "/" TEST_DLL_VALID_NAME};
const std::filesystem::path k_dll_no_symbol   {TEST_DLL_DIR "/" TEST_DLL_NO_SYMBOL_NAME};
const std::filesystem::path k_dll_null_factory{TEST_DLL_DIR "/" TEST_DLL_NULL_FACTORY_NAME};

constexpr auto k_dll_name = "mygame" TEST_DLL_SUFFIX;

} // namespace

TEST_SUITE("GameDll") {

TEST_CASE("Given_NonexistentSourceFile_When_Load_Then_ReturnsError") {
    TempDllDir tmp{"load_nonexistent"};

    // Stage nothing — the VFS path resolves to a file that does not exist.
    auto result = dy::GameDll::load(dy::VFSPath::create("{}://missing{}", tmp.scheme, TEST_DLL_SUFFIX));

    CHECK_FALSE(result.has_value());
    CHECK_FALSE(result.error().empty());
}

TEST_CASE("Given_DllWithNoSymbol_When_Load_Then_ReturnsError") {
    TempDllDir tmp{"load_no_symbol"};
    auto path = tmp.stage(k_dll_no_symbol, k_dll_name);

    auto result = dy::GameDll::load(path);

    CHECK_FALSE(result.has_value());
    CHECK(result.error().find("create_game") != std::string::npos);
}

TEST_CASE("Given_DllWithNullFactory_When_Load_Then_ReturnsError") {
    TempDllDir tmp{"load_null_factory"};
    auto path = tmp.stage(k_dll_null_factory, k_dll_name);

    auto result = dy::GameDll::load(path);

    CHECK_FALSE(result.has_value());
    CHECK(result.error().find("nullptr") != std::string::npos);
}

TEST_CASE("Given_ValidDll_When_Load_Then_ReturnsGameDllWithNonNullGame") {
    TempDllDir tmp{"load_valid"};
    auto path = tmp.stage(k_dll_valid, k_dll_name);

    auto result = dy::GameDll::load(path);

    REQUIRE(result.has_value());
    CHECK(result.value() != nullptr);
    CHECK(result.value()->game() != nullptr);
}

TEST_CASE("Given_ValidDll_When_Load_Then_HotCopyExistsOnDisk") {
    TempDllDir tmp{"load_hotcopy"};
    auto path = tmp.stage(k_dll_valid, k_dll_name);

    auto result = dy::GameDll::load(path);
    REQUIRE(result.has_value());

    // Generation 0 hot copy must exist alongside the source.
    auto hot = tmp.dir / ("mygame_hot_0" TEST_DLL_SUFFIX);
    CHECK(std::filesystem::exists(hot));
}

TEST_CASE("Given_LoadedDll_When_Destroyed_Then_HotCopyIsRemoved") {
    TempDllDir tmp{"load_destructor"};
    auto path = tmp.stage(k_dll_valid, k_dll_name);

    auto hot = tmp.dir / ("mygame_hot_0" TEST_DLL_SUFFIX);

    {
        auto result = dy::GameDll::load(path);
        REQUIRE(result.has_value());
        CHECK(std::filesystem::exists(hot));
    } // destructor runs here

    CHECK_FALSE(std::filesystem::exists(hot));
}

TEST_CASE("Given_LoadedDllWithNoPendingReload_When_PollReload_Then_ReturnsFalse") {
    TempDllDir tmp{"poll_no_pending"};
    auto path = tmp.stage(k_dll_valid, k_dll_name);

    auto result = dy::GameDll::load(path);
    REQUIRE(result.has_value());

    CHECK_FALSE(result.value()->poll_reload());
}

TEST_CASE("Given_LoadedDll_When_ForceReload_Then_ReturnsTrue") {
    TempDllDir tmp{"reload_happy"};
    auto path = tmp.stage(k_dll_valid, k_dll_name);

    auto result = dy::GameDll::load(path);
    REQUIRE(result.has_value());
    auto& dll = *result.value();

    CHECK(dll.force_reload());
}

TEST_CASE("Given_LoadedDll_When_ForceReload_Then_GameInstanceIsNonNull") {
    TempDllDir tmp{"reload_game_ptr"};
    auto path = tmp.stage(k_dll_valid, k_dll_name);

    auto result = dy::GameDll::load(path);
    REQUIRE(result.has_value());
    auto& dll = *result.value();

    REQUIRE(dll.force_reload());
    CHECK(dll.game() != nullptr);
}

TEST_CASE("Given_LoadedDll_When_ForceReload_Then_NewHotCopyExists_And_OldIsRemoved") {
    TempDllDir tmp{"reload_hotcopy_rotation"};
    auto path = tmp.stage(k_dll_valid, k_dll_name);

    auto result = dy::GameDll::load(path);
    REQUIRE(result.has_value());

    auto hot0 = tmp.dir / ("mygame_hot_0" TEST_DLL_SUFFIX);
    auto hot1 = tmp.dir / ("mygame_hot_1" TEST_DLL_SUFFIX);

    REQUIRE(std::filesystem::exists(hot0));

    REQUIRE(result.value()->force_reload());

    CHECK_FALSE(std::filesystem::exists(hot0));
    CHECK(std::filesystem::exists(hot1));
}

TEST_CASE("Given_SourceFileDeleted_When_ForceReload_Then_ReturnsFalse_And_GamePreserved") {
    TempDllDir tmp{"reload_src_deleted"};
    auto path = tmp.stage(k_dll_valid, k_dll_name);

    auto result = dy::GameDll::load(path);
    REQUIRE(result.has_value());
    auto& dll = *result.value();

    auto* game_before = dll.game();
    tmp.remove(k_dll_name);

    CHECK_FALSE(dll.force_reload());
    CHECK(dll.game() == game_before);
}

TEST_CASE("Given_SourceReplacedWithNoSymbolDll_When_ForceReload_Then_ReturnsFalse_And_GamePreserved") {
    TempDllDir tmp{"reload_no_symbol"};
    auto path = tmp.stage(k_dll_valid, k_dll_name);

    auto result = dy::GameDll::load(path);
    REQUIRE(result.has_value());
    auto& dll = *result.value();

    auto* game_before = dll.game();
    tmp.overwrite(k_dll_no_symbol, k_dll_name);

    CHECK_FALSE(dll.force_reload());
    CHECK(dll.game() == game_before);
}

TEST_CASE("Given_SourceReplacedWithNullFactoryDll_When_ForceReload_Then_ReturnsFalse_And_GamePreserved") {
    TempDllDir tmp{"reload_null_factory"};
    auto path = tmp.stage(k_dll_valid, k_dll_name);

    auto result = dy::GameDll::load(path);
    REQUIRE(result.has_value());
    auto& dll = *result.value();

    auto* game_before = dll.game();
    tmp.overwrite(k_dll_null_factory, k_dll_name);

    CHECK_FALSE(dll.force_reload());
    CHECK(dll.game() == game_before);
}

TEST_CASE("Given_SourceReplacedWithGarbage_When_ForceReload_Then_ReturnsFalse_And_GamePreserved") {
    TempDllDir tmp{"reload_garbage"};
    auto path = tmp.stage(k_dll_valid, k_dll_name);

    auto result = dy::GameDll::load(path);
    REQUIRE(result.has_value());
    auto& dll = *result.value();

    auto* game_before = dll.game();

    // Overwrite source with plaintext that is not a valid DLL.
    {
        std::ofstream f{tmp.dir / k_dll_name, std::ios::binary | std::ios::trunc};
        f << "this is not a dll";
    }

    CHECK_FALSE(dll.force_reload());
    CHECK(dll.game() == game_before);
}

} // TEST_SUITE("GameDll")
