#include "midi/sound_destination.h"

#include <algorithm>
#include <utility>

namespace sonare::midi {

SoundDestination SoundDestination::HostInstrument(uint32_t node_id, std::string label) {
  SoundDestination dest;
  dest.kind = DestinationKind::kHostInstrument;
  dest.host_instrument.node_id = node_id;
  dest.host_instrument.label = std::move(label);
  return dest;
}

SoundDestination SoundDestination::ExternalPort(uint32_t port_id, std::string port_name,
                                                std::string device_name, uint8_t group,
                                                bool is_midi2) {
  SoundDestination dest;
  dest.kind = DestinationKind::kExternalPort;
  dest.external_port.port_id = port_id;
  dest.external_port.port_name = std::move(port_name);
  dest.external_port.device_name = std::move(device_name);
  dest.external_port.group = group;
  dest.external_port.is_midi2 = is_midi2;
  return dest;
}

bool DestinationTable::add(uint32_t destination_id, const SoundDestination& destination) {
  if (destination_id == kNullDestinationId) return false;
  table_[destination_id] = destination;
  return true;
}

bool DestinationTable::remove(uint32_t destination_id) {
  if (destination_id == kNullDestinationId) return false;
  return table_.erase(destination_id) != 0;
}

bool DestinationTable::contains(uint32_t destination_id) const noexcept {
  if (destination_id == kNullDestinationId) return true;
  return table_.find(destination_id) != table_.end();
}

SoundDestination DestinationTable::lookup(uint32_t destination_id) const {
  const auto it = table_.find(destination_id);
  if (it == table_.end()) return SoundDestination::Null();
  return it->second;
}

std::vector<uint32_t> DestinationTable::ids() const {
  std::vector<uint32_t> result;
  result.reserve(table_.size());
  for (const auto& kv : table_) result.push_back(kv.first);
  std::sort(result.begin(), result.end());
  return result;
}

}  // namespace sonare::midi
