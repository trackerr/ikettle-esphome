#include "modem.h"

#include <cstring>
#include <utility>

namespace esphome {
namespace ikettle_mcu {
namespace {

bool starts_with(const std::string &text, const std::string &prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool hex_digit(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

// All deadlines are at most six seconds in the future. Use unsigned arithmetic
// (no implementation-defined unsigned-to-signed conversion across rollover).
// The owner must call tick/feed more often than once per 2^31 milliseconds.
bool before_deadline(uint32_t now, uint32_t deadline) {
  const uint32_t ahead = deadline - now;
  return ahead != 0 && ahead < 0x80000000U;
}

}  // namespace

VirtualModem::VirtualModem(ModemCallbacks callbacks) : callbacks_(std::move(callbacks)) {
  line_.reserve(MAX_AT_LINE);
  payload_.reserve(MAX_PAYLOAD);
}

void VirtualModem::set_mac_address(std::string address) {
  bool valid = address.size() == 17;
  for (size_t index = 0; valid && index < address.size(); ++index) {
    if (index % 3 == 2) {
      valid = address[index] == ':';
    } else {
      valid = hex_digit(address[index]);
      if (address[index] >= 'a' && address[index] <= 'f')
        address[index] = static_cast<char>(address[index] - 'a' + 'A');
    }
  }
  if (!valid) {
    ++counters_.invalid_mac_addresses;
    mac_address_.clear();
    return;
  }
  mac_address_ = std::move(address);
}

void VirtualModem::emit_(const Bytes &bytes) {
  if (callbacks_.uart_tx) {
    callbacks_.uart_tx(bytes);
    counters_.tx_bytes += bytes.size();
    if (callbacks_.trace)
      callbacks_.trace(TraceDirection::ESP_TO_MCU, TraceKind::UART_OUTPUT, bytes.data(), bytes.size());
  }
}

void VirtualModem::trace_notice_(const char *code) {
  if (callbacks_.trace)
    callbacks_.trace(TraceDirection::MCU_TO_ESP, TraceKind::NOTICE,
                     reinterpret_cast<const uint8_t *>(code), std::strlen(code));
}

void VirtualModem::text_(const std::string &text) {
  emit_(Bytes(text.begin(), text.end()));
}

void VirtualModem::ok_() { text_("\r\nOK\r\n"); }
void VirtualModem::error_() { text_("\r\nERROR\r\n"); }

void VirtualModem::reset_handshake_() {
  mux_seen_ = false;
  server_seen_ = false;
  udp_seen_ = false;
  rf_seen_ = false;
  warm_resume_hint_ = false;
  link_announced_ = false;
}

void VirtualModem::pause_(uint32_t now, uint32_t duration) {
  // Short commands must never shorten an existing scan/bootstrap quarantine.
  if (!paused_(now) || duration > static_cast<uint32_t>(pause_until_ - now))
    pause_until_ = now + duration;
  pause_active_ = true;
}

bool VirtualModem::paused_(uint32_t now) const {
  return pause_active_ && before_deadline(now, pause_until_);
}

void VirtualModem::reset_state_(uint32_t now) {
  begun_ = true;
  poisoned_ = false;
  line_.clear();
  line_cr_ = false;
  discard_line_ = false;
  payload_.clear();
  payload_pending_ = false;
  payload_expected_ = 0;
  payload_link_ = 1;
  reset_handshake_();
  pause_active_ = false;
  pause_(now, QUARANTINE_MS);
  ++counters_.resets;
  if (callbacks_.reset)
    callbacks_.reset();
  text_("\r\nready\r\n");
}

void VirtualModem::begin(uint32_t now) { reset_state_(now); }

void VirtualModem::poison_() {
  // A timed-out/ambiguous binary body's remaining bytes must NEVER become AT.
  // Ignore the stream until the owner explicitly calls begin() after reset.
  poisoned_ = true;
  payload_pending_ = false;
  payload_expected_ = 0;
  payload_.clear();
  line_.clear();
  line_cr_ = false;
  discard_line_ = false;
  reset_handshake_();
  if (callbacks_.reset)
    callbacks_.reset();
}

void VirtualModem::expire_payload_(uint32_t now) {
  if (payload_pending_ && static_cast<uint32_t>(now - payload_started_at_) >= PAYLOAD_TIMEOUT_MS) {
    ++counters_.payload_timeouts;
    ++counters_.payloads_rejected;
    trace_notice_("payload_timeout");
    error_();
    poison_();
  }
}

bool VirtualModem::unsigned_number_(const std::string &text, size_t limit, size_t &value) {
  if (text.empty())
    return false;
  value = 0;
  for (char digit : text) {
    if (digit < '0' || digit > '9')
      return false;
    const size_t next = static_cast<size_t>(digit - '0');
    if (next > limit || value > (limit - next) / 10)
      return false;
    value = value * 10 + next;
  }
  return true;
}

bool VirtualModem::quoted_(const std::string &text, size_t &position, size_t minimum,
                          size_t maximum, size_t &length) {
  if (position >= text.size() || text[position] != '"')
    return false;
  ++position;
  length = 0;
  while (position < text.size() && text[position] != '"') {
    const auto byte = static_cast<uint8_t>(text[position]);
    if (byte < 0x20 || byte == 0x7F || length >= maximum)
      return false;
    // No escaping is invented: F19 concatenates quoted strings directly.
    ++length;
    ++position;
  }
  if (position >= text.size() || length < minimum)
    return false;
  ++position;
  return true;
}

bool VirtualModem::joined_credentials_(const std::string &command) {
  size_t position = std::string("AT+CWJAP_DEF=").size();
  size_t length = 0;
  if (!quoted_(command, position, 1, 31, length) || position >= command.size() || command[position++] != ',')
    return false;
  return quoted_(command, position, 0, 31, length) && position == command.size();
}

bool VirtualModem::ap_configuration_(const std::string &command) {
  size_t position = std::string("AT+CWSAP_DEF=").size();
  size_t length = 0;
  if (!quoted_(command, position, 1, 31, length) || position >= command.size() || command[position++] != ',')
    return false;
  if (!quoted_(command, position, 0, 0, length))
    return false;
  return command.compare(position, std::string::npos, ",6,0") == 0;
}

void VirtualModem::finish_payload_() {
  payload_pending_ = false;
  if (callbacks_.trace)
    callbacks_.trace(TraceDirection::MCU_TO_ESP, TraceKind::PAYLOAD, payload_.data(), payload_.size());
  // Only slot 1 has a native local peer. The wrapper must treat decoder
  // rejection separately: SEND OK acknowledges transport, not frame semantics.
  const bool accepted = callbacks_.payload && callbacks_.payload(payload_link_, payload_);
  payload_expected_ = 0;
  payload_.clear();
  if (accepted) {
    ++counters_.payloads_accepted;
    text_("\r\nSEND OK\r\n");
  } else {
    ++counters_.payloads_rejected;
    error_();
  }
  // No quarantine extension here: F19 emits status through CIPSEND about every
  // two seconds. Extending the six-second guard would starve native commands.
}

void VirtualModem::process_send_(const std::string &command, uint32_t now) {
  const size_t prefix = std::string("AT+CIPSEND=").size();
  size_t length = 0;
  const bool valid = command.size() > prefix + 2 && command[prefix] >= '0' && command[prefix] <= '4' &&
                     command[prefix + 1] == ',' && unsigned_number_(command.substr(prefix + 2), MAX_PAYLOAD, length);
  if (!valid) {
    ++counters_.malformed_commands;
    ++counters_.payloads_rejected;
    trace_notice_("invalid_send");
    error_();
    // A following binary body has no trustworthy boundary, especially when
    // the announced length exceeded our limit. Do not parse it as AT text.
    poison_();
    return;
  }
  const auto link = static_cast<uint8_t>(command[prefix] - '0');
  if (link != 1 || (!link_ready() && length == 0)) {
    // Slots 0/2/3/4 have no external UDP/TCP peer. In particular no TCP
    // application session is ever assigned to reserved UDP slot zero.
    ++counters_.payloads_rejected;
    error_();
    return;
  }
  if (!link_ready() && !warm_resume_hint_) {
    // ESPHome may reboot independently while the unchanged MCU is heating or
    // holding warm and still uses its former slot 1. Repeated ERROR would
    // exhaust that slot, while MCU reinitialization can wait until idle.
    // A bounded, nonempty send is a LOCAL transport-resume hint, not evidence
    // that MUX/server/UDP/RF initialization occurred or that a start is safe.
    // The owner must establish a new identity and fresh status on link rise.
    warm_resume_hint_ = true;
    ++counters_.warm_resumes;
    pause_(now, QUARANTINE_MS);
  }
  payload_link_ = link;
  payload_expected_ = length;
  payload_started_at_ = now;
  payload_.clear();
  payload_pending_ = true;
  text_(">");
  if (length == 0)
    finish_payload_();
}

void VirtualModem::process_line_(uint32_t now) {
  // Move the bounded line out of parser state before synchronous callbacks.
  // No raw command, SSID or password is retained in diagnostics or errors.
  const std::string command = std::move(line_);
  line_.clear();
  line_.reserve(MAX_AT_LINE);
  ++counters_.at_commands;
  if (callbacks_.trace)
    callbacks_.trace(TraceDirection::MCU_TO_ESP, TraceKind::AT_COMMAND,
                     reinterpret_cast<const uint8_t *>(command.data()), command.size());
  if (starts_with(command, "AT+CIPSEND=")) {
    process_send_(command, now);
    return;
  }
  pause_(now, AT_SETTLE_MS);
  if (command == "AT") {
    ok_();
  } else if (command == "AT+RST" || command == "AT+RESTORE") {
    // LOCAL parser/session reset only. Never reset ESPHome or erase its Wi-Fi.
    reset_state_(now);
  } else if (command == "AT+CWMODE_DEF=1" || command == "AT+CWMODE_DEF=3") {
    ok_();
  } else if (command == "AT+CWJAP_DEF?") {
    // The virtual modem owns no radio association; do not report ESPHome's
    // station credentials as MCU-managed state.
    text_("\r\nNo AP\r\n\r\nOK\r\n");
  } else if (starts_with(command, "AT+CWJAP_DEF=")) {
    if (joined_credentials_(command)) {
      // F19's two-pattern join matcher requires this double CRLF. This ACK
      // accepts the local compatibility operation, NOT a real station join.
      text_("\r\n\r\nOK\r\n");
    } else {
      ++counters_.malformed_commands;
      error_();
    }
  } else if (command == "AT+CWQAP") {
    ok_();
  } else if (command == "AT+CIPAPMAC_CUR?") {
    if (mac_address_.empty()) {
      error_();
    } else {
      text_("\r\n+CIPAPMAC_CUR:\"" + mac_address_ + "\"\r\n\r\nOK\r\n");
    }
  } else if (starts_with(command, "AT+CWSAP_DEF=")) {
    if (ap_configuration_(command)) {
      ok_();
    } else {
      ++counters_.malformed_commands;
      error_();
    }
  } else if (command == "AT+CIPMUX=1") {
    if (link_announced_) {
      text_("\r\n1,CLOSED\r\n");
      if (callbacks_.reset)
        callbacks_.reset();
    }
    reset_handshake_();
    mux_seen_ = true;
    pause_(now, QUARANTINE_MS);
    ok_();
  } else if (command == "AT+CIPSERVER=1,2081") {
    if (mux_seen_) {
      server_seen_ = true;
      udp_seen_ = false;
      rf_seen_ = false;
      ok_();
    } else {
      error_();
    }
  } else if (command == "AT+CIPSTO=180") {
    ok_();
  } else if (command == "AT+CIPSTART=0,\"UDP\",\"0\",0,2081,2") {
    if (mux_seen_ && server_seen_) {
      udp_seen_ = true;
      rf_seen_ = false;
      ok_();
    } else {
      error_();
    }
  } else if (starts_with(command, "AT+RFPOWER=")) {
    size_t value = 0;
    if (unsigned_number_(command.substr(std::string("AT+RFPOWER=").size()), 82, value)) {
      if (mux_seen_ && server_seen_ && udp_seen_ && !rf_seen_) {
        rf_seen_ = true;
        pause_(now, QUARANTINE_MS);
      }
      ok_();
    } else {
      ++counters_.malformed_commands;
      error_();
    }
  } else if (command == "AT+GMR") {
    text_("\r\niKettle virtual modem 0.1.2\r\nESPHome native Wi-Fi; local MCU link only\r\n\r\nOK\r\n");
  } else if (command == "AT+CWLAP") {
    // No invented AP records. Give the MCU scan formatter and its later UART
    // drain four seconds plus margin before another application injection.
    pause_(now, QUARANTINE_MS);
    ok_();
  } else if (starts_with(command, "AT+CWLAP=")) {
    size_t position = std::string("AT+CWLAP=").size();
    size_t length = 0;
    if (quoted_(command, position, 1, 31, length) && position == command.size()) {
      pause_(now, QUARANTINE_MS);
      ok_();
    } else {
      ++counters_.malformed_commands;
      error_();
    }
  } else {
    ++counters_.unsupported_commands;
    error_();
  }
}

void VirtualModem::feed(const uint8_t *data, size_t size, uint32_t now) {
  expire_payload_(now);
  if (data == nullptr || size == 0)
    return;
  counters_.rx_bytes += size;
  if (!begun_ || poisoned_) {
    counters_.poisoned_bytes_dropped += size;
    return;
  }
  for (size_t index = 0; index < size; ++index) {
    if (poisoned_) {
      counters_.poisoned_bytes_dropped += size - index;
      break;
    }
    const uint8_t byte = data[index];
    if (payload_pending_) {
      // Length was validated before the prompt; the buffer can never exceed
      // MAX_PAYLOAD. Every byte, including AT/CR/LF/7E/7D/zero, is opaque here.
      payload_.push_back(byte);
      if (payload_.size() == payload_expected_)
        finish_payload_();
      continue;
    }
    if (byte == '\n') {
      if (discard_line_ || !line_cr_) {
        ++counters_.malformed_commands;
        trace_notice_("malformed_line");
        error_();
        pause_(now, AT_SETTLE_MS);
        line_.clear();
      } else if (!line_.empty()) {
        // Clear the terminator before process_line_ changes parser modes.
        line_cr_ = false;
        process_line_(now);
      }
      line_cr_ = false;
      discard_line_ = false;
      continue;
    }
    if (discard_line_)
      continue;
    if (byte == '\r') {
      if (line_cr_)
        discard_line_ = true;
      line_cr_ = true;
      continue;
    }
    if (line_cr_ || byte < 0x20 || byte == 0x7F) {
      discard_line_ = true;
      continue;
    }
    if (line_.size() >= MAX_AT_LINE) {
      ++counters_.line_overflows;
      trace_notice_("line_overflow");
      error_();
      // A truncated header may have been CIPSEND. Its trailing bytes cannot
      // safely be recovered merely by finding the next CRLF in binary data.
      poison_();
      continue;
    }
    line_.push_back(static_cast<char>(byte));
  }
  // Announce only AFTER processing incoming AT activity at this timestamp.
  tick(now);
}

void VirtualModem::tick(uint32_t now) {
  expire_payload_(now);
  if (pause_active_ && !paused_(now))
    pause_active_ = false;
  if (!begun_ || poisoned_ || payload_pending_ || discard_line_ || line_cr_ || !line_.empty() || paused_(now))
    return;
  const bool bootstrap_seen = mux_seen_ && server_seen_ && udp_seen_ && rf_seen_;
  if (!link_announced_ && (bootstrap_seen || warm_resume_hint_) && callbacks_.uart_tx) {
    link_announced_ = true;
    ++counters_.connects;
    text_("\r\n1,CONNECT\r\n");
    pause_(now, AT_SETTLE_MS);
  }
}

bool VirtualModem::link_ready() const {
  const bool bootstrap_seen = mux_seen_ && server_seen_ && udp_seen_ && rf_seen_;
  return begun_ && link_announced_ && (bootstrap_seen || warm_resume_hint_) && !poisoned_;
}

bool VirtualModem::can_inject(uint32_t now) const {
  return link_ready() && callbacks_.uart_tx && !payload_pending_ && !discard_line_ && !line_cr_ && line_.empty() &&
         !paused_(now);
}

bool VirtualModem::inject(const Bytes &payload, uint32_t now) {
  if (payload.empty() || payload.size() > MAX_PAYLOAD || !can_inject(now)) {
    ++counters_.injections_rejected;
    return false;
  }
  const std::string header = "+IPD,1," + std::to_string(payload.size()) + ":";
  Bytes frame;
  frame.reserve(header.size() + payload.size());
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), payload.begin(), payload.end());
  emit_(frame);
  ++counters_.injections_sent;
  return true;
}

const ModemCounters &VirtualModem::counters() const { return counters_; }

ModemDiagnostics VirtualModem::diagnostics() const {
  ModemDiagnostics result;
  result.counters = counters_;
  result.begun = begun_;
  result.poisoned = poisoned_;
  result.link_ready = link_ready();
  result.mac_configured = !mac_address_.empty();
  result.at_header_pending = !line_.empty() || line_cr_ || discard_line_;
  result.payload_pending = payload_pending_;
  result.mux_seen = mux_seen_;
  result.server_seen = server_seen_;
  result.udp_zero_reserved = udp_seen_;
  result.rf_power_seen = rf_seen_;
  result.warm_resume_hint = warm_resume_hint_;
  result.payload_expected = payload_expected_;
  result.payload_received = payload_.size();
  return result;
}

}  // namespace ikettle_mcu
}  // namespace esphome
