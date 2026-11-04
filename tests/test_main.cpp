#include "cycle_stats.h"
#include "drink_temp.h"
#include "modem.h"
#include "protocol.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace esphome::ikettle_mcu;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #x); } while (0)
static Bytes bytes(const std::string &s) { return Bytes(s.begin(), s.end()); }
static bool contains(const Bytes &b, const std::string &s) { return std::search(b.begin(),b.end(),s.begin(),s.end())!=b.end(); }
struct ModemHarness {
  Bytes tx;
  std::vector<std::pair<uint8_t,Bytes>> received;
  unsigned resets=0;
  bool accept=true;
  VirtualModem modem;
  ModemHarness():modem(ModemCallbacks{
    [this](const Bytes &b){tx.insert(tx.end(),b.begin(),b.end());},
    [this](uint8_t link,const Bytes &b){received.emplace_back(link,b);return accept;},
    [this](){++resets;}}) { modem.set_mac_address("02:00:00:00:00:42"); modem.begin(0); tx.clear(); }
  void feed(const Bytes &b,uint32_t t) { modem.feed(b.data(),b.size(),t); }
  void command(const std::string &s,uint32_t t) { feed(bytes(s+"\r\n"),t); }
  void bootstrap(uint32_t t=10) {
    command("AT",t);command("AT+CWMODE_DEF=1",t);command("AT+CIPMUX=1",t);
    command("AT+CIPSERVER=1,2081",t);command("AT+CIPSTO=180",t);
    command("AT+CIPSTART=0,\"UDP\",\"0\",0,2081,2",t);command("AT+RFPOWER=50",t);
  }
  void ready() { bootstrap();modem.tick(6010);modem.tick(6110);CHECK(modem.can_inject(6110));tx.clear(); }
  void send(uint8_t link,const Bytes &payload,uint32_t t) {
    command("AT+CIPSEND="+std::to_string(link)+","+std::to_string(payload.size()),t);
    CHECK(contains(tx,">")); feed(payload,t+1);
  }
};
static void modem_binary() {
  ModemHarness h;h.ready();
  const Bytes payload={0,0x7d,0x7e,13,10,'A','T','+','R','S','T',13,10};
  const auto header=bytes("AT+CIPSEND=1,13\r\n");
  for (auto b:header) h.feed(Bytes{b},6200);
  CHECK(contains(h.tx,">")); CHECK(h.received.empty()); h.tx.clear();
  for(size_t i=0;i<payload.size();++i) { h.feed(Bytes{payload[i]},6201); if(i+1<payload.size()) CHECK(h.received.empty()); }
  CHECK(h.received.size()==1); CHECK(h.received[0].first==1); CHECK(h.received[0].second==payload);
  CHECK(contains(h.tx,"SEND OK"));CHECK(h.resets==1);
  h.accept=false;h.tx.clear();h.send(1,Bytes{0x14,0,22,8,100,0,0x7e},6300);
  CHECK(contains(h.tx,"ERROR"));CHECK(!contains(h.tx,"SEND OK"));
}
static void modem_readiness() {
  ModemHarness h;h.bootstrap();
  CHECK(!contains(h.tx,"WIFI CONNECTED"));CHECK(!contains(h.tx,"0,CONNECT"));
  CHECK(!h.modem.can_inject(6009));h.modem.tick(6010);
  CHECK(contains(h.tx,"1,CONNECT"));CHECK(!h.modem.can_inject(6109));
  h.modem.tick(6110);CHECK(h.modem.can_inject(6110));
  h.tx.clear();CHECK(h.modem.inject(Bytes{0x64,0x7e},6110));CHECK(contains(h.tx,"+IPD,1,2:"));
}
static void modem_scan_guard() {
  ModemHarness h;h.bootstrap();h.modem.tick(6010);h.modem.tick(6110);
  h.tx.clear();h.command("AT+CWLAP=\"synthetic-ssid\"",6200);
  CHECK(contains(h.tx,"OK"));CHECK(!contains(h.tx,"+CWLAP:("));CHECK(!contains(h.tx,"WIFI CONNECTED"));
  CHECK(!h.modem.can_inject(12199));h.modem.tick(12200);CHECK(h.modem.can_inject(12200));
}
static void modem_status_does_not_starve_bootstrap() {
  ModemHarness h;h.ready();
  for(uint32_t t=6200;t<=18200;t+=2000) {h.send(1,Bytes{0x14,0,22,8,100,0,0x7e},t);h.modem.tick(t+2);CHECK(h.modem.can_inject(t+2));}
}
static void modem_poison() {
  ModemHarness h;h.command("AT+CIPSEND=1,999999",10);h.tx.clear();h.command("AT",20);
  CHECK(!contains(h.tx,"OK"));h.modem.begin(30);h.tx.clear();h.command("AT",31);CHECK(contains(h.tx,"OK"));
  h.bootstrap(40);h.modem.tick(6040);h.modem.tick(6140);h.tx.clear();h.command("AT+CIPSEND=1,7",6200);h.feed(Bytes{0x14},6201);h.modem.tick(100000);h.tx.clear();h.command("AT",100001);
  CHECK(!contains(h.tx,"OK"));CHECK(!h.modem.can_inject(100001));
}
static void modem_at_forms() {
  // All 20 observed command forms; fixture credentials below are invented.
  const std::vector<std::string> commands={"AT","AT+RESTORE","AT+CWMODE_DEF=3","AT+CWMODE_DEF=1",
    "AT+CWJAP_DEF?","AT+CWQAP","AT+CIPAPMAC_CUR?","AT+CWSAP_DEF=\"iKettle 2.0:42\",\"\",6,0",
    "AT+RST","AT+CWJAP_DEF=\"synthetic-ssid\",\"synthetic-password\"","AT+CIPMUX=1",
    "AT+CIPSERVER=1,2081","AT+CIPSTO=180","AT+CIPSTART=0,\"UDP\",\"0\",0,2081,2",
    "AT+RFPOWER=50","AT+RFPOWER=82","AT+CWLAP","AT+CWLAP=\"synthetic-ssid\"","AT+GMR","AT+CIPSEND=1,0"};
  for(const auto &command:commands) { ModemHarness h;h.ready();h.command(command,6200);CHECK(!h.tx.empty());CHECK(!contains(h.tx,"ERROR"));CHECK(!contains(h.tx,"WIFI CONNECTED")); }
  ModemHarness h;h.command("AT+CIPAPMAC_CUR?",10);CHECK(contains(h.tx,"02:00:00:00:00:42"));
  for(unsigned slot:{0u,2u,3u,4u}){h.tx.clear();h.command("AT+CIPSEND="+std::to_string(slot)+",3",20);CHECK(contains(h.tx,"ERROR"));}
}
struct SessionHarness {
  std::vector<Bytes> sent;
  bool accept=true;
  KettleSession session;
  SessionHarness():session([this](const Bytes &b,uint32_t){sent.push_back(b);return accept;}) {}
  void receive(const Bytes &b,uint32_t t) { session.receive(1,b,t); }
  void identify(uint32_t t=100) { session.tick(t,true);CHECK(!sent.empty());CHECK(sent.front()==Bytes({0x64,0x7e}));receive(Bytes{0x65,1,19,0x7e},t+1); }
  void status(uint32_t t=200,uint8_t state=0,uint8_t temp=22) { receive(Bytes{0x14,state,temp,8,100,0,0x7e},t); }
  void drain_metadata(uint32_t t=300) {
    for(unsigned i=0;i<8;++i) {
      auto old=sent.size();session.tick(t,true);
      if(sent.size()>old) { auto op=sent.back()[0];if(op==0x2e){receive(Bytes{0x2f,100,0,40,0x7e,0xa5},t+1);receive(Bytes{3,0,0x7e},t+2);}else if(op==0x2b){receive(Bytes{0x2d,0x0a,0x96,0x7e},t+1);receive(Bytes{3,0,0x7e},t+2);} }
      t+=100;
    }
  }
};
static void protocol_real_frames() {
  SessionHarness h;h.identify();h.status();
  CHECK(h.session.state().identity_known);CHECK(h.session.state().supported);CHECK(h.session.state().status_known);
  CHECK(h.session.state().on_base);CHECK(h.session.state().water_raw==2148);
  h.session.tick(210,true);CHECK(h.sent.back()==Bytes({0x2e,0x7e}));
  // Complete raw capture record 22 (manual boundary, not old probe decoding).
  h.receive(Bytes{0x2f,100,0,0,0x7e,0},215);CHECK(!h.session.state().defaults_known);h.receive(Bytes{3,0,0x7e},216);CHECK(h.session.state().defaults_known);
  CHECK(h.session.state().default_temperature==100);CHECK(h.session.state().default_keep_minutes==0);CHECK(h.session.state().default_formula_temperature==0);
  h.session.tick(220,true);CHECK(h.sent.back()==Bytes({0x2b,0x7e}));h.receive(Bytes{0x2d,0x0a,0x96,0x7e},221);h.receive(Bytes{3,0,0x7e},222);CHECK(h.session.state().base_known);CHECK(h.session.state().base_raw==2710);
  CHECK(h.session.refresh(230));
  h.session.tick(231,true);CHECK(h.sent.back()==Bytes({0x64,0x7e}));h.receive(Bytes{0x65,1,19,0x7e},232);
  h.session.tick(233,true);CHECK(h.sent.back()==Bytes({0x2e,0x7e}));
  // Synthetic 2F: offset 5 is arbitrary heap residue, not a terminator.
  h.receive(Bytes{0x2f,100,0,40,0x7e,0xa5},234);h.receive(Bytes{3,0,0x7e},235);CHECK(h.session.state().defaults_known);
  CHECK(h.session.state().default_temperature==100);CHECK(h.session.state().default_formula_temperature==40);
  h.session.tick(236,true);h.receive(Bytes{0x2d,0x0a,0x96,0x7e},237);h.receive(Bytes{3,0,0x7e},238);
  h.session.tick(239,true);CHECK(h.sent.back()==Bytes({0x6a,0x7e}));
  const std::string wifi="AT+GMR\r\r\nAT version:0.40.0.0(Aug  8 2015 14:45:58)\r\nSDK version:1.3.0\r\ncompile time:Aug  8 2015 17:19:38\r\nOK";
  Bytes wifi_frame{0x6b};auto text=bytes(wifi);wifi_frame.insert(wifi_frame.end(),text.begin(),text.end());wifi_frame.push_back(0x7e);h.receive(wifi_frame,240);CHECK(!h.session.state().wifi_version.empty());
  h.receive(Bytes{0x14,0,0xfb,8,0x7e,0,0x7e},250);CHECK(h.session.state().water_raw==2174);CHECK(h.session.state().temp==-5.0f);
  h.status(260,0,127);CHECK(!h.session.state().on_base);CHECK(std::isnan(h.session.state().temp));CHECK(!h.session.start(100,0,false,261));
}
static void protocol_no_implicit_heat() {
  SessionHarness h;h.identify();h.status();h.drain_metadata();
  for(const auto &b:h.sent) CHECK(b[0]!=0x15 && b[0]!=0x19 && b[0]!=0x16 && b[0]!=0x1a);
  CHECK(!h.session.start(100,0,false,10201));
  h.session.reset();CHECK(!h.session.start(100,0,false,10300));
  h.session.tick(10400,false);CHECK(!h.session.start(100,0,false,10401));
}
static void protocol_one_physical_start() {
  SessionHarness h;h.identify();h.status();h.drain_metadata();h.status(1200);
  auto before=h.sent.size();CHECK(h.session.start(100,0,false,1201));CHECK(h.sent.size()==before+1);
  CHECK(h.sent.back()[0]==0x15);CHECK(!h.session.start(100,0,false,1202));
  CHECK(h.session.state().state_code==0);h.receive(Bytes{3,0,0x7e},1203);CHECK(h.session.state().state_code==0);
  h.status(1204,1,23);CHECK(h.session.state().state_code==1);
  for(uint32_t t=2000;t<20000;t+=1000)h.session.tick(t,true);
  CHECK(std::count_if(h.sent.begin(),h.sent.end(),[](const Bytes &b){return b[0]==0x15||b[0]==0x19;})==1);
}
static void protocol_busy_and_late_ack() {
  SessionHarness h;h.identify();h.status();
  h.session.tick(300,true); // read-only metadata is now in flight
  const auto n=h.sent.size();CHECK(!h.session.start(100,0,false,301));CHECK(h.sent.size()==n);
  h.session.tick(4000,true); // expire the untagged transaction
  h.receive(Bytes{3,0,0x7e},4001); // a late ACK may not authorize a heater operation
  h.receive(Bytes{3,0,0x7e},4002);h.status(4003);
  CHECK(!h.session.start(100,0,false,4004));
  CHECK(std::none_of(h.sent.begin(),h.sent.end(),[](const Bytes &b){return b[0]==0x15||b[0]==0x19;}));
}
static void protocol_requires_post_start_ack_status() {
  SessionHarness h; h.identify(); h.status(); h.drain_metadata(); h.status(1200);
  CHECK(h.session.start(90,0,false,1201));
  // A new frame before the ACK still cannot authorize a second start.
  h.status(1202); h.receive(Bytes{3,0,0x7e},1203);
  CHECK(!h.session.start(80,0,false,1204));
  CHECK(!h.session.start(80,0,true,1205));
  CHECK(h.session.diagnostics().awaiting_start_status);
  h.status(1206,1,23);
  CHECK(!h.session.diagnostics().awaiting_start_status);
  CHECK(!h.session.start(80,0,false,1207));
  // A newly observed idle phase permits a new, explicit user intention.
  h.status(1210,0,23); CHECK(h.session.start(80,0,true,1211));
  CHECK(std::count_if(h.sent.begin(),h.sent.end(),[](const Bytes &b){return b[0]==0x15||b[0]==0x19;})==2);
}
static void protocol_reports_confirmed_command_outcomes() {
  SessionHarness h;
  h.identify();
  h.status();
  h.drain_metadata();
  const auto result = [&] { return h.session.diagnostics().command_status; };
  const Bytes ack{3, 0, 0x7e};

  CHECK(!h.session.start(100, 0, false, 10201));
  CHECK(result() == CommandStatus::NO_FRESH_STATUS);
  h.status(10300, 0, 127);
  CHECK(!h.session.start(100, 0, false, 10301));
  CHECK(result() == CommandStatus::OFF_BASE);
  h.status(10400);
  CHECK(h.session.start(100, 0, false, 10401));
  CHECK(result() == CommandStatus::SENT);
  h.status(10402, 1);
  h.receive(ack, 10403);
  CHECK(result() == CommandStatus::SENT);
  h.status(10404, 1);
  CHECK(result() == CommandStatus::HEATING);
  CHECK(!h.session.start(100, 0, false, 10405));
  CHECK(result() == CommandStatus::ALREADY_ACTIVE);

  CHECK(h.session.stop(10406));
  CHECK(result() == CommandStatus::SENT);
  h.receive(ack, 10407);
  h.status(10408, 1);
  CHECK(result() == CommandStatus::SENT);
  h.status(10409, 0);
  CHECK(result() == CommandStatus::STOPPED);

  CHECK(h.session.start(65, 0, true, 10410));
  h.receive(Bytes{3, 5, 0x7e}, 10411);
  CHECK(result() == CommandStatus::MCU_REJECTED);
  CHECK(h.session.start(65, 0, true, 10412));
  CHECK(!h.session.stop(10413));
  CHECK(result() == CommandStatus::BUSY);
  h.receive(ack, 10414);
  h.status(10415, 0);
  h.status(19900, 0);
  h.session.tick(20412, true);
  CHECK(result() == CommandStatus::NO_MATCHING_STATUS);
  h.status(20413, 1);
  CHECK(result() == CommandStatus::NO_MATCHING_STATUS);

  h.status(20414, 2);
  CHECK(h.session.stop_keep_warm(20415));
  h.receive(ack, 20416);
  h.status(20417, 2);
  CHECK(result() == CommandStatus::SENT);
  h.status(20418, 0);
  CHECK(result() == CommandStatus::KEEP_WARM_STOPPED);

  CHECK(h.session.stop(20419));
  h.session.tick(23419, true);
  CHECK(result() == CommandStatus::NO_ACK);
}
static void protocol_wrap_and_unsupported() {
  SessionHarness h;h.session.tick(0xffffffefu,true);h.session.receive(1,Bytes{0x65,1,19,0x7e},0xfffffff0u);
  h.status(0xfffffff0u);CHECK(h.session.status_fresh(0x100u));CHECK(!h.session.status_fresh(0x3000u));
  SessionHarness bad;bad.session.tick(1,true);bad.receive(Bytes{0x65,2,20,0x7e},2);bad.status(3);
  CHECK(!bad.session.state().supported);CHECK(!bad.session.start(100,0,false,4));
}
static void end_to_end_fake_mcu() {
  Bytes uart;KettleSession *session_ptr=nullptr;VirtualModem *modem_ptr=nullptr;
  VirtualModem modem(ModemCallbacks{[&](const Bytes &b){uart.insert(uart.end(),b.begin(),b.end());},
    [&](uint8_t link,const Bytes &b){session_ptr->receive(link,b,6200);return true;},[&](){if(session_ptr)session_ptr->reset();}});
  modem_ptr=&modem;
  KettleSession session([&](const Bytes &b,uint32_t t){return modem_ptr->inject(b,t);});session_ptr=&session;
  modem.set_mac_address("02:00:00:00:00:42");modem.begin(0);
  auto feed=[&](const std::string &s,uint32_t t){auto b=bytes(s);modem.feed(b.data(),b.size(),t);};
  feed("AT+CIPMUX=1\r\nAT+CIPSERVER=1,2081\r\nAT+CIPSTART=0,\"UDP\",\"0\",0,2081,2\r\nAT+RFPOWER=50\r\n",10);
  session.tick(100,false);CHECK(!contains(uart,"+IPD"));modem.tick(6010);modem.tick(6110);uart.clear();
  session.tick(6110,modem.link_ready(),modem.can_inject(6110));CHECK(contains(uart,"+IPD,1,2:"));CHECK(uart.size()>=2);CHECK(uart[uart.size()-2]==0x64);CHECK(uart.back()==0x7e);
  uart.clear();feed("AT+CIPSEND=1,4\r\n",6200);CHECK(contains(uart,">"));
  const Bytes identity{0x65,1,19,0x7e};modem.feed(identity.data(),identity.size(),6200);CHECK(contains(uart,"SEND OK"));CHECK(session.state().supported);
  feed("AT+CIPSEND=1,7\r\n",6201);const Bytes status{0x14,0,22,8,100,0,0x7e};
  modem.feed(status.data(),3,6201);CHECK(!session.state().status_known);modem.feed(status.data()+3,4,6202);
  CHECK(session.state().status_known);CHECK(session.state().water_raw==2148);CHECK(session.state().state_code==0);
  session.tick(6300,modem.link_ready(),modem.can_inject(6300));CHECK(session.diagnostics().in_flight);CHECK(session.diagnostics().in_flight_name=="defaults");
  const auto epoch=session.diagnostics().session_epoch;const auto requests=session.diagnostics().requests_sent;
  const auto sequence=session.state().status_sequence;
  uart.clear();feed("AT+CIPS",6301);CHECK(modem.link_ready());CHECK(!modem.can_inject(6301));
  session.tick(6301,modem.link_ready(),modem.can_inject(6301));CHECK(session.state().identity_known);CHECK(session.diagnostics().session_epoch==epoch);
  feed("END=1,7\r\n",6302);session.tick(6302,modem.link_ready(),modem.can_inject(6302));
  modem.feed(status.data(),3,6303);session.tick(6303,modem.link_ready(),modem.can_inject(6303));
  CHECK(session.state().status_sequence==sequence);CHECK(session.diagnostics().in_flight_name=="defaults");
  modem.feed(status.data()+3,4,6304);session.tick(6304,modem.link_ready(),modem.can_inject(6304));
  CHECK(session.state().status_sequence==sequence+1);CHECK(session.state().identity_known);CHECK(session.diagnostics().session_epoch==epoch);
  CHECK(session.diagnostics().requests_sent==requests);CHECK(session.diagnostics().in_flight_name=="defaults");CHECK(!contains(uart,"+IPD,1,2:"));
}
static void warm_mcu_after_independent_esphome_reboot() {
  Bytes uart;std::vector<Bytes> injected;uint32_t now=0;
  KettleSession *session_ptr=nullptr;VirtualModem *modem_ptr=nullptr;
  VirtualModem modem(ModemCallbacks{
    [&](const Bytes &b){uart.insert(uart.end(),b.begin(),b.end());},
    [&](uint8_t link,const Bytes &b){session_ptr->receive(link,b,now);return true;},
    [&](){if(session_ptr)session_ptr->reset();}});
  modem_ptr=&modem;
  KettleSession session([&](const Bytes &b,uint32_t t){if(!modem_ptr->inject(b,t))return false;injected.push_back(b);return true;});
  session_ptr=&session;modem.set_mac_address("02:00:00:00:00:42");modem.begin(0);uart.clear();
  auto feed=[&](const Bytes &b,uint32_t t){now=t;modem.feed(b.data(),b.size(),t);};
  auto loop=[&](uint32_t t){now=t;modem.tick(t);session.tick(t,modem.link_ready(),modem.can_inject(t));};
  // Independent ESPHome reboot: the original MCU remains in warm-keep state 2
  // and does not replay its AT bootstrap. Its first command is ordinary TX.
  feed(bytes("AT+CIPSEND=1,7\r\n"),100);CHECK(contains(uart,">"));
  feed(Bytes{0x14,2,80,8,100,0,0x7e},101);CHECK(contains(uart,"SEND OK"));
  const auto diagnostics=modem.diagnostics();
  CHECK(!diagnostics.mux_seen);CHECK(!diagnostics.server_seen);CHECK(!diagnostics.udp_zero_reserved);CHECK(!diagnostics.rf_power_seen);
  loop(102);loop(6099);CHECK(!modem.link_ready());CHECK(!modem.can_inject(6099));CHECK(injected.empty());CHECK(!contains(uart,"+IPD"));
  loop(6100);CHECK(contains(uart,"1,CONNECT"));CHECK(!modem.can_inject(6100));CHECK(injected.empty());
  loop(6199);CHECK(injected.empty());loop(6200);
  CHECK(injected.size()==1);CHECK(injected.front()==Bytes({0x64,0x7e}));CHECK(session.diagnostics().in_flight_name=="identity");
  feed(bytes("AT+CIPSEND=1,4\r\n"),6201);feed(Bytes{0x65,1,19,0x7e},6202);
  feed(bytes("AT+CIPSEND=1,7\r\n"),6203);feed(Bytes{0x14,2,80,8,100,0,0x7e},6204);loop(6205);
  CHECK(session.state().identity_known);CHECK(session.state().supported);CHECK(session.state().status_known);
  CHECK(session.state().state_code==2);CHECK(session.status_fresh(6205));CHECK(!session.start(100,0,false,6206));
  CHECK(std::none_of(injected.begin(),injected.end(),[](const Bytes &b){return b[0]==0x15||b[0]==0x19||b[0]==0x16||b[0]==0x30;}));
  // A zero-length first TX is not evidence of a running MCU application.
  ModemHarness empty;empty.command("AT+CIPSEND=1,0",100);empty.modem.tick(10000);
  CHECK(!empty.modem.link_ready());CHECK(!empty.modem.can_inject(10000));CHECK(!contains(empty.tx,"1,CONNECT"));
}
static void cycle_statistics_from_status() {
  CycleTracker tracker;
  KettleState state;
  state.on_base = true;
  auto observe = [&](uint32_t time, uint8_t phase, float temperature, uint32_t epoch = 1) {
    ++state.status_sequence;
    state.last_status_ms = time;
    state.state_code = phase;
    state.temp = temperature;
    return tracker.observe(state, epoch, true);
  };
  CHECK(!observe(0xfffffb00u, 0, 21));
  CHECK(!observe(0xfffffc00u, 1, 22));
  CHECK(!observe(0x00000b70u, 1, 73)); // 3.952 s across clock rollover.
  CHECK(!tracker.observe(state, 1, true)); // Re-publishing is not a new sample.
  CHECK(!observe(0x00000fa0u, 4, 99));
  CHECK(!observe(5000, 2, 65));
  CHECK(!observe(8000, 2, 63));
  const auto completed = observe(11000, 0, 64);
  CHECK(completed.has_value());
  CHECK(completed->start_temperature == 22);
  CHECK(completed->heating_ms == 5024);
  CHECK(completed->keep_warm_ms == 6000);
  CHECK(std::abs(completed->heating_energy_wh(2200) - 3.07022f) < 0.001f);
  CHECK(!tracker.observe(state, 1, true));

  CHECK(!observe(12000, 1, 50));
  CHECK(!observe(22000, 0, 61)); // Timing gap invalidates the cycle.
  CHECK(!observe(23000, 1, 60));
  CHECK(!tracker.observe(state, 1, false));
  CHECK(!observe(24000, 0, 61));
  CHECK(!observe(25000, 1, 58));
  CHECK(!observe(26000, 2, 66, 2)); // Reconnected part way through.
  CHECK(!observe(27000, 0, 65, 2));
  CHECK(!observe(28000, 1, 64, 2));
  state.on_base = false;
  const auto lifted = observe(29000, 0, 127, 2);
  CHECK(lifted.has_value());
  CHECK(lifted->heating_ms == 1000 && lifted->keep_warm_ms == 0);
}

static void drink_temperature_after_cooling() {
  DrinkTemperatureAlert alert;
  KettleState state;
  auto observe = [&](uint8_t phase, float temp, bool on_base = true, float threshold = 65,
                     uint32_t epoch = 1) {
    state.on_base = on_base;
    state.state_code = phase;
    state.temp = temp;
    ++state.status_sequence;
    state.last_status_ms += 1000;
    return alert.update(state, epoch, true, threshold);
  };
  CHECK(!observe(1, 95)); // Reboot mid-cycle.
  CHECK(!observe(4, 65));
  CHECK(!observe(0, 64));
  CHECK(!observe(1, 24));
  CHECK(!observe(1, 90));
  CHECK(!observe(4, 66, true, 80)); // Target change applies to the next cycle.
  CHECK(observe(4, 65));
  CHECK(!alert.update(state, 1, true, 65)); // A repeat cannot re-emit the event.
  CHECK(!observe(0, 64));
  CHECK(!observe(1, 68));
  CHECK(!observe(2, 64)); // Don't alert while keeping warm.
  CHECK(observe(0, 64));
  CHECK(!observe(1, 89));
  CHECK(!observe(4, 72));
  CHECK(!observe(0, 65, false)); // Jug lifted.
  CHECK(!observe(0, 64));
  CHECK(!observe(1, 89));
  alert.cancel(); // Stop button.
  CHECK(!observe(1, 70));
  CHECK(!observe(0, 65));
  CHECK(!observe(1, 89));
  CHECK(!observe(4, 65, true, 65, 2)); // Modem session interrupted.
}

int main() {
  const std::vector<std::pair<const char*,std::function<void()>>> tests={
    {"binary exact-count and rejection",modem_binary},{"bootstrap quiet period and slot",modem_readiness},
    {"empty scan guard",modem_scan_guard},{"regular status does not starve injection",modem_status_does_not_starve_bootstrap},
    {"overflow and timeout poison",modem_poison},{"20 observed AT forms",modem_at_forms},{"real captures and synthetic edges",protocol_real_frames},
    {"no startup/reconnect heat",protocol_no_implicit_heat},{"one physical start and observed status",protocol_one_physical_start},
    {"busy and late untagged ACK",protocol_busy_and_late_ack},
    {"start ACK requires later status",protocol_requires_post_start_ack_status},
    {"commands report actual status and actionable errors",protocol_reports_confirmed_command_outcomes},
    {"rollover and unsupported profile",protocol_wrap_and_unsupported},{"fake MCU through CIPSEND and delayed IPD",end_to_end_fake_mcu},
    {"warm MCU after independent ESPHome reboot",warm_mcu_after_independent_esphome_reboot},
    {"cycle statistics from continuous MCU status",cycle_statistics_from_status},
    {"drink temperature once per observed cooldown",drink_temperature_after_cooling}};
  unsigned passed=0;for(const auto &test:tests){try{test.second();++passed;std::cout<<"PASS "<<test.first<<'\n';}catch(const std::exception &e){std::cerr<<"FAIL "<<test.first<<": "<<e.what()<<'\n';}}
  std::cout<<passed<<"/"<<tests.size()<<" cases passed\n";return passed==tests.size()?0:1;
}
