/// @file calib.cpp
/// @brief Overhead calibration targets. This is the one library file compiled WITH -finstrument-functions
///        (see CMakeLists.txt), so calling these functions measures exactly what the hooks cost.
namespace ht {

/// Empty instrumented function: its span is a hop's own hook cost.
__attribute__((noinline)) void calib_leaf() {
  asm volatile("");
}

/// Calls calib_leaf(): the extra span is what one child hop adds to its parent.
__attribute__((noinline)) void calib_parent() {
  calib_leaf();
}

}  // namespace ht
