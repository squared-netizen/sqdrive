-- build.lua: sqdrive's own build, for sqmake: the Virtual Cartridge Drive
-- as a C++23 module (sq.drive, libsqdrive.a) and a command-line tool.
--
--   sqmake          build/libsqdrive.a, build/sqdrive and the tests' programs
--   sqmake test     the tests
--   sqmake --debug  -O0 (build/debug)
--
-- Included by a project that vendors it (square), set sqdrive_settings =
-- {root = "third_party/sqdrive", cart = <sq.cart's libraries>} first: it
-- returns {drive = the library} and builds nothing else.
--
-- The compiler is $CXX (and $CC): clang and libc++ 23 with `import std;`.
-- Offline: sq.cart and what it builds with are vendored in
-- third_party/sqcart (tools/vendor_sqcart.sh), built by its own build.lua.

local s = sqdrive_settings or {}
local top = s.root == nil
local function here(path) return top and path or s.root .. "/" .. path end

if top then
  local opt = mode.debug and {"-O0", "-g"} or {"-O2", "-g"}
  toolchain.cxxflags = {"-std=c++23", "-stdlib=libc++", opt}
  toolchain.cflags = {opt}
  toolchain.ldflags = {"-stdlib=libc++", "-pthread"}  -- -pthread: Android has no libpthread
end

local warn = {"-Wall", "-Wextra", "-Wpedantic", "-Wconversion", "-Wno-sign-conversion",
              "-Wno-missing-designated-field-initializers"}

local cart = s.cart
if not cart then
  sqcart_settings = {root = here("third_party/sqcart/v3")}
  cart = include(here("third_party/sqcart/v3/build.lua"))
end

-- sq.drive reads and writes (sq.cart.write): the writer's libraries.
local drive = library{name = "sqdrive", srcs = {here("src/drive.cppm")}, deps = {cart.write}, flags = warn}

if top then
  exe{name = "sqdrive", srcs = {"tools/sqdrive.cpp"}, deps = {drive}, flags = warn}
  local tests = {}
  for _, src in ipairs(glob("tests/test_*.cpp")) do
    tests[#tests + 1] = exe{name = src:match("([^/]+)%.cpp$"), srcs = {src}, deps = {drive}, flags = warn}
  end
  verb("test", {needs = tests, call = function()
    local failed = 0
    for _, t in ipairs(tests) do
      if exec{t} ~= 0 then failed = failed + 1 end
    end
    print(failed == 0 and "all tests passed" or "FAILED")
    return failed == 0 and 0 or 1
  end})
end

return {drive = drive, cart = cart}
