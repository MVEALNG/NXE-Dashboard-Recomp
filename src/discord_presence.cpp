// Discord rich presence, spoken directly.
//
// discord-rpc, the library the API docs point at, is archived and wants a
// RapidJSON submodule and a second CMake project to build one thing: a named
// pipe carrying JSON. The wire format is small enough to write out plainly,
// which is what this does -- no submodule, no dependency, and the failure modes
// stay visible in this file rather than inside a vendored library.
//
// The protocol, in full:
//
//   Discord listens on \\.\pipe\discord-ipc-N, N in 0..9, first one that opens
//   wins. Every message is a 4-byte little-endian opcode, a 4-byte
//   little-endian payload length, then that many bytes of UTF-8 JSON.
//
//     op 0  HANDSHAKE  {"v":1,"client_id":"..."}   must be sent first
//     op 1  FRAME      commands, in both directions
//     op 2  CLOSE      either side, with a reason
//     op 3  PING       -> the other side answers PONG
//     op 4  PONG
//
//   Presence is one FRAME:
//
//     {"cmd":"SET_ACTIVITY","nonce":"<unique>",
//      "args":{"pid":<pid>,"activity":{...}}}
//
// Everything happens on a worker thread. The dashboard thread only ever takes a
// short lock to leave a new state behind, so a missing or wedged Discord cannot
// stall the UI -- which matters here, because the launch path calls this while
// it is already juggling window focus with the emulator.

#include "discord_presence.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// The application whose name and art Discord shows. Created by the user at
// discord.com/developers/applications; the id is the "Application ID" on the
// General Information page. Empty means rich presence stays off, which is why
// nothing here needs a build change to disable it.
REXCVAR_DEFINE_STRING(discord_client_id, "", "Discord",
                      "Discord application id for rich presence. Empty disables it.");

// Art Assets keys, uploaded under Rich Presence in that same application.
REXCVAR_DEFINE_STRING(discord_default_image, "nxe", "Discord",
                      "Art asset key shown on the dashboard, and beside game art in-game.");

// A game's art is looked up by its title id in lower-case hex -- "4d5307e6" for
// Halo 3 -- so adding art for a game is uploading one asset under that name and
// needs no code change. Discord silently shows nothing for a key it does not
// have, so a missing one costs the picture and not the presence.
REXCVAR_DEFINE_BOOL(discord_game_art, true, "Discord",
                    "Look a game's art up by its title id in lower-case hex.");

namespace nxe_discord {
namespace {

using Clock = std::chrono::steady_clock;

// Discord throttles presence updates to roughly one per fifteen seconds and
// quietly drops the rest, so a change arriving inside that window is held and
// sent when the window opens rather than thrown away.
constexpr auto kMinUpdateInterval = std::chrono::seconds(15);
constexpr auto kReconnectInterval = std::chrono::seconds(10);

struct Activity {
  std::string details;
  std::string state;
  std::string large_image;
  std::string large_text;
  std::string small_image;
  std::string small_text;
  int64_t start_time = 0;

  bool operator==(const Activity& o) const {
    return details == o.details && state == o.state && large_image == o.large_image &&
           large_text == o.large_text && small_image == o.small_image &&
           small_text == o.small_text && start_time == o.start_time;
  }
};

std::mutex g_mutex;
std::condition_variable g_wake;
std::thread g_thread;
bool g_running = false;
bool g_dirty = false;
Activity g_wanted;

HANDLE g_pipe = INVALID_HANDLE_VALUE;

int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// JSON string contents, escaped. Everything below 0x20 goes out as \u00XX;
// bytes above ASCII are passed through, since the payload is already UTF-8 and
// Discord reads it as such.
std::string Escape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

void AppendField(std::string& json, const char* name, const std::string& value, bool& first) {
  if (value.empty()) {
    return;
  }
  if (!first) {
    json += ',';
  }
  first = false;
  json += '"';
  json += name;
  json += "\":\"";
  json += Escape(value);
  json += '"';
}

std::string BuildActivityJson(const Activity& a) {
  std::string assets;
  {
    bool first = true;
    AppendField(assets, "large_image", a.large_image, first);
    AppendField(assets, "large_text", a.large_text, first);
    AppendField(assets, "small_image", a.small_image, first);
    AppendField(assets, "small_text", a.small_text, first);
  }

  std::string activity;
  bool first = true;
  AppendField(activity, "details", a.details, first);
  AppendField(activity, "state", a.state, first);
  if (a.start_time) {
    if (!first) {
      activity += ',';
    }
    first = false;
    activity += "\"timestamps\":{\"start\":" + std::to_string(a.start_time) + "}";
  }
  if (!assets.empty()) {
    if (!first) {
      activity += ',';
    }
    activity += "\"assets\":{" + assets + "}";
  }
  return "{" + activity + "}";
}

bool WriteFrame(uint32_t opcode, const std::string& payload) {
  if (g_pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  std::string frame;
  frame.resize(8 + payload.size());
  const uint32_t length = static_cast<uint32_t>(payload.size());
  std::memcpy(&frame[0], &opcode, 4);
  std::memcpy(&frame[4], &length, 4);
  std::memcpy(&frame[8], payload.data(), payload.size());

  DWORD written = 0;
  if (!WriteFile(g_pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) ||
      written != frame.size()) {
    return false;
  }
  return true;
}

bool WriteFrame(uint32_t opcode, const std::string& payload);
void Disconnect();

void Disconnect() {
  if (g_pipe != INVALID_HANDLE_VALUE) {
    CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
  }
}

// Discord answers the handshake and every frame, and those answers are the only
// place it ever explains itself.
//
// Discarding them unread was why a run that logged "connected" still showed
// nothing on the profile: a bad client id, a malformed payload and a healthy
// connection all look identical from the sending side. Discord reports the
// first two in a CLOSE frame carrying a reason, and confirms the third with
// DISPATCH/READY, so the replies are parsed and logged.
//
// Reads can arrive split across frame boundaries, so bytes accumulate in a
// buffer and only complete frames are taken from it.
std::string g_rx;

// Discord drops anything sent between the handshake and its READY dispatch.
//
// The pipe accepts the write and reports success, so the send looks fine from
// here -- the first working build connected, sent the activity in the same
// millisecond, and got READY back a second later with the activity already
// discarded. Nothing is sent until this turns true.
bool g_ready = false;

void HandleFrame(uint32_t opcode, const std::string& payload) {
  switch (opcode) {
    case 1: {  // FRAME
      static int logged = 0;
      if (logged < 4) {
        ++logged;
        REXLOG_INFO("Discord: <- {}", payload);
      }
      if (!g_ready && payload.find("\"evt\":\"READY\"") != std::string::npos) {
        g_ready = true;
        REXLOG_INFO("Discord: handshake complete; publishing presence");
      }
      break;
    }
    case 2:  // CLOSE, always with a reason
      REXLOG_WARN("Discord: closed by Discord: {}", payload);
      Disconnect();
      break;
    case 3:  // PING, answer it or the connection is dropped
      WriteFrame(4, payload);
      break;
    default:
      break;
  }
}

void DrainReplies() {
  if (g_pipe == INVALID_HANDLE_VALUE) {
    return;
  }
  char buffer[4096];
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &available, nullptr)) {
      Disconnect();
      return;
    }
    if (!available) {
      break;
    }
    DWORD read = 0;
    const DWORD want = available < sizeof(buffer) ? available : sizeof(buffer);
    if (!ReadFile(g_pipe, buffer, want, &read, nullptr) || !read) {
      Disconnect();
      return;
    }
    g_rx.append(buffer, read);
  }

  while (g_rx.size() >= 8) {
    uint32_t opcode = 0;
    uint32_t length = 0;
    std::memcpy(&opcode, &g_rx[0], 4);
    std::memcpy(&length, &g_rx[4], 4);
    if (g_rx.size() < 8 + length) {
      break;  // the rest is still on its way
    }
    const std::string payload = g_rx.substr(8, length);
    g_rx.erase(0, 8 + length);
    HandleFrame(opcode, payload);
    if (g_pipe == INVALID_HANDLE_VALUE) {
      return;
    }
  }
}

bool Connect(const std::string& client_id) {
  for (int i = 0; i < 10; ++i) {
    char path[64];
    std::snprintf(path, sizeof(path), "\\\\.\\pipe\\discord-ipc-%d", i);
    HANDLE pipe = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                              nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      continue;
    }
    // The handle stays in blocking mode. PIPE_NOWAIT is documented as existing
    // only for LanMan 2.0 compatibility and can make WriteFile report a short
    // write on a full buffer; asking PeekNamedPipe what is waiting before every
    // read keeps the worker from blocking without it.
    g_pipe = pipe;
    g_rx.clear();
    g_ready = false;
    if (!WriteFrame(0, "{\"v\":1,\"client_id\":\"" + Escape(client_id) + "\"}")) {
      Disconnect();
      continue;
    }
    REXLOG_INFO("Discord: connected on discord-ipc-{}", i);
    return true;
  }
  return false;
}

bool SendActivity(const Activity& a) {
  static uint32_t nonce = 0;
  const std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" +
                              std::to_string(++nonce) + "\",\"args\":{\"pid\":" +
                              std::to_string(static_cast<uint32_t>(GetCurrentProcessId())) +
                              ",\"activity\":" + BuildActivityJson(a) + "}}";
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("Discord: -> {}", payload);
  }
  return WriteFrame(1, payload);
}

void Worker(std::string client_id) {
  Activity sent;
  bool have_sent = false;
  auto last_send = Clock::now() - kMinUpdateInterval;
  auto last_connect_try = Clock::now() - kReconnectInterval;

  for (;;) {
    Activity wanted;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      // Wake for a state change, or periodically to retry a connection or to
      // release an update that was held back by the rate limit.
      g_wake.wait_for(lock, std::chrono::seconds(1), [] { return !g_running || g_dirty; });
      if (!g_running) {
        break;
      }
      wanted = g_wanted;
      g_dirty = false;
    }

    const auto now = Clock::now();
    if (g_pipe == INVALID_HANDLE_VALUE) {
      if (now - last_connect_try < kReconnectInterval) {
        continue;
      }
      last_connect_try = now;
      if (!Connect(client_id)) {
        continue;
      }
      have_sent = false;  // a fresh connection knows nothing about us
    }

    DrainReplies();
    if (g_pipe == INVALID_HANDLE_VALUE) {
      continue;
    }
    if (!g_ready) {
      // A pipe that opens but never says READY is not usable. Drop it and let
      // the reconnect timer try again rather than sending into the void.
      if (now - last_connect_try > kReconnectInterval) {
        REXLOG_WARN("Discord: no READY after handshake; reconnecting");
        Disconnect();
      }
      continue;
    }

    // Before Start's first SetDashboard lands, the wanted state is still
    // default-constructed. Sending that produced an activity of {}, which
    // Discord accepts and displays as nothing -- and then the real state was a
    // change, so it sat out the fifteen second rate limit before it could go.
    if (wanted.details.empty() && wanted.state.empty()) {
      continue;
    }
    if (have_sent && wanted == sent) {
      continue;
    }
    if (now - last_send < kMinUpdateInterval) {
      continue;  // held; the one-second wake will bring it back
    }
    if (!SendActivity(wanted)) {
      REXLOG_INFO("Discord: connection lost; will retry");
      Disconnect();
      continue;
    }
    sent = wanted;
    have_sent = true;
    last_send = now;
  }

  if (g_pipe != INVALID_HANDLE_VALUE) {
    WriteFrame(2, "{}");
    Disconnect();
  }
}

// What is being shown, in the dashboard's terms rather than Discord's.
//
// Kept apart from the Activity handed to the worker so that a scene change and
// a game launch can each update one fact without having to restate the others.
std::mutex g_state_mutex;

// The screens the user has opened, outermost first.
//
// A single "current scene" is not enough: navigating in is reported, but
// backing out is a different call and would leave the presence stuck on a
// screen that had been closed -- reading "Viewing Achievements" while the user
// was back on the dashboard. The stack makes backing out mean something.
std::vector<std::string> g_scenes;
bool g_in_game = false;
uint32_t g_title_id = 0;
std::string g_title_name;
int64_t g_session_start = 0;
int64_t g_game_start = 0;

// Scene files are named for what they show, so the name is the label.
//
// "TitleDetailsRomeScene.xur" -> "Title Details". The suffix, the "Scene" and
// the shell's internal skin names come off, then the CamelCase is split into
// words -- which means a screen nobody has hand-named still reads correctly,
// and the table below is only for the ones where the file name is not the words
// a person would use.
std::string SceneLabel(const std::string& scene_file) {
  std::string name = scene_file;
  const auto dot = name.rfind('.');
  if (dot != std::string::npos) {
    name.erase(dot);
  }
  for (const char* skin : {"Rome", "Moby", "Epix"}) {
    const auto at = name.find(skin);
    if (at != std::string::npos) {
      name.erase(at, std::strlen(skin));
    }
  }
  // "402_Achievements.xur" -- the shell numbers some scene files. The number is
  // an ordering detail, never part of the name.
  size_t digits = 0;
  while (digits < name.size() && std::isdigit(static_cast<unsigned char>(name[digits]))) {
    ++digits;
  }
  if (digits && digits < name.size() && name[digits] == '_') {
    name.erase(0, digits + 1);
  }

  if (name.size() > 5 && name.compare(name.size() - 5, 5, "Scene") == 0) {
    name.erase(name.size() - 5);
  }
  if (name.size() > 1 && name[0] == 'C' && std::isupper(static_cast<unsigned char>(name[1]))) {
    name.erase(0, 1);  // the shell's class-name convention, not a word
  }

  // Only for screens whose file name is not the words a person would use. The
  // rest fall through to the CamelCase split below and read correctly on their
  // own, which is why this table stays short.
  static const std::map<std::string, std::string> kNamed = {
      {"TitleDetails", "Looking at a game"},
      {"GameLibrary", "Browsing the Game Library"},
      {"GamerRoot", "Viewing their Gamercard"},
      {"Achievements", "Viewing Achievements"},
      {"ThemesRoot", "Choosing a Theme"},
      {"HomePage", "On the dashboard"},
      {"MarketplaceHome", "Browsing the Marketplace"},
      {"GameMarketplace", "Browsing the Game Marketplace"},
      {"VideoMarketplace", "Browsing the Video Marketplace"},
  };
  const auto named = kNamed.find(name);
  if (named != kNamed.end()) {
    return named->second;
  }

  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    if (i && std::isupper(static_cast<unsigned char>(name[i])) &&
        !std::isupper(static_cast<unsigned char>(name[i - 1]))) {
      out += ' ';
    }
    out += name[i];
  }
  return out;
}

void Publish(const Activity& a) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_running) {
      return;
    }
    g_wanted = a;
    g_dirty = true;
  }
  g_wake.notify_one();
}

// Turn the current state into what Discord should show.
void Rebuild() {
  Activity a;
  std::string scene;
  bool in_game;
  uint32_t title_id;
  std::string title_name;
  int64_t start;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    scene = g_scenes.empty() ? std::string() : g_scenes.back();
    in_game = g_in_game;
    title_id = g_title_id;
    title_name = g_title_name;
    start = in_game ? g_game_start : g_session_start;
  }

  const std::string fallback = REXCVAR_GET(discord_default_image);
  a.start_time = start;

  if (in_game) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08x", title_id);
    const std::string title = title_name.empty() ? std::string(hex) : title_name;
    a.details = "Playing " + title;
    a.state = "Xbox 360";
    // With art for the game the dashboard icon moves to the corner badge;
    // without it the dashboard icon takes the large slot so there is still a
    // picture.
    if (REXCVAR_GET(discord_game_art) && title_id) {
      a.large_image = hex;
      a.large_text = title;
      a.small_image = fallback;
      a.small_text = "NXE Dashboard";
    } else {
      a.large_image = fallback;
      a.large_text = "NXE Dashboard";
    }
  } else {
    a.details = scene.empty() ? "In the Dashboard" : scene;
    a.large_image = fallback;
    a.large_text = "NXE Dashboard";
  }
  Publish(a);
}

}  // namespace

void Start() {
  const std::string client_id = REXCVAR_GET(discord_client_id);
  if (client_id.empty()) {
    REXLOG_INFO("Discord: no discord_client_id set; rich presence is off");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running) {
      return;
    }
    g_running = true;
  }
  g_session_start = NowSeconds();
  g_thread = std::thread(Worker, client_id);
  SetDashboard();
}

void Stop() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_running) {
      return;
    }
    g_running = false;
  }
  g_wake.notify_one();
  if (g_thread.joinable()) {
    g_thread.join();
  }
}

void SetDashboard() {
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_in_game = false;
  }
  Rebuild();
}

void SetScene(const std::string& scene_file) {
  const std::string label = SceneLabel(scene_file);
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    // A game is the more interesting fact; the shell keeps navigating behind
    // it and would otherwise talk over the title being played.
    if (g_in_game || (!g_scenes.empty() && g_scenes.back() == label)) {
      return;
    }
    // A shell that navigates deeper than this is not something this dashboard
    // does; the cap only stops a mismatched push from growing without bound.
    if (g_scenes.size() >= 8) {
      g_scenes.erase(g_scenes.begin());
    }
    g_scenes.push_back(label);
  }
  Rebuild();
}

void PopScene() {
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_in_game || g_scenes.empty()) {
      return;
    }
    g_scenes.pop_back();
  }
  Rebuild();
}

void SetPlaying(uint32_t title_id, const std::string& name) {
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_in_game = true;
    g_title_id = title_id;
    g_title_name = name;
    g_game_start = NowSeconds();
  }
  Rebuild();
}

}  // namespace nxe_discord
