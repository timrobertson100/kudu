// Copyright (c) 2014, Cloudera, inc.
//
// Copied from Impala. Changes include:
// - Namespace + imports.
// - Adapted to Kudu metrics library.
// - Removal of ThreadGroups.
// - Switched from promise to spinlock in SuperviseThread to RunThread
//   communication.
// - Fixes for cpplint.
// - Added spinlock for protection against KUDU-11.

#include "util/thread.h"

#include <boost/foreach.hpp>
#include <map>
#include <set>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "gutil/atomicops.h"
#include "server/webserver.h"
#include "util/debug-util.h"
#include "util/errno.h"
#include "util/metrics.h"
#include "util/url-coding.h"
#include "util/os-util.h"

using boost::bind;
using boost::lock_guard;
using boost::mem_fn;
using boost::mutex;
using boost::shared_ptr;
using boost::thread;
using std::endl;
using std::map;
using std::stringstream;

namespace kudu {

class ThreadMgr;

METRIC_DEFINE_gauge_uint64(total_threads, MetricUnit::kThreads,
                           "All time total number of threads");
METRIC_DEFINE_gauge_uint64(current_num_threads, MetricUnit::kThreads,
                           "Current number of running threads");

// Singleton instance of ThreadMgr. Only visible in this file, used only by Thread.
// The Thread class adds a reference to thread_manager while it is supervising a thread so
// that a race between the end of the process's main thread (and therefore the destruction
// of thread_manager) and the end of a thread that tries to remove itself from the
// manager after the destruction can be avoided.
shared_ptr<ThreadMgr> thread_manager;

// A singleton class that tracks all live threads, and groups them together for easy
// auditing. Used only by Thread.
class ThreadMgr {
 public:
  ThreadMgr() : metrics_enabled_(false) { }

  Status StartInstrumentation(MetricRegistry* registry, Webserver* webserver);

  // Registers a thread to the supplied category. The key is a boost::thread::id, used
  // instead of the system TID since boost::thread::id is always available, unlike
  // gettid() which might fail.
  void AddThread(const thread::id& thread, const string& name, const string& category,
      int64_t tid);

  // Removes a thread from the supplied category. If the thread has
  // already been removed, this is a no-op.
  void RemoveThread(const thread::id& boost_id, const string& category);

 private:
  // Container class for any details we want to capture about a thread
  // TODO: Add start-time.
  // TODO: Track fragment ID.
  class ThreadDescriptor {
   public:
    ThreadDescriptor() { }
    ThreadDescriptor(const string& category, const string& name, int64_t thread_id)
        : name_(name), category_(category), thread_id_(thread_id) {
    }

    const string& name() const { return name_; }
    const string& category() const { return category_; }
    int64_t thread_id() const { return thread_id_; }

   private:
    string name_;
    string category_;
    int64_t thread_id_;
  };

  // A ThreadCategory is a set of threads that are logically related.
  // TODO: unordered_map is incompatible with boost::thread::id, but would be more
  // efficient here.
  typedef map<const thread::id, ThreadDescriptor> ThreadCategory;

  // All thread categorys, keyed on the category name.
  typedef map<string, ThreadCategory> ThreadCategoryMap;

  // Protects thread_categories_ and metrics_enabled_
  mutex lock_;

  // All thread categorys that ever contained a thread, even if empty
  ThreadCategoryMap thread_categories_;

  // True after StartInstrumentation(..) returns
  bool metrics_enabled_;

  // Counters to track all-time total number of threads, and the
  // current number of running threads.
  uint64 total_threads_metric_;
  uint64 current_num_threads_metric_;

  // Metric callbacks.
  uint64 ReadNumTotalThreads();
  uint64 ReadNumCurrentThreads();

  // Webpage callback; prints all threads by category
  void ThreadPathHandler(const Webserver::ArgumentMap& args, stringstream* output);
  void PrintThreadCategoryRows(const ThreadCategory& category, stringstream* output);
};

Status ThreadMgr::StartInstrumentation(MetricRegistry* registry, Webserver* webserver) {
  DCHECK_NOTNULL(registry);
  DCHECK_NOTNULL(webserver);
  MetricContext ctx(registry, "threading");
  lock_guard<mutex> l(lock_);
  metrics_enabled_ = true;

  // TODO: These metrics should be expressed as counters but their lifecycles
  // are tough to define because ThreadMgr is a singleton.
  METRIC_total_threads.InstantiateFunctionGauge(ctx,
      bind(&ThreadMgr::ReadNumTotalThreads, this));
  METRIC_current_num_threads.InstantiateFunctionGauge(ctx,
      bind(&ThreadMgr::ReadNumCurrentThreads, this));

  Webserver::PathHandlerCallback thread_callback =
      bind<void>(mem_fn(&ThreadMgr::ThreadPathHandler), this, _1, _2);
  webserver->RegisterPathHandler("/threadz", thread_callback);
  return Status::OK();
}

uint64 ThreadMgr::ReadNumTotalThreads() {
  lock_guard<mutex> l(lock_);
  return total_threads_metric_;
}

uint64 ThreadMgr::ReadNumCurrentThreads() {
  lock_guard<mutex> l(lock_);
  return current_num_threads_metric_;
}

void ThreadMgr::AddThread(const thread::id& thread, const string& name,
    const string& category, int64_t tid) {
  lock_guard<mutex> l(lock_);
  thread_categories_[category][thread] = ThreadDescriptor(category, name, tid);
  if (metrics_enabled_) {
    current_num_threads_metric_++;
    total_threads_metric_++;
  }
}

void ThreadMgr::RemoveThread(const thread::id& boost_id, const string& category) {
  lock_guard<mutex> l(lock_);
  ThreadCategoryMap::iterator category_it = thread_categories_.find(category);
  DCHECK(category_it != thread_categories_.end());
  category_it->second.erase(boost_id);
  if (metrics_enabled_) {
    current_num_threads_metric_--;
  }
}

void ThreadMgr::PrintThreadCategoryRows(const ThreadCategory& category,
    stringstream* output) {
  BOOST_FOREACH(const ThreadCategory::value_type& thread, category) {
    ThreadStats stats;
    Status status = GetThreadStats(thread.second.thread_id(), &stats);
    if (!status.ok()) {
      LOG_EVERY_N(INFO, 100) << "Could not get per-thread statistics: "
                             << status.ToString();
    }
    (*output) << "<tr><td>" << thread.second.name() << "</td><td>"
              << (static_cast<double>(stats.user_ns) / 1e9) << "</td><td>"
              << (static_cast<double>(stats.kernel_ns) / 1e9) << "</td><td>"
              << (static_cast<double>(stats.iowait_ns) / 1e9) << "</td></tr>";
  }
}

void ThreadMgr::ThreadPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  lock_guard<mutex> l(lock_);
  vector<const ThreadCategory*> categories_to_print;
  Webserver::ArgumentMap::const_iterator category_name = args.find("group");
  if (category_name != args.end()) {
    string group = EscapeForHtmlToString(category_name->second);
    (*output) << "<h2>Thread Group: " << group << "</h2>" << endl;
    if (group != "all") {
      ThreadCategoryMap::const_iterator category = thread_categories_.find(group);
      if (category == thread_categories_.end()) {
        (*output) << "Thread group '" << group << "' not found" << endl;
        return;
      }
      categories_to_print.push_back(&category->second);
      (*output) << "<h3>" << category->first << " : " << category->second.size()
                << "</h3>";
    } else {
      BOOST_FOREACH(const ThreadCategoryMap::value_type& category, thread_categories_) {
        categories_to_print.push_back(&category.second);
      }
      (*output) << "<h3>All Threads : </h3>";
    }

    (*output) << "<table class='table table-hover table-border'>";
    (*output) << "<tr><th>Thread name</th><th>Cumulative User CPU(s)</th>"
              << "<th>Cumulative Kernel CPU(s)</th>"
              << "<th>Cumulative IO-wait(s)</th></tr>";

    BOOST_FOREACH(const ThreadCategory* category, categories_to_print) {
      PrintThreadCategoryRows(*category, output);
    }
    (*output) << "</table>";
  } else {
    (*output) << "<h2>Thread Groups</h2>";
    if (metrics_enabled_) {
      (*output) << "<h4>" << current_num_threads_metric_ << " thread(s) running";
    }
    (*output) << "<a href='/threadz?group=all'><h3>All Threads</h3>";

    BOOST_FOREACH(const ThreadCategoryMap::value_type& category, thread_categories_) {
      string category_arg;
      UrlEncode(category.first, &category_arg);
      (*output) << "<a href='/threadz?group=" << category_arg << "'><h3>"
                << category.first << " : " << category.second.size() << "</h3></a>";
    }
  }
}

// Not thread-safe.
void InitThreading() {
  if (thread_manager.get() == NULL) {
    thread_manager.reset(new ThreadMgr());
  }
}

Status StartThreadInstrumentation(MetricRegistry* registry, Webserver* webserver) {
  return thread_manager->StartInstrumentation(registry, webserver);
}

enum {
  THREAD_NOT_ASSIGNED,
  THREAD_ASSIGNED,
  THREAD_RUNNING
};

// Spin-loop until *x value equals 'from', then set *x value to 'to'.
inline void SpinWait(Atomic32* x, uint32_t from, int32_t to) {
  int loop_count = 0;

  while (base::subtle::Acquire_Load(x) != from) {
    boost::detail::yield(loop_count++);
  }

  // We use an Acquire_Load spin and a Release_Store because we need both
  // directions of memory barrier here, and atomicops.h doesn't offer a
  // Barrier_CompareAndSwap call. TSAN will fail with either Release or Acquire
  // CAS above.
  base::subtle::Release_Store(x, to);
}

void Thread::StartThread(const ThreadFunctor& functor) {
  DCHECK(thread_manager.get() != NULL)
      << "Thread created before InitThreading called";
  DCHECK_EQ(tid_, UNINITIALISED_THREAD_ID) << "StartThread called twice";

  Atomic64 c_p_tid = UNINITIALISED_THREAD_ID;
  Atomic32 p_c_assigned = THREAD_NOT_ASSIGNED;

  thread_.reset(
      new thread(&Thread::SuperviseThread, name_, category_, functor,
                 &c_p_tid, &p_c_assigned));

  // We've assigned into thread_; the child may now continue running.
  Release_Store(&p_c_assigned, THREAD_ASSIGNED);

  // Wait for the child to discover its tid, then set it.
  int loop_count = 0;
  while (base::subtle::Acquire_Load(&c_p_tid) == UNINITIALISED_THREAD_ID) {
    boost::detail::yield(loop_count++);
  }
  tid_ = base::subtle::Acquire_Load(&c_p_tid);

  VLOG(2) << "Started thread " << tid_ << " - " << category_ << ":" << name_;
}

void Thread::SuperviseThread(const string& name, const string& category,
    Thread::ThreadFunctor functor, Atomic64* c_p_tid, Atomic32 *p_c_assigned) {
  int64_t system_tid = syscall(SYS_gettid);
  if (system_tid == -1) {
    string error_msg = ErrnoToString(errno);
    LOG_EVERY_N(INFO, 100) << "Could not determine thread ID: " << error_msg;
  }
  // Make a copy, since we want to refer to these variables after the unsafe point below.
  string category_copy = category;
  shared_ptr<ThreadMgr> thread_mgr_ref = thread_manager;
  stringstream ss;
  ss << (name.empty() ? "thread" : name) << "-" << system_tid;
  string name_copy = ss.str();

  if (category_copy.empty()) category_copy = "no-category";

  // Use boost's get_id rather than the system thread ID as the unique key for this thread
  // since the latter is more prone to being recycled.
  thread_mgr_ref->AddThread(boost::this_thread::get_id(), name_copy, category_copy, system_tid);

  // Wait for the parent to unblock us.
  SpinWait(p_c_assigned, THREAD_ASSIGNED, THREAD_RUNNING);

  // Signal the parent with our tid. This signal serves double duty: the parent also knows
  // that we're done with the function parameters.
  Release_Store(c_p_tid, system_tid);

  // Any reference to any parameter not copied in by value may no longer be valid after
  // this point, since the caller that is waiting on *c_p_tid != UNINITIALISED_THREAD_ID
  // may wake, take the lock and destroy the enclosing Thread object.

  functor();
  thread_mgr_ref->RemoveThread(boost::this_thread::get_id(), category_copy);
}

} // namespace kudu
