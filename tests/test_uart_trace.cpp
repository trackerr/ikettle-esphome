#include "modem.h"
#include "uart_trace.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace esphome::ikettle_mcu;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #x); } while (0)

using Lines = std::vector<std::string>;
static const std::string CANARY = "PVT_K93_MQ4_XZ8_SECRET";
static Bytes bytes(const std::string &text) { return Bytes(text.begin(), text.end()); }
static std::string hex(const Bytes &value) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string result;
  for (uint8_t byte : value) { result += digits[byte >> 4]; result += digits[byte & 15]; }
  return result;
}
static bool contains(const Lines &lines, const std::string &value) {
  return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) { return line.find(value) != std::string::npos; });
}
static std::string lower(std::string value) {
  for (char &byte : value) byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
  return value;
}
static std::string hex_digits(std::string text) {
  text = lower(text);
  for (size_t pos = text.find("0x"); pos != std::string::npos; pos = text.find("0x")) text.erase(pos, 2);
  std::string result;
  for (unsigned char byte : text) if (std::isxdigit(byte)) result += static_cast<char>(byte);
  return result;
}
static void private_absent(const Lines &lines, const std::string &secret = CANARY) {
  CHECK(!lines.empty());
  for (const auto &line : lines) {
    CHECK(line.size() <= UartTrace::MAX_LINE);
    CHECK(lower(line).find(lower(secret)) == std::string::npos);
    // Detect compact, spaced, colon/comma-separated and0x-prefixed hex leaks.
    CHECK(hex_digits(line).find(lower(hex(bytes(secret)))) == std::string::npos);
    for (size_t offset = 0; offset + 6 <= secret.size(); ++offset) {
      const auto fragment = secret.substr(offset, 6);
      CHECK(lower(line).find(lower(fragment)) == std::string::npos);
      CHECK(hex_digits(line).find(lower(hex(bytes(fragment)))) == std::string::npos);
    }
    for (unsigned char byte : line) CHECK(byte >= 0x20 && byte <= 0x7E);
  }
}
static void record(UartTrace &trace, TraceDirection direction, TraceKind kind, const Bytes &data, uint32_t now) {
  trace.record(direction, kind, data.data(), data.size(), now);
}
static Lines drain_all(UartTrace &trace, uint32_t now) {
  Lines lines;
  for (size_t iteration = 0; iteration < UartTrace::QUEUE_CAPACITY + 1; ++iteration) {
    const size_t count = trace.drain(now, [&](const char *line) { lines.emplace_back(line); });
    CHECK(count <= UartTrace::MAX_DRAIN);
    if (count == 0) break;
  }
  CHECK(trace.queued() == 0);
  return lines;
}
static Lines direct(TraceDirection direction, TraceKind kind, const Bytes &data) {
  UartTrace trace;
  CHECK(trace.configure(300, 20));
  trace.set_enabled(true, 0);
  record(trace, direction, kind, data, 1);
  return drain_all(trace, 1);
}
static void redacted(const Lines &lines, const std::string &secret = CANARY) {
  private_absent(lines, secret);
  CHECK(!contains(lines, " hex="));
}

struct FakeModem {
  UartTrace trace;
  std::vector<Bytes> writes;
  std::vector<std::pair<uint8_t, Bytes>> payloads;
  std::vector<std::string> writer_order;
  Lines logs;
  uint32_t now{0};
  unsigned resets{0};
  bool accept{true}, in_modem{false}, writer_before_trace{true};
  size_t trace_callbacks{0};
  VirtualModem modem;

  explicit FakeModem(bool enabled = true) : modem(ModemCallbacks{
      [this](const Bytes &data) {
        writes.push_back(data);
        writer_order.push_back("wire:" + hex(data));
      },
      [this](uint8_t link, const Bytes &data) {
        payloads.emplace_back(link, data);
        writer_order.push_back("payload:" + std::to_string(link) + ":" + hex(data));
        return accept;
      },
      [this]() { ++resets; writer_order.emplace_back("reset"); },
      [this](TraceDirection direction, TraceKind kind, const uint8_t *data, size_t length) {
        ++trace_callbacks;
        if (kind == TraceKind::UART_OUTPUT) {
          Bytes observed;
          if (length) observed.assign(data, data + length);
          if (writes.empty() || writes.back() != observed) writer_before_trace = false;
        }
        trace.record(direction, kind, data, length, now);
      }}) {
    CHECK(trace.configure(300, 20));
    if (enabled) trace.set_enabled(true, 0);
    modem.set_mac_address("02:00:00:00:00:42");
    begin(0);
  }
  void collect() {
    CHECK(!in_modem);
    const auto output = drain_all(trace, now);
    logs.insert(logs.end(), output.begin(), output.end());
  }
  void feed(const Bytes &data, uint32_t time, bool bytewise = false) {
    now = time;
    in_modem = true;
    if (bytewise) {
      for (uint8_t byte : data) modem.feed(&byte, 1, now);
    } else modem.feed(data.data(), data.size(), now);
    in_modem = false;
  }
  void command(const std::string &text, uint32_t time, bool bytewise = false) {
    feed(bytes(text + "\r\n"), time, bytewise);
  }
  void tick(uint32_t time) {
    now = time; in_modem = true; modem.tick(time); in_modem = false;
  }
  void begin(uint32_t time) {
    now = time; in_modem = true; modem.begin(time); in_modem = false;
  }
  bool inject(const Bytes &data, uint32_t time) {
    now = time; in_modem = true; const bool accepted = modem.inject(data, time); in_modem = false;
    return accepted;
  }
  void ready() {
    for (const auto &line : {"AT", "AT+CWMODE_DEF=1", "AT+CIPMUX=1", "AT+CIPSERVER=1,2081",
                            "AT+CIPSTO=180", "AT+CIPSTART=0,\"UDP\",\"0\",0,2081,2", "AT+RFPOWER=50"}) {
      command(line, 10); collect();
    }
    tick(6010); collect(); tick(6110); collect();
    CHECK(modem.can_inject(6110));
  }
  void send(const Bytes &data, uint32_t time, bool bytewise = true) {
    command("AT+CIPSEND=1," + std::to_string(data.size()), time, bytewise);
    feed(data, time + 1, bytewise);
  }
};

static void defaults_and_configuration_bounds() {
  UartTrace trace;
  CHECK(!trace.enabled());
  record(trace, TraceDirection::MCU_TO_ESP, TraceKind::AT_COMMAND, bytes("AT=" + CANARY), 0);
  CHECK(trace.queued() == 0 && trace.counters().seen_events == 0);
  CHECK(drain_all(trace, 0).empty());
  CHECK(!trace.configure(9, 8)); CHECK(!trace.configure(301, 8));
  CHECK(!trace.configure(10, 0)); CHECK(!trace.configure(10, 21));
  CHECK(trace.configure(10, 1)); CHECK(!trace.enabled());
  CHECK(trace.configure(300, 20)); CHECK(!trace.enabled());
  CHECK(trace.summary().size() <= UartTrace::MAX_LINE);
}

static void deadline_repeated_on_and_explicit_off() {
  UartTrace trace;
  CHECK(trace.configure(10, 20)); trace.set_enabled(true, 100);
  record(trace, TraceDirection::MCU_TO_ESP, TraceKind::AT_COMMAND, bytes("AT"), 101);
  trace.set_enabled(false, 102);
  CHECK(!trace.enabled() && trace.queued() == 0 && trace.counters().dropped_events == 1);
  CHECK(drain_all(trace, 103).empty());
  trace.set_enabled(true, 200);
  trace.set_enabled(true, 10199);
  CHECK(trace.enabled()); trace.tick(10200); CHECK(!trace.enabled());
  record(trace, TraceDirection::MCU_TO_ESP, TraceKind::AT_COMMAND, bytes("AT"), 10201);
  CHECK(trace.counters().seen_events == 1);
}

static void active_configuration_waits_for_next_window() {
  UartTrace trace;
  CHECK(trace.configure(10, 1)); trace.set_enabled(true, 0);
  CHECK(trace.configure(300, 20));
  for (unsigned i = 0; i < 2; ++i) record(trace, TraceDirection::MCU_TO_ESP, TraceKind::AT_COMMAND, bytes("AT"), 1);
  CHECK(trace.queued() == 1 && trace.counters().dropped_events == 1);
  trace.tick(10000); CHECK(!trace.enabled());
  trace.set_enabled(true, 10001);
  for (unsigned i = 0; i < 2; ++i) record(trace, TraceDirection::MCU_TO_ESP, TraceKind::AT_COMMAND, bytes("AT"), 10002);
  CHECK(trace.queued() == 2);
  trace.tick(310000); CHECK(trace.enabled()); trace.tick(310001); CHECK(!trace.enabled());
}

static void rollover_deadline_and_rate_window() {
  constexpr uint32_t start = 0xFFFFFF00U;
  UartTrace trace;
  CHECK(trace.configure(10, 1)); trace.set_enabled(true, start);
  record(trace, TraceDirection::MCU_TO_ESP, TraceKind::AT_COMMAND, bytes("AT"), start);
  record(trace, TraceDirection::ESP_TO_MCU, TraceKind::UART_OUTPUT, bytes(">"), start + 999U);
  CHECK(trace.queued() == 1 && trace.counters().dropped_events == 1);
  record(trace, TraceDirection::ESP_TO_MCU, TraceKind::UART_OUTPUT, bytes(">"), start + 1000U);
  CHECK(trace.queued() == 2);
  trace.tick(start + 9999U); CHECK(trace.enabled());
  trace.tick(start + 10000U); CHECK(!trace.enabled() && trace.queued() == 0);
  CHECK(trace.counters().seen_events == trace.counters().dropped_events);
}

static void rate_queue_and_drain_are_bounded() {
  UartTrace trace;
  CHECK(trace.configure(300, 20)); trace.set_enabled(true, 0);
  for (unsigned i = 0; i < 100; ++i) record(trace, TraceDirection::MCU_TO_ESP, TraceKind::AT_COMMAND, bytes("AT"), 1);
  CHECK(trace.queued() == UartTrace::QUEUE_CAPACITY);
  CHECK(trace.counters().seen_events == 100 && trace.counters().dropped_events == 92);
  Lines lines;
  CHECK(trace.drain(1, [&](const char *line) { lines.emplace_back(line); }) == 2);
  CHECK(lines.size() == 2 && trace.queued() == 6);
  const auto rest = drain_all(trace, 1);
  CHECK(rest.size() == 6 && trace.counters().emitted_events == 8);
  CHECK(trace.counters().rx_events + trace.counters().tx_events == trace.counters().seen_events);
  CHECK(trace.counters().emitted_events + trace.counters().dropped_events == trace.counters().seen_events);
  record(trace, TraceDirection::ESP_TO_MCU, TraceKind::UART_OUTPUT, bytes(">"), 1001);
  CHECK(trace.queued() == 1);
  CHECK(trace.drain(1001, {}) == 0 && trace.queued() == 1);
}

static void sink_can_turn_off_without_dangling_borrow() {
  UartTrace trace;
  CHECK(trace.configure(10, 20)); trace.set_enabled(true, 0);
  for (unsigned i = 0; i < 3; ++i) record(trace, TraceDirection::MCU_TO_ESP, TraceKind::AT_COMMAND, bytes("AT"), 1);
  size_t calls = 0;
  CHECK(trace.drain(1, [&](const char *line) {
    ++calls; const std::string before(line); trace.set_enabled(false, 1); CHECK(std::string(line) == before);
  }) == 1);
  CHECK(calls == 1 && !trace.enabled() && trace.queued() == 0);
  CHECK(trace.counters().emitted_events == 1 && trace.counters().dropped_events == 2);
}

static void direct_at_credentials_are_never_rendered() {
  const std::vector<std::string> commands = {
    "AT+CWJAP_DEF=\"" + CANARY + "\",\"" + CANARY + "\"",
    "AT+CWSAP_DEF=\"" + CANARY + "\",\"" + CANARY + "\",6,3",
    "AT+CWLAP=\"" + CANARY + "\"",
    "AT+UNKNOWN_" + CANARY + "=" + CANARY,
    "AT+CWJAP_DEF=\"" + CANARY + "\r\nAT+RST\"",
    std::string(300, 'x') + CANARY,
  };
  for (const auto &command : commands) {
    for (TraceDirection direction : {TraceDirection::MCU_TO_ESP, TraceDirection::ESP_TO_MCU}) {
      redacted(direct(direction, TraceKind::AT_COMMAND, bytes(command)));
      redacted(direct(direction, TraceKind::UART_OUTPUT, bytes(command + "\r\n")));
    }
  }
}

static void direct_fragmented_text_both_directions_is_private() {
  const auto command = bytes("AT+CWJAP_DEF=\"" + CANARY + "\",\"" + CANARY + "\"\r\n");
  for (size_t split = 0; split <= command.size(); ++split) {
    for (TraceDirection direction : {TraceDirection::MCU_TO_ESP, TraceDirection::ESP_TO_MCU}) {
      for (TraceKind kind : {TraceKind::AT_COMMAND, TraceKind::UART_OUTPUT}) {
        UartTrace trace; CHECK(trace.configure(300, 20)); trace.set_enabled(true, 0);
        const Bytes first(command.begin(), command.begin() + split);
        const Bytes second(command.begin() + split, command.end());
        if (!first.empty()) record(trace, direction, kind, first, 1);
        if (!second.empty()) record(trace, direction, kind, second, 1);
        redacted(drain_all(trace, 1));
      }
    }
  }
}

static void modem_bytewise_credentials_and_enable_midline() {
  for (const auto &line : {
      "AT+CWJAP_DEF=\"" + CANARY + "\",\"" + CANARY + "\"",
      "AT+CWSAP_DEF=\"" + CANARY + "\",\"\",6,0",
      "AT+CWLAP=\"" + CANARY + "\""}) {
    FakeModem h(false);
    h.feed(bytes(line.substr(0, 12)), 1, true);
    h.trace.set_enabled(true, 2);
    h.feed(bytes(line.substr(12) + "\r\n"), 3, true);
    h.collect(); private_absent(h.logs);
    CHECK(h.writer_before_trace);
    CHECK(h.payloads.empty());
  }
}

static void malformed_and_overlong_modem_input_is_private() {
  const std::vector<Bytes> inputs = {
    bytes("AT+CWJAP_DEF=\"" + CANARY + "\n"),
    bytes("AT+CWSAP_DEF=\"" + CANARY + "\rbroken\n"),
    bytes("AT+CWLAP=\"" + CANARY + "\r\r\n"),
    bytes("AT+CWJAP_DEF=\"" + CANARY + std::string(300, 'q') + "\"\r\n"),
  };
  for (const auto &input : inputs) {
    FakeModem h; h.logs.clear(); h.collect(); h.logs.clear();
    h.feed(input, 1001, true); h.collect(); private_absent(h.logs);
    CHECK(h.payloads.empty());
  }
}

static void unknown_binary_ascii_and_lookalike_output_are_private() {
  Bytes unknown{0x99, 0x00, 0xFF};
  const auto secret = bytes(CANARY); unknown.insert(unknown.end(), secret.begin(), secret.end());
  unknown.push_back(0x7E);
  for (TraceDirection direction : {TraceDirection::MCU_TO_ESP, TraceDirection::ESP_TO_MCU}) {
    redacted(direct(direction, TraceKind::PAYLOAD, unknown));
    redacted(direct(direction, TraceKind::UART_OUTPUT, unknown));
    redacted(direct(direction, TraceKind::UART_OUTPUT, bytes("\r\nOK\r\n" + CANARY)));
    redacted(direct(direction, TraceKind::UART_OUTPUT, bytes("SEND OK" + CANARY)));
    redacted(direct(direction, TraceKind::NOTICE, bytes(CANARY)));
  }
}

static void binary_at_like_body_remains_one_opaque_payload() {
  FakeModem h; h.ready(); h.logs.clear();
  Bytes body{0x99, 0x00, 0xFF};
  const auto text = bytes("AT+RST\r\nAT+CWJAP_DEF=\"" + CANARY + "\",\"" + CANARY + "\"\r\n");
  body.insert(body.end(), text.begin(), text.end()); body.push_back(0x7E);
  const auto resets = h.resets;
  h.send(body, 8000, true); h.collect();
  CHECK(h.payloads.size() == 1 && h.payloads[0].second == body && h.resets == resets);
  CHECK(h.modem.counters().payloads_accepted == 1);
  CHECK(!contains(h.logs, "name=AT+RST")); private_absent(h.logs);
}

static void known_response_frames_show_only_allowlisted_bytes() {
  const std::vector<Bytes> frames = {
    {0x14, 0, 22, 0x80, 0xFF, 0, 0x7E}, {0x65, 1, 19, 0x7E},
    {0x03, 0x69, 0x7E}, {0x2D, 0x80, 0xFF, 0x7E},
  };
  for (const auto &frame : frames) {
    const auto lines = direct(TraceDirection::MCU_TO_ESP, TraceKind::PAYLOAD, frame);
    CHECK(contains(lines, "hex=" + hex(frame))); private_absent(lines);
  }
  const Bytes settings{0x2F, 100, 5, 40, 0x7E, 0xE7};
  const auto lines = direct(TraceDirection::MCU_TO_ESP, TraceKind::PAYLOAD, settings);
  CHECK(contains(lines, "hex=2F6405287E")); CHECK(contains(lines, "tail=redacted"));
  CHECK(!contains(lines, "2F6405287EE7")); CHECK(!contains(lines, "E7")); private_absent(lines);
}

static void response_length_range_and_profile_fail_closed() {
  const std::vector<Bytes> invalid = {
    {0x14, 5, 22, 0, 0, 0, 0x7E}, {0x14, 0, 22, 0, 0, 1, 0x7E},
    {0x14, 0, 22, 0, 0, 0}, {0x14, 0, 22, 0, 0, 0, 0x00},
    {0x65, 2, 19, 0x7E}, {0x65, 1, 20, 0x7E}, {0x03, 0, 0x7E, 0},
    {0x2D, 1, 2, 0}, {0x2F, 19, 0, 0, 0x7E, 0x11},
    {0x2F, 100, 4, 0, 0x7E, 0x11}, {0x2F, 100, 31, 0, 0x7E, 0x11},
    {0x2F, 100, 0, 19, 0x7E, 0x11}, {0x2F, 100, 0, 0, 0x7E},
    {0x65, 1, 19, 0x7E, 0x03, 0, 0x7E},
  };
  for (const auto &frame : invalid) redacted(direct(TraceDirection::MCU_TO_ESP, TraceKind::PAYLOAD, frame));
}

static Bytes ipd(const Bytes &payload, const std::string &length = "", const std::string &link = "1") {
  auto result = bytes("+IPD," + link + "," + (length.empty() ? std::to_string(payload.size()) : length) + ":");
  result.insert(result.end(), payload.begin(), payload.end()); return result;
}

static void outgoing_known_commands_need_exact_ipd_or_payload() {
  const std::vector<Bytes> commands = {
    {0x64, 0x7E}, {0x2B, 0x7E}, {0x2E, 0x7E}, {0x6A, 0x7E}, {0x16, 0x7E}, {0x30, 0x7E},
    {0x15, 20, 0, 0x7E}, {0x15, 100, 30, 0x7E}, {0x19, 40, 5, 0x7E},
  };
  for (const auto &command : commands) {
    const auto lines = direct(TraceDirection::ESP_TO_MCU, TraceKind::UART_OUTPUT, ipd(command));
    CHECK(contains(lines, "IPD link=1")); CHECK(contains(lines, "hex=" + hex(command))); private_absent(lines);
  }
  for (const auto &command : std::vector<Bytes>{{0x15,19,0,0x7E}, {0x15,100,4,0x7E},
      {0x19,101,0,0x7E}, {0x21,0x7E}, {0x15,100,0,0x7E,0}})
    redacted(direct(TraceDirection::ESP_TO_MCU, TraceKind::UART_OUTPUT, ipd(command)));
}

static void broken_or_fragmented_ipd_never_dumps_payload() {
  const Bytes command{0x15, 100, 0, 0x7E};
  const auto envelope = ipd(command);
  for (size_t split = 1; split < envelope.size(); ++split) {
    UartTrace trace; CHECK(trace.configure(300,20)); trace.set_enabled(true,0);
    record(trace, TraceDirection::ESP_TO_MCU, TraceKind::UART_OUTPUT, Bytes(envelope.begin(), envelope.begin()+split),1);
    record(trace, TraceDirection::ESP_TO_MCU, TraceKind::UART_OUTPUT, Bytes(envelope.begin()+split,envelope.end()),1);
    redacted(drain_all(trace,1));
  }
  const auto secret = bytes(CANARY);
  const std::vector<Bytes> malformed = {
    ipd(secret, "1"), ipd(secret, "9999999999999999999999999"), ipd(secret, "0"),
    ipd(secret, "-1"), ipd(secret, "+21"), ipd(secret, "21", "0"),
    bytes("+IPD,1,x:" + CANARY), bytes("+IPD,1,2,192.0.2.1,2081:" + CANARY),
    bytes("\r\n+IPD,1,2:" + CANARY),
  };
  for (const auto &data : malformed) redacted(direct(TraceDirection::ESP_TO_MCU, TraceKind::UART_OUTPUT, data));
  auto appended = envelope; appended.insert(appended.end(), secret.begin(), secret.end());
  redacted(direct(TraceDirection::ESP_TO_MCU, TraceKind::UART_OUTPUT, appended));
}

static void wifi_version_and_truncated_version_are_metadata_only() {
  Bytes full{0x6B}; const auto text=bytes("AT+GMR\r\nversion="+CANARY+"\r\nOK");
  full.insert(full.end(),text.begin(),text.end()); full.push_back(0x7E);
  for (size_t size : {size_t{1}, size_t{7}, full.size()-1, full.size()}) {
    const Bytes fragment(full.begin(),full.begin()+size);
    const auto lines=direct(TraceDirection::MCU_TO_ESP,TraceKind::PAYLOAD,fragment);
    redacted(lines); CHECK(contains(lines,"op=6B"));
  }
  FakeModem h; h.ready(); h.logs.clear(); h.send(full,8000); h.collect();
  CHECK(h.payloads.back().second==full); redacted(h.logs);
}

static void signed_char_bytes_do_not_expand_or_expose_unknown_data() {
  for (unsigned value=128; value<=255; ++value) {
    const Bytes status{0x14,0,static_cast<uint8_t>(value),0x80,0xFF,0,0x7E};
    const auto lines=direct(TraceDirection::MCU_TO_ESP,TraceKind::PAYLOAD,status);
    CHECK(contains(lines,"hex="+hex(status))); CHECK(!contains(lines,"FFFFFF"));
  }
  Bytes unknown{0x99}; for (unsigned value=128; value<=255; ++value) unknown.push_back(static_cast<uint8_t>(value));
  const auto marker=bytes(CANARY); unknown.insert(unknown.end(),marker.begin(),marker.end());
  redacted(direct(TraceDirection::MCU_TO_ESP,TraceKind::PAYLOAD,unknown));
}

static void borrowed_event_is_sanitized_before_original_mutates() {
  UartTrace trace; CHECK(trace.configure(10,20)); trace.set_enabled(true,0);
  Bytes data{0x14,0,22,0x80,0xFF,0,0x7E}; const auto original=hex(data);
  record(trace,TraceDirection::MCU_TO_ESP,TraceKind::PAYLOAD,data,1);
  std::fill(data.begin(),data.end(),static_cast<uint8_t>(0xA5));
  const auto lines=drain_all(trace,1); CHECK(contains(lines,"hex="+original)); CHECK(!contains(lines,"A5A5A5"));
  Bytes private_data=bytes(CANARY); record(trace,TraceDirection::ESP_TO_MCU,TraceKind::UART_OUTPUT,private_data,2);
  std::fill(private_data.begin(),private_data.end(),static_cast<uint8_t>('z')); redacted(drain_all(trace,2));
}

static void notice_allowlist_and_untrusted_bounds() {
  for (const auto &notice : {"payload_timeout","line_overflow","malformed_line","invalid_send"}) {
    const auto lines=direct(TraceDirection::MCU_TO_ESP,TraceKind::NOTICE,bytes(notice));
    CHECK(contains(lines,"notice="+std::string(notice))); private_absent(lines);
  }
  redacted(direct(TraceDirection::MCU_TO_ESP,TraceKind::NOTICE,bytes("line_overflow "+CANARY)));
  UartTrace trace; CHECK(trace.configure(10,20)); trace.set_enabled(true,0);
  trace.record(TraceDirection::MCU_TO_ESP,TraceKind::PAYLOAD,nullptr,std::numeric_limits<size_t>::max(),1);
  const auto lines=drain_all(trace,1); redacted(lines); CHECK(trace.counters().rx_bytes==std::numeric_limits<uint32_t>::max());
  CHECK(trace.summary().size()<=UartTrace::MAX_LINE);
}

static void compare_modems(const FakeModem &off,const FakeModem &on) {
  CHECK(off.writes==on.writes); CHECK(off.payloads==on.payloads); CHECK(off.writer_order==on.writer_order);
  CHECK(off.resets==on.resets); CHECK(on.writer_before_trace);
  const auto a=off.modem.diagnostics(), b=on.modem.diagnostics();
  CHECK(a.poisoned==b.poisoned && a.link_ready==b.link_ready && a.payload_pending==b.payload_pending);
  CHECK(a.payload_expected==b.payload_expected && a.payload_received==b.payload_received);
  CHECK(a.counters.rx_bytes==b.counters.rx_bytes && a.counters.tx_bytes==b.counters.tx_bytes);
  CHECK(a.counters.at_commands==b.counters.at_commands && a.counters.payloads_accepted==b.counters.payloads_accepted);
  CHECK(a.counters.payloads_rejected==b.counters.payloads_rejected && a.counters.payload_timeouts==b.counters.payload_timeouts);
}

static void tracing_changes_no_wire_writer_order_or_payload_outcome() {
  FakeModem off(false), on(true); off.ready(); on.ready();
  const std::vector<Bytes> frames={{0x65,1,19,0x7E},{0x14,0,22,8,100,0,0x7E},{0x2F,100,0,0,0x7E,0xE7},{3,0,0x7E}};
  uint32_t now=8000;
  for (const auto &frame:frames) {off.send(frame,now);on.send(frame,now);off.collect();on.collect();now+=1001;}
  CHECK(off.inject({0x64,0x7E},now)==on.inject({0x64,0x7E},now)); off.collect();on.collect();
  compare_modems(off,on); CHECK(off.logs.empty()); CHECK(!on.logs.empty());
  CHECK(on.trace_callbacks>0); CHECK(on.trace.counters().emitted_events>0);
}

static void trace_never_reenters_writer_and_starting_trace_sends_nothing() {
  FakeModem h(false); h.ready(); const auto before=h.writer_order; const auto payloads=h.payloads;
  h.trace.set_enabled(true,6200); h.now=6201; h.trace.tick(h.now); h.collect();
  CHECK(h.writer_order==before && h.payloads==payloads);
  h.command("AT",8000); CHECK(h.logs.empty());
  size_t calls=0;
  while(h.trace.queued()) CHECK(h.trace.drain(h.now,[&](const char *line){CHECK(!h.in_modem);++calls;CHECK(std::string(line).size()<=160);})<=2);
  CHECK(calls>0 && h.writer_order.size()>before.size());
  const auto after=h.writer_order; h.trace.set_enabled(false,8001); CHECK(h.writer_order==after);
}

static void tracing_changes_no_timeout_poison_reset_or_rejection() {
  FakeModem off(false),on(true);off.ready();on.ready();
  for (FakeModem *h:{&off,&on}) {
    h->command("AT+CIPSEND=1,7",8000,true);h->feed({0x14},8001,true);h->tick(9500);h->collect();
    CHECK(h->modem.diagnostics().poisoned);
    h->begin(10000);h->collect();h->command("AT+CWJAP_DEF=\""+CANARY+"\",\""+CANARY+"\"",11001,true);h->collect();
    h->command("AT+"+std::string(300,'q')+CANARY,12002,true);h->collect();
  }
  compare_modems(off,on);private_absent(on.logs);
}

static void fresh_trace_after_reboot_is_off_without_old_queue() {
  UartTrace old;CHECK(old.configure(10,20));old.set_enabled(true,0);
  record(old,TraceDirection::MCU_TO_ESP,TraceKind::AT_COMMAND,bytes("AT+CWJAP_DEF=\""+CANARY+"\""),1);
  CHECK(old.queued()==1);
  UartTrace rebooted;CHECK(!rebooted.enabled());CHECK(rebooted.queued()==0);CHECK(drain_all(rebooted,2).empty());
  old.set_enabled(false,2);CHECK(old.queued()==0);CHECK(drain_all(old,2).empty());
}

int main() {
  const std::vector<std::pair<const char *,std::function<void()>>> groups={
    {"off default and configuration bounds",defaults_and_configuration_bounds},
    {"deadline repeated ON and explicit OFF",deadline_repeated_on_and_explicit_off},
    {"active settings apply next window",active_configuration_waits_for_next_window},
    {"rollover deadline and rate",rollover_deadline_and_rate_window},
    {"rate queue drops and drain bounds",rate_queue_and_drain_are_bounded},
    {"OFF inside sink preserves borrowed line",sink_can_turn_off_without_dangling_borrow},
    {"credential and unknown AT privacy",direct_at_credentials_are_never_rendered},
    {"all text split points both directions",direct_fragmented_text_both_directions_is_private},
    {"bytewise modem credentials enable midline",modem_bytewise_credentials_and_enable_midline},
    {"malformed overlong modem input privacy",malformed_and_overlong_modem_input_is_private},
    {"unknown binary ASCII and lookalike privacy",unknown_binary_ascii_and_lookalike_output_are_private},
    {"binary AT-like body remains opaque",binary_at_like_body_remains_one_opaque_payload},
    {"known response allowlist and heap tail",known_response_frames_show_only_allowlisted_bytes},
    {"bad response length range and profile",response_length_range_and_profile_fail_closed},
    {"outgoing exact frames and IPD",outgoing_known_commands_need_exact_ipd_or_payload},
    {"broken and fragmented IPD privacy",broken_or_fragmented_ipd_never_dumps_payload},
    {"full truncated6B is metadata only",wifi_version_and_truncated_version_are_metadata_only},
    {"all signed high bytes and unknown data",signed_char_bytes_do_not_expand_or_expose_unknown_data},
    {"borrowed bytes sanitized before mutation",borrowed_event_is_sanitized_before_original_mutates},
    {"notice allowlist null and bounded counters",notice_allowlist_and_untrusted_bounds},
    {"trace off ON wire and payload passivity",tracing_changes_no_wire_writer_order_or_payload_outcome},
    {"deferred sink and trace ON sends nothing",trace_never_reenters_writer_and_starting_trace_sends_nothing},
    {"trace off ON timeout poison reset passivity",tracing_changes_no_timeout_poison_reset_or_rejection},
    {"reboot default and explicit clear",fresh_trace_after_reboot_is_off_without_old_queue},
  };
  size_t passed=0;
  for(const auto &group:groups) {
    try {group.second();++passed;std::cout<<"PASS "<<group.first<<'\n';}
    catch(const std::exception &error){std::cerr<<"FAIL "<<group.first<<": "<<error.what()<<'\n';}
  }
  std::cout<<passed<<"/"<<groups.size()<<" uart trace groups passed\n";
  return passed==groups.size()?0:1;
}
