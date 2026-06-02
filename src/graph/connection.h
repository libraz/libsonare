#pragma once

/// @file connection.h
/// @brief Audio routing edge description.

#include <string>

namespace sonare::graph {

struct Connection {
  std::string source_node;
  int source_port = 0;
  std::string dest_node;
  int dest_port = 0;

  // Mixing intent for an edge into a destination port. NOTE: the compiled graph
  // makes the per-port result an order-independent sum — the first edge into a
  // port always overwrites (clearing the buffer) and every later edge adds —
  // regardless of this flag. The flag is retained for API compatibility and to
  // express intent; it no longer makes the result depend on connection order.
  enum class Mix {
    Replace,
    Add,
  };

  Mix mix = Mix::Add;
};

}  // namespace sonare::graph
