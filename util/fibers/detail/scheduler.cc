// Copyright 2023, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//
#include "util/fibers/detail/scheduler.h"

#include "base/logging.h"

namespace util {
namespace fb2 {
namespace detail {

namespace ctx = boost::context;
using namespace std;

namespace {

constexpr size_t kSizeOfCtx = sizeof(FiberInterface);  // because of the virtual +8 bytes.
constexpr size_t kSizeOfSH = sizeof(FI_SleepHook);
constexpr size_t kSizeOfLH = sizeof(FI_ListHook);
class DispatcherImpl final : public FiberInterface {
 public:
  DispatcherImpl(ctx::preallocated const& palloc, ctx::fixedsize_stack&& salloc,
                 Scheduler* sched) noexcept;
  ~DispatcherImpl();

  bool is_terminating() const {
    return is_terminating_;
  }

 private:

  ctx::fiber Run(ctx::fiber&& c);

  bool is_terminating_ = false;
};

// Serves as a stub Fiber since it does not allocate any stack.
// It's used as a main fiber of the thread.
class MainFiberImpl final : public FiberInterface {
 public:
  MainFiberImpl() noexcept : FiberInterface{MAIN, 1, "main"} {
  }

 protected:
  void Terminate() {
  }
};


DispatcherImpl* MakeDispatcher(Scheduler* sched) {
  ctx::fixedsize_stack salloc;
  ctx::stack_context sctx = salloc.allocate();
  ctx::preallocated palloc = MakePreallocated<DispatcherImpl>(sctx);

  void* sp_ptr = palloc.sp;

  // placement new of context on top of fiber's stack
  return new (sp_ptr) DispatcherImpl{std::move(palloc), std::move(salloc), sched};
}


// Per thread initialization structure.
struct FiberInitializer {
  // Currently active fiber.
  FiberInterface* active;

  // Per-thread scheduler instance.
  // Allows overriding the main dispatch loop
  Scheduler* sched;
  DispatcherAlgo custom_algo;

  FiberInitializer(const FiberInitializer&) = delete;

  FiberInitializer() : sched(nullptr) {
    DVLOG(1) << "Initializing FiberLib";

    // main fiber context of this thread.
    // We use it as a stub
    FiberInterface* main_ctx = new MainFiberImpl{};
    active = main_ctx;
    sched = new Scheduler(main_ctx);
  }

  ~FiberInitializer() {
    FiberInterface* main_cntx = sched->main_context();
    delete sched;
    delete main_cntx;
  }
};

FiberInitializer& FbInitializer() noexcept {
  // initialized the first time control passes; per thread
  thread_local static FiberInitializer fb_initializer;
  return fb_initializer;
}

// DispatcherImpl implementation.
DispatcherImpl::DispatcherImpl(ctx::preallocated const& palloc, ctx::fixedsize_stack&& salloc,
                               detail::Scheduler* sched) noexcept
    : FiberInterface{DISPATCH, 0, "_dispatch"} {
  entry_ = ctx::fiber(std::allocator_arg, palloc, salloc,
                      [this](ctx::fiber&& caller) { return Run(std::move(caller)); });
  scheduler_ = sched;
}

DispatcherImpl::~DispatcherImpl() {
  DVLOG(1) << "~DispatcherImpl";

  DCHECK(!entry_);
}

ctx::fiber DispatcherImpl::Run(ctx::fiber&& c) {
  if (c) {
    // We context switched from intrusive_ptr_release and this object is destroyed.
    return std::move(c);
  }

  // Normal SwitchTo operation.

  auto& fb_init = detail::FbInitializer();
  if (fb_init.custom_algo) {
    fb_init.custom_algo(fb_init.sched);
  } else {
    fb_init.sched->DefaultDispatch();
  }

  DVLOG(1) << "Dispatcher exiting, switching to main_cntx";
  is_terminating_ = true;

  // Like with worker fibers, we switch to another fiber, but in this case to the main fiber.
  // We will come back here during the deallocation of DispatcherImpl from intrusive_ptr_release
  // in order to return from Run() and come back to main context.
  auto fc = fb_init.sched->main_context()->SwitchTo();

  DCHECK(fc);  // Should bring us back to main, into intrusive_ptr_release.
  return fc;
}

}  // namespace


Scheduler::Scheduler(FiberInterface* main_cntx) : main_cntx_(main_cntx) {
  DCHECK(!main_cntx->scheduler_);
  main_cntx->scheduler_ = this;
  dispatch_cntx_.reset(MakeDispatcher(this));
}

Scheduler::~Scheduler() {
  shutdown_ = true;
  DCHECK(main_cntx_ == FiberActive());
  DCHECK(ready_queue_.empty());

  DispatcherImpl* dimpl = static_cast<DispatcherImpl*>(dispatch_cntx_.get());
  if (!dimpl->is_terminating()) {
    DVLOG(1) << "~Scheduler switching to dispatch " << dispatch_cntx_->IsDefined();
    auto fc = dispatch_cntx_->SwitchTo();
    CHECK(!fc);
    CHECK(dimpl->is_terminating());
  }
  DCHECK_EQ(0u, num_worker_fibers_);

  // destroys the stack and the object via intrusive_ptr_release.
  dispatch_cntx_.reset();
  DestroyTerminated();
}

ctx::fiber_context Scheduler::Preempt() {
  if (ready_queue_.empty()) {
    return dispatch_cntx_->SwitchTo();
  }

  DCHECK(!ready_queue_.empty());
  FiberInterface* fi = &ready_queue_.front();
  ready_queue_.pop_front();

  __builtin_prefetch(fi);
  return fi->SwitchTo();
}

void Scheduler::Attach(FiberInterface* cntx) {
  cntx->scheduler_ = this;
  if (cntx->type() == FiberInterface::WORKER) {
    ++num_worker_fibers_;
  }
}

void Scheduler::ScheduleTermination(FiberInterface* cntx) {
  terminate_queue_.push_back(*cntx);
  if (cntx->type() == FiberInterface::WORKER) {
    --num_worker_fibers_;
  }
}

void Scheduler::DefaultDispatch() {
  DCHECK(ready_queue_.empty());

  /*while (true) {
    if (shutdown_) {
      if (num_worker_fibers_ == 0)
        break;
    }
    DestroyTerminated();

  }*/
  DestroyTerminated();
  LOG(WARNING) << "No thread suspension is supported";
}

void Scheduler::DestroyTerminated() {
  while (!terminate_queue_.empty()) {
    FiberInterface* tfi = &terminate_queue_.front();
    terminate_queue_.pop_front();
    DVLOG(1) << "Destructing " << tfi->name_;

    // maybe someone holds a Fiber handle and waits for the fiber to join.
    intrusive_ptr_release(tfi);
  }
}

void Scheduler::WaitUntil(chrono::steady_clock::time_point tp, FiberInterface* me) {
  DCHECK(!me->sleep_hook.is_linked());
  me->tp_ = tp;
  sleep_queue_.insert(*me);
  auto fc = Preempt();
  DCHECK(!fc);
}

void Scheduler::ProcessSleep() {
  if (sleep_queue_.empty())
    return;

  chrono::steady_clock::time_point now = chrono::steady_clock::now();
  while (sleep_queue_.begin()->tp_ >= now) {
    FiberInterface& fi = *sleep_queue_.begin();
    MarkReady(&fi);
    sleep_queue_.erase(fi);
    if (sleep_queue_.empty())
      break;
  }
}

FiberInterface* FiberActive() noexcept {
  return FbInitializer().active;
}



FiberInterface::FiberInterface(Type type, uint32_t cnt, string_view nm)
    : use_count_(cnt), flagval_(0), type_(type) {
  size_t len = std::min(nm.size(), sizeof(name_) - 1);
  name_[len] = 0;
  if (len) {
    memcpy(name_, nm.data(), len);
  }
}

FiberInterface::~FiberInterface() {
  DVLOG(2) << "Destroying " << name_;
  DCHECK(wait_queue_.empty());
  DCHECK(!list_hook.is_linked());
}

// We can not destroy this instance within the context of the fiber it's been running in.
// The reason: the instance is hosted within the stack region of the fiber itself, and it
// implicitly destroys the stack when destroying its 'entry_' member variable.
// Therefore, to destroy a FiberInterface (WORKER) object, we must call intrusive_ptr_release
// from another fiber. intrusive_ptr_release is smart about how it releases resources too.
ctx::fiber_context FiberInterface::Terminate() {
  DCHECK(this == FiberActive());
  DCHECK(!list_hook.is_linked());
  DCHECK(!flags.terminated);

  flags.terminated = 1;
  scheduler_->ScheduleTermination(this);

  while (!wait_queue_.empty()) {
    FiberInterface* blocked = &wait_queue_.front();
    wait_queue_.pop_front();

    // should be the scheduler of the blocked fiber.
    blocked->scheduler_->MarkReady(blocked);
  }

  // usually Preempt returns empty fc but here we return the value of where
  // to switch to when this fiber completes. See intrusive_ptr_release for more info.
  return scheduler_->Preempt();
}

void FiberInterface::Start() {
  auto& fb_init = detail::FbInitializer();
  fb_init.sched->Attach(this);
  fb_init.sched->MarkReady(this);
}

void FiberInterface::Join() {
  FiberInterface* active = FiberActive();

  CHECK(active != this);

  // currently single threaded.
  // TODO: to use Vyukov's intrusive mpsc queue:
  // https://www.boost.org/doc/libs/1_63_0/boost/fiber/detail/context_mpsc_queue.hpp
  CHECK(active->scheduler_ == scheduler_);

  if (!flags.terminated) {
    wait_queue_.push_front(*active);
    scheduler_->Preempt();
  }
}

ctx::fiber_context FiberInterface::SwitchTo() {
  FiberInterface* prev = this;

  std::swap(FbInitializer().active, prev);

  // pass pointer to the context that resumes `this`
  return std::move(entry_).resume_with([prev](ctx::fiber_context&& c) {
    DCHECK(!prev->entry_);

    prev->entry_ = std::move(c);  // update the return address in the context we just switch from.
    return ctx::fiber_context{};
  });
}

}  // namespace detail

void SetCustomDispatcher(DispatcherAlgo algo) {
  detail::FiberInitializer& fb_init = detail::FbInitializer();
  fb_init.custom_algo = std::move(algo);
}

}  // namespace fb2
}  // namespace util