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

  // Mixing intent for an edge into a destination port. Replace establishes a
  // new port value at that point in connection order; Add accumulates onto the
  // current value. The first edge always initializes the port buffer.
  enum class Mix {
    Replace,
    Add,
  };

  Mix mix = Mix::Add;
};

}  // namespace sonare::graph
