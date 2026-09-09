#include "tipe/learning.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;
using tipe::Event;
using tipe::EventType;

int main() {
  const std::string dir = "/tmp/tipe-learning-test";
  std::filesystem::remove_all(dir);
  // Repeated adjacent candidate navigation is telemetry noise.  It is
  // coalesced inside the worker batch, while selection and commit events keep
  // their order and still form one complete phrase-learning chain.
  {
    const std::string mdir = "/tmp/tipe-learning-candidate-moves";
    std::filesystem::remove_all(mdir);
    tipe::Learner l(mdir);
    // Make the per-event test delay visible: with coalescing only three move
    // records reach the worker (two runs plus the boundary), while the raw
    // burst would take several seconds.
    ::setenv("TIPE_LEARNER_TEST_BATCH_DELAY_US", "100000", 1);
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 64; ++i) {
      Event move;
      move.type = EventType::CandidateMove;
      move.key = "Down";
      move.session = 41;
      move.context = "editor";
      l.push(move);
    }
    // A different move key and a non-move event are boundaries: coalescing
    // must never cross either one.
    Event turn;
    turn.type = EventType::CandidateMove;
    turn.key = "Up";
    turn.session = 41;
    turn.context = "editor";
    l.push(turn);
    Event first;
    first.type = EventType::CandidateSelect;
    first.pinyin = "ni";
    first.committed = "你";
    first.remaining = "hao";
    first.session = 41;
    first.context = "editor";
    l.push(first);
    for (int i = 0; i < 32; ++i) {
      Event move;
      move.type = EventType::CandidateMove;
      move.key = "Up";
      move.session = 41;
      move.context = "editor";
      l.push(move);
    }
    Event second;
    second.type = EventType::CandidateSelect;
    second.pinyin = "hao";
    second.committed = "好";
    second.session = 41;
    second.context = "editor";
    l.push(second);
    assert(l.wait_until_idle(3s));
    auto model = l.model();
    assert(model && model->entries.size() == 1);
    assert(model->entries[0].pinyin == "ni'hao");
    assert(model->entries[0].text == "你好");
    assert(l.publish_count() == 1);
    assert(std::chrono::steady_clock::now() - started < 1500ms);
    ::unsetenv("TIPE_LEARNER_TEST_BATCH_DELAY_US");
  }
  {
    tipe::Learner l(dir);
    Event a; a.type=EventType::CandidateSelect; a.pinyin="ni";
    a.committed="你"; a.remaining="hao"; a.session=7; l.push(a);
    Event b; b.type=EventType::CandidateSelect; b.pinyin="hao";
    b.committed="好"; b.session=7; l.push(b);
    assert(l.wait_until_idle(2s));
    auto m=l.model(); assert(m && m->entries.size()==1);
    assert(m->entries[0].pinyin=="ni'hao" && m->entries[0].text=="你好");
    assert(l.preference("你好")==1);
    for (int i = 0; i < 2; ++i) {
      Event repeat; repeat.type = EventType::Commit; repeat.pinyin = "ni'hao";
      repeat.committed = "你好"; repeat.context = ""; l.push(repeat);
    }
    assert(l.wait_until_idle(2s));
    auto exact = l.exact_entries("nihao", "");
    assert(!exact.empty() && exact.front().text == "你好" && exact.front().count >= 3);
    assert(l.pause(true)); Event ignored; ignored.type=EventType::Commit;
    ignored.pinyin="shi"; ignored.committed="是"; l.push(ignored);
    std::this_thread::sleep_for(150ms); assert(l.preference("是")==0);
    assert(l.pause(false));
    assert(l.set_enabled(false));
    assert(!l.enabled());
    Event disabled; disabled.type=EventType::Commit; disabled.pinyin="guan";
    disabled.committed="关"; l.push(disabled);
    assert(l.observed() >= 2); // pre-existing observations remain visible
    std::this_thread::sleep_for(150ms);
    assert(l.preference("关")==0);
    assert(l.set_enabled(true));
    assert(l.enabled());
    Event secret; secret.type=EventType::Commit; secret.pinyin="mi";
    secret.committed="秘密"; secret.sensitive=true; l.push(secret);
    assert(l.wait_until_idle(2s)); assert(l.preference("秘密")==0);
    const std::string out=dir+"/export.tsv"; assert(l.export_data(out));
    assert(l.clear()); assert(l.model()->entries.empty()); assert(l.preference("你好")==0);
    // Exports are private user data and are published atomically.
    struct stat export_stat{};
    assert(::stat(out.c_str(), &export_stat) == 0);
    assert((export_stat.st_mode & 0777) == 0600);
    assert(!std::filesystem::exists(out + ".tmp"));
  }

  // Control commands from several read-only control-center instances must
  // serialize through control.lock instead of losing revisions.  With no
  // automatic writer alive, each command temporarily acquires writer.lock
  // and acknowledges the request itself.
  {
    const std::string cdir = "/tmp/tipe-learning-control-concurrency";
    std::filesystem::remove_all(cdir);
    constexpr int kThreads = 4;
    constexpr int kCommands = 5;
    std::vector<std::thread> ts;
    std::atomic<int> done{0};
    for (int i = 0; i < kThreads; ++i) {
      ts.emplace_back([&, i] {
        tipe::Learner ctl(cdir, tipe::LearnerMode::Control);
        for (int j = 0; j < kCommands; ++j) {
          // Alternate pause state; all calls must complete even though every
          // instance starts without a worker thread of its own.
          ctl.pause(((i + j) & 1) != 0);
        }
        ++done;
      });
    }
    for (auto& t : ts) t.join();
    assert(done == kThreads);
    std::ifstream control(cdir + "/control.current");
    std::string line;
    assert(std::getline(control, line));
    // Every command gets a distinct revision under control.lock.
    assert(line.rfind("#tipe-control-v1\t" +
                      std::to_string(kThreads * kCommands) + "\t", 0) == 0);
    tipe::Learner observer(cdir, tipe::LearnerMode::Control);
    assert(observer.status().size() > 0);
  }

  // Clear and rollback are safe while the worker is receiving a burst.  Once
  // the producer stops, clear must leave an empty model and a subsequent
  // rollback must report failure (the clear intentionally removes the
  // previous generation).
  {
    const std::string idir = "/tmp/tipe-learning-interleaved";
    std::filesystem::remove_all(idir);
    tipe::Learner l(idir);
    Event seed; seed.type = EventType::Commit; seed.pinyin = "seed";
    seed.committed = "种"; l.push(seed);
    assert(l.wait_until_idle(2s));
    { std::ofstream stale(idir + "/model.previous.tmp"); stale << "stale"; }
    // A large batch keeps the worker busy while the control command is
    // published, exercising the generation hand-off at a real publication
    // boundary without allowing events to arrive after clear.
    for (int n = 0; n < 1000; ++n) {
      Event e; e.type = EventType::Commit; e.pinyin = "burst" + std::to_string(n);
      e.committed = "词"; l.push(std::move(e));
    }
    std::this_thread::sleep_for(45ms);
    l.clear();
    assert(l.wait_until_idle(3s));
    assert(l.model()->entries.empty());
    assert(l.preference("种") == 0);
    assert(!std::filesystem::exists(idir + "/model.previous.tmp"));
    // The command protocol remains usable after the interleaving.
    assert(l.rollback() == false);
  }

  // Pausing during a queued burst cancels the in-flight batch transactionally;
  // no partial counts may become visible while the learner is paused.
  {
    const std::string pdir = "/tmp/tipe-learning-pause-cancel";
    std::filesystem::remove_all(pdir);
    tipe::Learner l(pdir);
    for (int n = 0; n < 2048; ++n) {
      Event e; e.type = EventType::Commit; e.pinyin = "cancel";
      e.committed = "取消"; l.push(std::move(e));
    }
    l.pause(true);
    assert(l.paused());
    std::this_thread::sleep_for(150ms);
    const auto paused_count = l.preference("取消");
    // Whether the worker had already committed the batch before the pause
    // request is timing dependent; the important invariant is that pausing
    // never publishes a partial batch and blocks subsequent input.
    Event blocked; blocked.type = EventType::Commit; blocked.pinyin = "blocked";
    blocked.committed = "阻塞"; l.push(blocked);
    std::this_thread::sleep_for(150ms);
    assert(l.preference("阻塞") == 0);
    l.pause(false);
    assert(l.wait_until_idle(3s));
    assert(l.preference("取消") >= paused_count);
  }

  // A separate control-center instance must be able to cancel an in-flight
  // writer batch. The delay is test-only and never enabled in production.
  {
    const std::string xdir = "/tmp/tipe-learning-cross-process-cancel";
    std::filesystem::remove_all(xdir);
    ::setenv("TIPE_LEARNER_TEST_BATCH_DELAY_US", "1000", 1);
    tipe::Learner writer(xdir);
    tipe::Learner control(xdir, tipe::LearnerMode::Control);
    for (int n = 0; n < 128; ++n) {
      Event e; e.type = EventType::Commit;
      e.pinyin = "cross" + std::to_string(n);
      e.committed = "跨"; writer.push(std::move(e));
    }
    std::this_thread::sleep_for(70ms);
    control.pause(true);
    assert(control.paused());
    assert(writer.model()->entries.empty());
    control.pause(false);
    assert(writer.wait_until_idle(5s));
    assert(writer.preference("跨") > 0);
    ::unsetenv("TIPE_LEARNER_TEST_BATCH_DELAY_US");
  }

  // Sustained input remains bounded even when every event has a distinct
  // spelling: the queue, published model and on-disk generation stay capped.
  {
    const std::string sdir = "/tmp/tipe-learning-resource-bound";
    std::filesystem::remove_all(sdir);
    tipe::Learner l(sdir);
    for (int n = 0; n < 12000; ++n) {
      Event e; e.type = EventType::Commit;
      e.pinyin = "bounded" + std::to_string(n);
      e.committed = "词"; l.push(std::move(e));
    }
    assert(l.queue_depth() <= 2048);
    assert(l.dropped_events() > 0);
    assert(l.wait_until_idle(8s));
    assert(l.model()->entries.size() <= 4096);
    assert(l.disk_bytes() < 8 * 1024 * 1024);
    // disk_bytes() reports current on-disk usage, rather than an ever-
    // increasing sum of every historical publication.
    uint64_t actual_bytes = 0;
    std::error_code size_error;
    for (const auto &entry : std::filesystem::directory_iterator(sdir, size_error)) {
      if (size_error) break;
      if (entry.is_regular_file(size_error))
        actual_bytes += entry.file_size(size_error);
    }
    assert(l.disk_bytes() == actual_bytes);
  }

  // A crash between writing and renaming leaves only allowlisted temporary
  // files.  The next writer startup removes those files before loading the
  // durable current/previous generation, so they cannot accumulate forever.
  {
    const std::string cdir = "/tmp/tipe-learning-stale-temporaries";
    std::filesystem::remove_all(cdir);
    std::filesystem::create_directories(cdir);
    for (const char *name : {"model.current.tmp", "model.previous.tmp",
                             "state.current.tmp", "control.current.tmp"}) {
      std::ofstream stale(cdir + "/" + name);
      stale << "stale";
    }
    tipe::Learner l(cdir);
    l.wait_until_idle(100ms);
    for (const char *name : {"model.current.tmp", "model.previous.tmp",
                             "state.current.tmp", "control.current.tmp"})
      assert(!std::filesystem::exists(cdir + "/" + name));
  }

  // Process-level recovery: an input worker can disappear between batches
  // without running its destructor.  Atomic publication must leave the next
  // writer with a readable current/previous generation and a usable lock.
  {
    const std::string crash_dir = "/tmp/tipe-learning-process-recovery";
    std::filesystem::remove_all(crash_dir);
    {
      tipe::Learner seed(crash_dir);
      Event e; e.type = EventType::Commit; e.pinyin = "seed"; e.committed = "种";
      seed.push(e);
      assert(seed.wait_until_idle(2s));
    }
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
      ::setenv("TIPE_LEARNER_TEST_BATCH_DELAY_US", "50000", 1);
      tipe::Learner dying(crash_dir);
      for (int i = 0; i < 128; ++i) {
        Event e; e.type = EventType::Commit;
        e.pinyin = "crash" + std::to_string(i); e.committed = "崩";
        dying.push(std::move(e));
      }
      std::this_thread::sleep_for(80ms);
      ::_exit(0); // deliberately skip Learner::~Learner and worker join
    }
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ::unsetenv("TIPE_LEARNER_TEST_BATCH_DELAY_US");
    tipe::Learner recovered(crash_dir);
    assert(recovered.preference("种") == 1);
    Event after; after.type = EventType::Commit;
    after.pinyin = "recovered"; after.committed = "复";
    recovered.push(after);
    assert(recovered.wait_until_idle(4s));
    assert(recovered.preference("复") == 1);
    tipe::Learner observer(crash_dir, tipe::LearnerMode::Control);
    assert(observer.preference("种") == 1 && observer.preference("复") == 1);
    for (const char *name : {"model.current.tmp", "model.previous.tmp",
                             "state.current.tmp", "control.current.tmp"})
      assert(!std::filesystem::exists(crash_dir + "/" + name));
  }

  // The cancellation protocol also has to work across an operating-system
  // process boundary.  Keep the writer's worker in one child and issue the
  // pause/resume controls from a separate short-lived control process.  This
  // catches accidental reliance on in-process atomics or shared C++ state.
  {
    const std::string ipc_dir = "/tmp/tipe-learning-process-cancel";
    std::filesystem::remove_all(ipc_dir);
    int ready_pipe[2]{};
    int done_pipe[2]{};
    assert(::pipe(ready_pipe) == 0 && ::pipe(done_pipe) == 0);
    const pid_t writer_pid = ::fork();
    assert(writer_pid >= 0);
    if (writer_pid == 0) {
      ::close(ready_pipe[0]);
      ::close(done_pipe[0]);
      ::setenv("TIPE_LEARNER_TEST_BATCH_DELAY_US", "5000", 1);
      tipe::Learner writer(ipc_dir);
      for (int i = 0; i < 512; ++i) {
        Event e; e.type = EventType::Commit;
        e.pinyin = "ipc" + std::to_string(i); e.committed = "跨";
        writer.push(std::move(e));
      }
      // Let the worker leave its 40 ms batching window and enter the batch
      // before the independent controller asks it to pause.
      std::this_thread::sleep_for(120ms);
      const char marker = 'R';
      (void)::write(ready_pipe[1], &marker, 1);
      const bool drained = writer.wait_until_idle(15s);
      const char result = drained ? '1' : '0';
      (void)::write(done_pipe[1], &result, 1);
      ::close(ready_pipe[1]);
      ::close(done_pipe[1]);
      ::_exit(drained ? 0 : 3);
    }
    ::close(ready_pipe[1]);
    ::close(done_pipe[1]);
    char marker = 0;
    assert(::read(ready_pipe[0], &marker, 1) == 1 && marker == 'R');
    ::close(ready_pipe[0]);

    auto run_control = [&](bool pause) {
      int result_pipe[2]{};
      assert(::pipe(result_pipe) == 0);
      const pid_t control_pid = ::fork();
      assert(control_pid >= 0);
      if (control_pid == 0) {
        ::close(result_pipe[0]);
        tipe::Learner control(ipc_dir, tipe::LearnerMode::Control);
        const bool ok = control.pause(pause);
        const char result = ok ? '1' : '0';
        (void)::write(result_pipe[1], &result, 1);
        ::close(result_pipe[1]);
        ::_exit(ok ? 0 : 4);
      }
      ::close(result_pipe[1]);
      char result = 0;
      assert(::read(result_pipe[0], &result, 1) == 1 && result == '1');
      ::close(result_pipe[0]);
      int control_status = 0;
      assert(::waitpid(control_pid, &control_status, 0) == control_pid);
      assert(WIFEXITED(control_status) && WEXITSTATUS(control_status) == 0);
    };

    run_control(true);
    // The pause acknowledgement is written only after the worker has rolled
    // its current batch back.  A reader must therefore never observe a
    // partially trained model from this cancellation boundary.
    tipe::Learner paused(ipc_dir, tipe::LearnerMode::Control);
    assert(paused.paused());
    assert(paused.model()->entries.empty());
    run_control(false);

    char done = 0;
    assert(::read(done_pipe[0], &done, 1) == 1 && done == '1');
    ::close(done_pipe[0]);
    int writer_status = 0;
    assert(::waitpid(writer_pid, &writer_status, 0) == writer_pid);
    assert(WIFEXITED(writer_status) && WEXITSTATUS(writer_status) == 0);
    tipe::Learner resumed(ipc_dir, tipe::LearnerMode::Control);
    assert(!resumed.paused());
    assert(resumed.preference("跨") > 0);
  }

  // If both generations are damaged, a control-only reader must fail closed
  // to an empty snapshot rather than exposing partially parsed data.
  {
    const std::string ddir = "/tmp/tipe-learning-damaged";
    std::filesystem::remove_all(ddir);
    std::filesystem::create_directories(ddir);
    {
      std::ofstream cur(ddir + "/model.current");
      cur << "#tipe-v1\t7\t0\ntruncated";
      std::ofstream prev(ddir + "/model.previous");
      prev << "not-a-model\n";
    }
    tipe::Learner damaged(ddir, tipe::LearnerMode::Control);
    assert(damaged.model()->entries.empty());
    assert(damaged.preference("任何") == 0);
  }
  // A failed write is retried automatically, without publishing an empty or
  // partial generation or counting the same event twice.
  {
    const std::string fdir = "/tmp/tipe-learning-write-failure";
    std::filesystem::remove_all(fdir);
    tipe::Learner l(fdir);
    Event first; first.type=EventType::Commit; first.pinyin="jia";
    first.committed="甲"; l.push(first);
    assert(l.wait_until_idle(2s));
    std::filesystem::create_directory(fdir + "/model.current.tmp");
    Event second; second.type=EventType::Commit; second.pinyin="yi";
    second.committed="乙"; l.push(second);
    assert(!l.wait_until_idle(120ms));
    assert(l.preference("甲") == 1);
    assert(l.preference("乙") == 0);
    std::filesystem::remove(fdir + "/model.current.tmp");
    assert(l.wait_until_idle(3s));
    assert(l.preference("甲") == 1);
    assert(l.preference("乙") == 1);
    // A corrupt rollback target is rejected before changing current.
    { std::ofstream bad(fdir + "/model.previous", std::ios::trunc); bad << "broken\n"; }
    assert(!l.rollback());
    assert(l.preference("甲") == 1 && l.preference("乙") == 1);
    tipe::Learner reader(fdir, tipe::LearnerMode::Control);
    assert(reader.preference("甲") == 1 && reader.preference("乙") == 1);
  }
  {
    tipe::Learner l(dir); assert(l.model()->entries.empty());
  }
  // Two successful publications leave a usable previous generation; rollback
  // must atomically restore it and keep the in-memory snapshot in sync.
  {
    const std::string rdir = "/tmp/tipe-learning-rollback";
    std::filesystem::remove_all(rdir);
    tipe::Learner l(rdir);
    Event first; first.type=EventType::Commit; first.pinyin="alpha"; first.committed="甲"; l.push(first);
    assert(l.wait_until_idle(2s));
    assert(l.preference("甲") == 1);
    Event second; second.type=EventType::Commit; second.pinyin="beta"; second.committed="乙"; l.push(second);
    assert(l.wait_until_idle(2s));
    assert(l.preference("乙") == 1);
    assert(l.rollback());
    assert(l.preference("甲") == 1);
    assert(l.preference("乙") == 0);
    { std::ofstream bad(rdir + "/model.current", std::ios::trunc); bad << "corrupt\n"; }
    tipe::Learner recovered(rdir, tipe::LearnerMode::Control);
    assert(recovered.preference("甲") == 1);
    assert(recovered.preference("乙") == 0);
    // Generation files are transient implementation details; only current
    // and previous are retained so repeated learning cannot grow storage
    // without bound.
    size_t generation_files = 0;
    for (auto const& ent : std::filesystem::directory_iterator(rdir))
      if (ent.path().filename().string().rfind("model-", 0) == 0)
        ++generation_files;
    assert(generation_files == 0);
  }
  // Rebuilding after a corrupt current file must preserve the valid
  // previous generation so rollback remains possible.
  {
    const std::string rdir = "/tmp/tipe-learning-rollback-rebuild";
    std::filesystem::remove_all(rdir);
    tipe::Learner seed(rdir);
    Event first; first.type=EventType::Commit; first.pinyin="alpha"; first.committed="甲"; seed.push(first);
    assert(seed.wait_until_idle(2s));
    Event second; second.type=EventType::Commit; second.pinyin="beta"; second.committed="乙"; seed.push(second);
    assert(seed.wait_until_idle(2s));
    { std::ofstream bad(rdir + "/model.current", std::ios::trunc); bad << "broken\n"; }
  }
  {
    tipe::Learner rebuilt("/tmp/tipe-learning-rollback-rebuild");
    assert(rebuilt.preference("甲") == 1);
    Event extra; extra.type=EventType::Commit; extra.pinyin="gamma"; extra.committed="丙"; rebuilt.push(extra);
    assert(rebuilt.wait_until_idle(2s));
    assert(rebuilt.preference("丙") == 1);
    assert(rebuilt.rollback());
    assert(rebuilt.preference("甲") == 1);
    assert(rebuilt.preference("丙") == 0);
  }
  // A second process (for example, the control center) observes counters
  // published by the fcitx learner through the shared state file.
  {
    const std::string shared = "/tmp/tipe-learning-control-observer";
    std::filesystem::remove_all(shared);
    tipe::Learner fcitx(shared);
    Event e; e.type=EventType::Commit; e.pinyin="ni"; e.committed="你"; fcitx.push(e);
    assert(fcitx.wait_until_idle(2s));
    tipe::Learner control(shared, tipe::LearnerMode::Control);
    assert(control.observed() >= 1);
    assert(control.learned() >= 1);
    assert(control.set_enabled(false));
    assert(!control.enabled());
    Event blocked; blocked.type=EventType::Commit; blocked.pinyin="bu"; blocked.committed="不";
    fcitx.push(blocked);
    std::this_thread::sleep_for(250ms);
    assert(fcitx.preference("不")==0);
    assert(control.set_enabled(true));
    assert(control.enabled());
    assert(control.pause(true));
    Event queued; queued.type=EventType::Commit; queued.pinyin="hao"; queued.committed="好"; control.push(queued);
    assert(control.clear());
    assert(control.model()->entries.empty());
    assert(control.observed()==0);
  }

  // The control-center snapshot follows each committed publication and then
  // converges to the acknowledged clear.  A failed clear reports the retry
  // state while preserving the previous durable model; once the write fault
  // is removed, the same request is retried and the observer converges to an
  // empty model.  This exercises the state.current contract used by the Qt UI
  // instead of relying on an in-process snapshot alone.
  {
    const std::string sdir = "/tmp/tipe-learning-control-consistency";
    std::filesystem::remove_all(sdir);
    tipe::Learner writer(sdir);
    tipe::Learner control(sdir, tipe::LearnerMode::Control);
    Event first; first.type = EventType::Commit; first.pinyin = "first";
    first.committed = "首"; writer.push(first);
    assert(writer.wait_until_idle(2s));
    assert(control.learned() == 1 && control.observed() >= 1);
    Event second; second.type = EventType::Commit; second.pinyin = "second";
    second.committed = "次"; writer.push(second);
    assert(writer.wait_until_idle(2s));
    assert(control.learned() == 2 && control.observed() >= 2);

    // Make the next forced empty publication fail.  The old current model is
    // kept intact and the command returns false after its bounded wait.
    std::filesystem::create_directory(sdir + "/model.current.tmp");
    assert(!control.clear());
    assert(control.status() == "下次自动重试");
    assert(control.learned() == 2);
    std::filesystem::remove(sdir + "/model.current.tmp");
    assert(control.clear());
    assert(control.learned() == 0);
    assert(control.observed() == 0);
    assert(control.status() == "等待输入");
    // A long-lived observer must resume from the new zero baseline when the
    // writer publishes input after a clear from another instance.
    Event after_clear; after_clear.type = EventType::Commit;
    after_clear.pinyin = "after"; after_clear.committed = "后";
    writer.push(after_clear);
    assert(writer.wait_until_idle(2s));
    assert(control.learned() == 1 && control.observed() >= 1);
    assert(control.clear());
    assert(control.learned() == 0 && control.observed() == 0);
    // With no previous generation after the final clear, rollback is
    // acknowledged as a failed operation and must leave the empty state.
    assert(!control.rollback());
    assert(control.learned() == 0 && control.model()->entries.empty());
  }
  // A clear floor is a transition marker, not a permanent override.  The
  // same control instance and a long-lived second observer must both see new
  // writer input after a clear issued by the first instance.
  {
    const std::string sdir = "/tmp/tipe-learning-clear-observer-lifetime";
    std::filesystem::remove_all(sdir);
    tipe::Learner writer(sdir);
    tipe::Learner control(sdir, tipe::LearnerMode::Control);
    tipe::Learner watcher(sdir, tipe::LearnerMode::Control);
    Event before; before.type = EventType::Commit; before.pinyin = "before";
    before.committed = "前"; writer.push(before);
    assert(writer.wait_until_idle(2s));
    assert(control.status_snapshot().learned == 1);
    assert(watcher.status_snapshot().observed >= 1);
    assert(control.clear());
    auto cleared = control.status_snapshot();
    assert(cleared.learned == 0 && cleared.observed == 0);
    auto watched_clear = watcher.status_snapshot();
    assert(watched_clear.learned == 0 && watched_clear.observed == 0);
    Event after; after.type = EventType::Commit; after.pinyin = "after";
    after.committed = "后"; writer.push(after);
    assert(writer.wait_until_idle(2s));
    auto control_after = control.status_snapshot();
    auto watcher_after = watcher.status_snapshot();
    assert(control_after.learned == 1 && control_after.observed >= 1);
    assert(watcher_after.learned == 1 && watcher_after.observed >= 1);
  }
  // Repeated deletion evidence is kept in a separate, bounded correction
  // layer. It generalizes only within the same application context and is
  // restored from the atomically published model.
  {
    const std::string cdir = "/tmp/tipe-learning-correction";
    std::filesystem::remove_all(cdir);
    tipe::Learner l(cdir);
    for (int repeat = 0; repeat < 2; ++repeat) {
      Event a; a.type=EventType::Commit; a.pinyin="nihao"; a.committed="你好";
      a.corrected_from="ihao"; a.context="editor"; l.push(a);
      Event b; b.type=EventType::Commit; b.pinyin="nimen"; b.committed="你们";
      b.corrected_from="imen"; b.context="editor"; l.push(b);
      Event c; c.type=EventType::Commit; c.pinyin="nizao"; c.committed="你早";
      c.corrected_from="izao"; c.context="editor"; l.push(c);
    }
    assert(l.wait_until_idle(3s));
    const auto generated = l.correction_candidates("izai", "editor");
    assert(std::find(generated.begin(), generated.end(), "nizai") != generated.end());
    assert(l.correction_candidates("izai", "terminal").empty());
    tipe::Learner reader(cdir, tipe::LearnerMode::Control);
    const auto restored = reader.correction_candidates("izai", "editor");
    assert(std::find(restored.begin(), restored.end(), "nizai") != restored.end());
    reader.clear();
    assert(reader.correction_candidates("izai", "editor").empty());
  }
}
