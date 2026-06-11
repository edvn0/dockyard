// Intentionally exports no create_game symbol.
// Used to test that GameDll::load / force_reload handle missing symbols.
extern "C" int dll_no_symbol_present() { return 0; }
