#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/utils/process_session.h"
#include "common/utils/process_utils.h"

namespace {

using Clock = std::chrono::steady_clock;
using vinput::process::ChildEvent;
using vinput::process::ChildEventKind;
using vinput::process::ProcessSession;

constexpr auto kEventWait = std::chrono::milliseconds(50);
// Well below the scripts' own sleeps, so a bounded failure is reported as a failure instead of the
// suite waiting for the child.
constexpr auto kTestTimeout = std::chrono::milliseconds(5000);

void ExpectTrue(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ExpectTrue failed: " << message << "\n";
    std::abort();
  }
}

std::string ToText(const std::vector<std::byte>& data) {
  std::string text;
  text.reserve(data.size());
  for (const std::byte value : data) {
    text.push_back(static_cast<char>(value));
  }
  return text;
}

ProcessSession MustStart(const std::string& script) {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", script};
  auto session = ProcessSession::Start(spec);
  ExpectTrue(session.has_value(), "ProcessSession::Start should succeed");
  return std::move(*session);
}

// Drains events until the child is reaped, so a session that never finishes fails the test instead
// of hanging the suite.
ChildEvent AwaitExit(ProcessSession& session, std::chrono::milliseconds timeout) {
  const Clock::time_point deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    ChildEvent event;
    if (!session.NextEvent(event, kEventWait)) {
      continue;
    }
    if (event.kind == ChildEventKind::Exited) {
      return event;
    }
  }
  ExpectTrue(false, "the session should report Exited within the timeout");
  return {};
}

// Drains whatever arrives until the child exits, so a test can look at the whole conversation.
std::string DrainUntilExit(ProcessSession& session, ChildEvent* terminal,
                           std::chrono::milliseconds timeout) {
  std::string stdout_text;
  const Clock::time_point deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    ChildEvent event;
    if (!session.NextEvent(event, kEventWait)) {
      continue;
    }
    if (event.kind == ChildEventKind::StdoutChunk) {
      stdout_text.append(ToText(event.data));
    }
    if (event.kind == ChildEventKind::Exited) {
      if (terminal != nullptr) {
        *terminal = std::move(event);
      }
      break;
    }
  }
  return stdout_text;
}

std::span<const std::byte> AsBytes(const std::string& text) {
  return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

/**
 * The session is a conversation: what the caller writes reaches the child, and what the child
 * answers comes back as events in the order it was produced.
 */
void TestRoundTrip() {
  auto session = MustStart(R"(while IFS= read -r line; do printf 'ack:%s\n' "$line"; done)");

  const std::string payload = "hello_session\n";
  ExpectTrue(session.PushStdin(AsBytes(payload)), "the session should accept stdin");

  std::string stdout_text;
  const Clock::time_point deadline = Clock::now() + kTestTimeout;
  while (stdout_text.find("ack:hello_session") == std::string::npos && Clock::now() < deadline) {
    ChildEvent event;
    if (!session.NextEvent(event, kEventWait)) {
      continue;
    }
    if (event.kind == ChildEventKind::StdoutChunk) {
      stdout_text.append(ToText(event.data));
    }
    if (event.kind == ChildEventKind::Exited) {
      break;
    }
  }
  ExpectTrue(stdout_text.find("ack:hello_session") != std::string::npos,
             "the child's answer should come back as stdout events");

  session.CloseStdin();
  const ChildEvent exit = AwaitExit(session, kTestTimeout);
  ExpectTrue(exit.exit_code == 0, "a child that finished on its own should exit 0");
  ExpectTrue(!exit.killed_by_us, "a child that finished on its own was not killed by the session");
}

/**
 * Speech recognition runs the other way round: the provider talks on its own schedule, so output
 * has to be observed even while the caller pushes no audio at all.
 */
void TestOutputArrivesWithoutStdin() {
  auto session = MustStart("for i in 1 2 3; do echo tick$i; sleep 0.1; done");

  const std::string stdout_text = DrainUntilExit(session, nullptr, kTestTimeout);
  ExpectTrue(stdout_text.find("tick1") != std::string::npos,
             "the first line the child printed on its own should be delivered");
  ExpectTrue(stdout_text.find("tick3") != std::string::npos,
             "every line the child printed on its own should be delivered");
}

/**
 * Cancelling has to take effect while the caller is waiting for output, and it has to be reported
 * as something this session caused rather than as a provider failure.
 */
void TestCancelInterruptsBlockedWait() {
  auto session = MustStart("echo ready; sleep 30");

  std::string stdout_text;
  const Clock::time_point ready_deadline = Clock::now() + kTestTimeout;
  while (stdout_text.find("ready") == std::string::npos && Clock::now() < ready_deadline) {
    ChildEvent event;
    if (!session.NextEvent(event, kEventWait)) {
      continue;
    }
    if (event.kind == ChildEventKind::StdoutChunk) {
      stdout_text.append(ToText(event.data));
    }
  }
  ExpectTrue(stdout_text.find("ready") != std::string::npos, "the child should announce itself");

  const Clock::time_point cancel_start = Clock::now();
  session.Cancel();
  const ChildEvent exit = AwaitExit(session, kTestTimeout);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - cancel_start);
  ExpectTrue(elapsed < std::chrono::milliseconds(2000),
             "cancelling must end the session long before the child's own sleep does (elapsed=" +
                 std::to_string(elapsed.count()) + "ms)");
  ExpectTrue(exit.killed_by_us, "a cancelled session must report that it killed the child");
}

/**
 * A provider script is usually a wrapper: cancelling only its pid would leave the helpers it forked
 * running with the session's pipes still open.
 */
void TestCancelTerminatesTheProcessGroup() {
  auto session = MustStart("sleep 30 & echo $!; wait");

  std::string stdout_text;
  const Clock::time_point deadline = Clock::now() + kTestTimeout;
  while (stdout_text.find('\n') == std::string::npos && Clock::now() < deadline) {
    ChildEvent event;
    if (!session.NextEvent(event, kEventWait)) {
      continue;
    }
    if (event.kind == ChildEventKind::StdoutChunk) {
      stdout_text.append(ToText(event.data));
    }
  }
  const std::size_t newline = stdout_text.find('\n');
  ExpectTrue(newline != std::string::npos, "the child should report its helper's pid");
  const int helper_pid = std::atoi(stdout_text.substr(0, newline).c_str());
  ExpectTrue(helper_pid > 0, "the reported helper pid should be usable");

  const std::uint64_t helper_start_time = vinput::process::ProcessStartTime(helper_pid);
  ExpectTrue(helper_start_time != 0, "the helper should be running before the cancel");

  session.Cancel();
  (void)AwaitExit(session, kTestTimeout);

  bool helper_gone = false;
  const Clock::time_point gone_deadline = Clock::now() + std::chrono::milliseconds(2000);
  while (Clock::now() < gone_deadline) {
    if (!vinput::process::ProcessIdentityMatches(helper_pid, helper_start_time)) {
      helper_gone = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ExpectTrue(helper_gone, "cancelling must not leave a helper the provider forked behind");
}

/**
 * Closing stdin is an explicit end of input: the child must see every byte handed over before it,
 * and then EOF.
 */
void TestCloseStdinDeliversQueuedBytesBeforeEof() {
  auto session = MustStart("cat > /dev/null; echo done_after_eof");

  const std::string payload = "payload_without_newline";
  ExpectTrue(session.PushStdin(AsBytes(payload)), "the session should accept stdin");
  session.CloseStdin();
  ExpectTrue(!session.PushStdin(AsBytes(payload)), "a closed session must refuse further stdin");

  const std::string stdout_text = DrainUntilExit(session, nullptr, kTestTimeout);
  ExpectTrue(stdout_text.find("done_after_eof") != std::string::npos,
             "the child should reach EOF only after the queued bytes were written");
}

/**
 * The destructor is the last owner of the child: it has to stop it and reap it, because a zombie
 * out lives the session otherwise.
 */
void TestDestructorReapsTheChild() {
  int child_pid = -1;
  {
    auto session = MustStart("sleep 30");
    child_pid = session.Pid();
    ExpectTrue(child_pid > 0, "the session should report a usable pid");
    ExpectTrue(vinput::process::ProcessStartTime(child_pid) != 0, "the child should be running");
  }
  // A reaped child leaves no /proc entry behind; a zombie would still be listed there.
  ExpectTrue(vinput::process::ProcessStartTime(child_pid) == 0,
             "the destructor must reap the child instead of leaving a zombie");
}

} // namespace

int main() {
  std::cout << "Running test_process_session...\n";
  TestRoundTrip();
  TestOutputArrivesWithoutStdin();
  TestCancelInterruptsBlockedWait();
  TestCancelTerminatesTheProcessGroup();
  TestCloseStdinDeliversQueuedBytesBeforeEof();
  TestDestructorReapsTheChild();
  std::cout << "All test_process_session passed!\n";
  return 0;
}
