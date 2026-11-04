#include "uart_trace.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace esphome {
namespace ikettle_mcu {
namespace {
void add(uint32_t &counter, size_t amount = 1) {
  const auto maximum = std::numeric_limits<uint32_t>::max();
  counter = amount > maximum - counter ? maximum : counter + static_cast<uint32_t>(amount);
}
bool equal(const uint8_t *data, size_t length, const char *text) {
  const size_t size = std::strlen(text);
  return length == size && (size == 0 || std::memcmp(data, text, size) == 0);
}
bool keep_valid(uint8_t value) { return value == 0 || (value >= 5 && value <= 30); }
bool temperature_valid(uint8_t value) { return value >= 20 && value <= 100; }

// Only one exact frame is permitted. Coalesced/partial/unknown frames never
// receive a hex representation, even if their first opcode looks familiar.
size_t safe_frame(const uint8_t *data, size_t length, bool outbound) {
  if (data == nullptr || length == 0 || length > 7)
    return 0;
  if (outbound) {
    if (length == 2 && data[1] == 0x7e) {
      switch (data[0]) {
        case 0x64: case 0x2b: case 0x2e: case 0x6a: case 0x16: case 0x30: return 2;
        default: return 0;
      }
    }
    if (length == 4 && (data[0] == 0x15 || data[0] == 0x19) && temperature_valid(data[1]) &&
        keep_valid(data[2]) && data[3] == 0x7e)
      return 4;
    return 0;
  }
  switch (data[0]) {
    case 0x14: return length == 7 && data[1] <= 4 && data[5] == 0 && data[6] == 0x7e ? 7 : 0;
    case 0x65: return length == 4 && data[1] == 1 && data[2] == 19 && data[3] == 0x7e ? 4 : 0;
    case 0x03: return length == 3 && data[2] == 0x7e ? 3 : 0;
    case 0x2d: return length == 4 && data[3] == 0x7e ? 4 : 0;
    case 0x2f:
      return length == 6 && temperature_valid(data[1]) && keep_valid(data[2]) &&
        (data[3] == 0 || temperature_valid(data[3])) && data[4] == 0x7e ? 5 : 0;
    default: return 0;
  }
}
std::string frame_metadata(const uint8_t *data, size_t length, bool outbound) {
  std::string result = "payload len=" + std::to_string(length);
  if (data == nullptr || length == 0)
    return result;
  char opcode[12];
  std::snprintf(opcode, sizeof(opcode), " op=%02X", static_cast<unsigned>(data[0]));
  result += opcode;
  const size_t safe = safe_frame(data, length, outbound);
  if (safe == 0)
    return result + " data=redacted";
  result += " hex=";
  constexpr char DIGITS[] = "0123456789ABCDEF";
  for (size_t index = 0; index < safe; ++index) {
    result.push_back(DIGITS[data[index] >> 4]);
    result.push_back(DIGITS[data[index] & 15]);
  }
  if (safe != length)
    result += " tail=redacted";
  return result;
}
std::string at_metadata(const uint8_t *data, size_t length) {
  const std::string metadata = "AT len=" + std::to_string(length);
  if (length == 0 || length > 256 || data == nullptr)
    return metadata + " name=unknown args=redacted";
  // The modem hook omits CRLF; accepting the exact final pair also makes the
  // sanitizer useful for complete fixture lines. Internal controls fail closed.
  size_t end = length;
  if (end >= 2 && data[end-2] == '\r' && data[end-1] == '\n')
    end -= 2;
  for (size_t index = 0; index < end; ++index)
    if (data[index] < 0x20 || data[index] > 0x7e)
      return metadata + " name=unknown args=redacted";
  size_t name_length = 0;
  while (name_length < end && data[name_length] != '=')
    ++name_length;
  constexpr const char *NAMES[] = {"AT", "AT+RESTORE", "AT+CWMODE_DEF", "AT+CWJAP_DEF?", "AT+CWJAP_DEF",
    "AT+CWQAP", "AT+CIPAPMAC_CUR?", "AT+CWSAP_DEF", "AT+RST", "AT+CIPMUX", "AT+CIPSERVER", "AT+CIPSTO",
    "AT+CIPSTART", "AT+RFPOWER", "AT+CWLAP", "AT+GMR", "AT+CIPSEND"};
  for (const char *name : NAMES) {
    if (equal(data, name_length, name))
      return metadata + " name=" + name + " args=redacted";
  }
  return metadata + " name=unknown args=redacted";
}
}  // namespace

bool UartTrace::configure(uint32_t duration_seconds, uint8_t rate) {
  if (duration_seconds < 10 || duration_seconds > 300 || rate < 1 || rate > 20)
    return false;
  duration_ms_ = duration_seconds * 1000;
  rate_ = rate;
  return true;
}
void UartTrace::clear_() {
  add(counters_.dropped_events, count_);
  for (auto &line : queue_)
    line.fill(0);
  head_ = count_ = 0;
}
void UartTrace::set_enabled(bool enabled, uint32_t now) {
  tick(now);
  if (!enabled) {
    enabled_ = false;
    clear_();
  } else if (!enabled_) {
    clear_();
    enabled_ = true;
    enabled_at_ = rate_window_at_ = now;
    active_duration_ms_ = duration_ms_;
    active_rate_ = rate_;
    rate_used_ = 0;
  }
}
void UartTrace::tick(uint32_t now) {
  if (enabled_ && static_cast<uint32_t>(now - enabled_at_) >= active_duration_ms_) {
    enabled_ = false;
    clear_();
  }
}
std::string UartTrace::sanitize_(TraceDirection direction, TraceKind kind, const uint8_t *data, size_t length) {
  if (data == nullptr && length != 0)
    return "invalid borrowed event data=redacted";
  if (kind == TraceKind::AT_COMMAND)
    return at_metadata(data, length);
  if (kind == TraceKind::PAYLOAD)
    return frame_metadata(data, length, direction == TraceDirection::ESP_TO_MCU);
  if (kind == TraceKind::NOTICE) {
    for (const char *notice : {"payload_timeout", "line_overflow", "malformed_line", "invalid_send"})
      if (equal(data, length, notice))
        return std::string("notice=") + notice;
    return "notice=redacted";
  }
  if (kind != TraceKind::UART_OUTPUT || direction != TraceDirection::ESP_TO_MCU)
    return "event len=" + std::to_string(length) + " data=redacted";
  // Parse ONLY exact local-peer +IPD envelopes; reject inconsistent length,
  // alternate slots, extra headers/trailers and decimal overflow.
  constexpr char PREFIX[] = "+IPD,1,";
  if (length > sizeof(PREFIX)-1 && std::memcmp(data, PREFIX, sizeof(PREFIX)-1) == 0) {
    size_t position = sizeof(PREFIX)-1, expected = 0, digits = 0;
    while (position < length && data[position] >= '0' && data[position] <= '9' && digits < 4) {
      expected = expected * 10 + static_cast<size_t>(data[position++] - '0');
      ++digits;
    }
    if (digits && expected > 0 && expected <= 512 && position < length && data[position] == ':' &&
        length - position - 1 == expected)
      return "IPD link=1 " + frame_metadata(data + position + 1, expected, true);
    return "UART len=" + std::to_string(length) + " envelope=invalid data=redacted";
  }
  struct Fixed { const char *wire; const char *label; };
  constexpr Fixed FIXED[] = {{"\r\nOK\r\n", "OK"}, {"\r\nERROR\r\n", "ERROR"},
    {"\r\nready\r\n", "ready"}, {">", "prompt"}, {"\r\nSEND OK\r\n", "SEND_OK"},
    {"\r\n1,CONNECT\r\n", "CONNECT_1"}, {"\r\n1,CLOSED\r\n", "CLOSED_1"},
    {"\r\n\r\nOK\r\n", "OK"}, {"\r\nNo AP\r\n\r\nOK\r\n", "NO_AP_OK"}};
  for (const auto &fixed : FIXED)
    if (equal(data, length, fixed.wire))
      return std::string("UART ") + fixed.label;
  return "UART len=" + std::to_string(length) + " text=redacted";
}
void UartTrace::record(TraceDirection direction, TraceKind kind, const uint8_t *data, size_t length, uint32_t now) {
  tick(now);
  if (!enabled_)
    return;
  add(counters_.seen_events);
  if (direction == TraceDirection::MCU_TO_ESP) {
    add(counters_.rx_events); add(counters_.rx_bytes, length);
  } else {
    add(counters_.tx_events); add(counters_.tx_bytes, length);
  }
  if (static_cast<uint32_t>(now - rate_window_at_) >= 1000) {
    rate_window_at_ = now;
    rate_used_ = 0;
  }
  if (rate_used_ >= active_rate_ || count_ == QUEUE_CAPACITY) {
    add(counters_.dropped_events);
    return;
  }
  ++rate_used_;
  auto &line = queue_[(head_ + count_) % QUEUE_CAPACITY];
  const auto sanitized = sanitize_(direction, kind, data, length);
  std::snprintf(line.data(), line.size(), "t=%lu %s %s", static_cast<unsigned long>(now),
                direction == TraceDirection::MCU_TO_ESP ? "RX" : "TX", sanitized.c_str());
  ++count_;
}
size_t UartTrace::drain(uint32_t now, const Sink &sink) {
  tick(now);
  if (!enabled_ || !sink)
    return 0;
  size_t emitted = 0;
  while (count_ && emitted < MAX_DRAIN) {
    // A local sanitized copy keeps the borrowed pointer valid even if the sink
    // turns tracing off, which clears all queued entries immediately.
    const auto line = queue_[head_];
    queue_[head_].fill(0);
    head_ = (head_ + 1) % QUEUE_CAPACITY;
    --count_;
    add(counters_.emitted_events);
    ++emitted;
    sink(line.data());
    if (!enabled_)
      break;
  }
  return emitted;
}
std::string UartTrace::summary() const {
  char result[MAX_LINE + 1];
  std::snprintf(result, sizeof(result), "%s rx=%lu tx=%lu seen=%lu emitted=%lu dropped=%lu queued=%lu",
    enabled_ ? "ON" : "OFF", static_cast<unsigned long>(counters_.rx_events),
    static_cast<unsigned long>(counters_.tx_events), static_cast<unsigned long>(counters_.seen_events),
    static_cast<unsigned long>(counters_.emitted_events), static_cast<unsigned long>(counters_.dropped_events),
    static_cast<unsigned long>(count_));
  return result;
}
}  // namespace ikettle_mcu
}  // namespace esphome
