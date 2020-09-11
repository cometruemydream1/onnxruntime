// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "testcaset_request.h"
#include "dataitem_request.h"
#include "TestCase.h"

#include "core/common/logging/logging.h"
#include "core/common/logging/macros.h"
#include <core/session/onnxruntime_cxx_api.h>

#include <utility>

namespace onnxruntime {
namespace test {

TestCaseRequestContext::TestCaseRequestContext(const Callback& cb, PThreadPool tp, const ITestCase& c, Ort::Env& env,
                       const Ort::SessionOptions& session_opts)
    : cb_(cb),
      tp_(tp),
      c_(c),
      env_(env),
      session_opts_(session_opts.Clone()),
      session_(nullptr),
      allocator_(),
      result_(),
      data_tasks_started_(0),
      data_tasks_inprogress_(0) {
  result_ = std::make_shared<TestCaseResult>(c_.GetDataCount(), EXECUTE_RESULT::NOT_SET, c.GetTestCaseName());
  SetTimeSpecToZero(&test_case_time_);
  CallableFactory<TestCaseRequestContext, void, size_t, EXECUTE_RESULT, const TIME_SPEC&> f(this);
  on_data_task_cb_ = f.GetCallable<&TestCaseRequestContext::OnDataTaskComplete>();
}

void TestCaseRequestContext::SetupSession() {
  const auto* test_case_name = c_.GetTestCaseName().c_str();
  session_opts_.SetLogId(test_case_name);
  Ort::Session session{env_, c_.GetModelUrl(), session_opts_};
  LOGF_DEFAULT(INFO, "testing %s\n", test_case_name);
  session_ = std::move(session);
}


std::shared_ptr<TestCaseResult> TestCaseRequestContext::Run(PThreadPool tpool,
                                                            const ITestCase& c, Ort::Env& env,
                                                            const Ort::SessionOptions& session_opts,
                                                            size_t concurrent_runs, size_t repeat_count) {
  //temp hack. Because we have no resource control. We may not have enough memory to run this test in parallel
  if (c.GetTestCaseName() == "coreml_FNS-Candy_ImageNet") {
    concurrent_runs = 1;
  }

  Callback empty_cb;
  TestCaseRequestContext ctx(empty_cb, tpool, c, env, session_opts);

  const size_t data_count = c.GetDataCount();
  if (concurrent_runs > 1 && data_count > 1) {
    ctx.RunAsync(concurrent_runs);
    ctx.Wait();
  } else {
    ctx.RunSequentially(repeat_count);
  }
  return ctx.GetResult();
}

void TestCaseRequestContext::Request(Callback cb, PThreadPool tpool,
                                     const ITestCase& c,
                                     Ort::Env& env,
                                     const Ort::SessionOptions& session_opts,
                                     size_t concurrent_runs) {
  //temp hack. Because we have no resource control. We may not have enough memory to run this test in parallel
  if (c.GetTestCaseName() == "coreml_FNS-Candy_ImageNet") {
    concurrent_runs = 1;
  }

  std::unique_ptr<TestCaseRequestContext> self(new TestCaseRequestContext(cb, tpool, c, env, session_opts));
  CallableFactory<TestCaseRequestContext, void, size_t> f(self.get());
  auto runnable = f.GetCallable<&TestCaseRequestContext::RunAsync>();
  tpool->Schedule([runnable, concurrent_runs]() { runnable.Invoke(concurrent_runs); });
  self.release();
}

void TestCaseRequestContext::RunAsync(size_t concurrent_runs) {
  assert(concurrent_runs > 0);
  concurrent_runs = std::min(concurrent_runs, c_.GetDataCount());

  SetupSession();

  // Reserve one refcount for this thread so the object does not get deleted when
  // several TestCases are run in parallel
  // by worker threads before this thread finishes. In exchange, we run one of the tasks.
  auto this_task_id = data_tasks_started_.fetch_add(1, std::memory_order_relaxed);
  data_tasks_inprogress_.fetch_add(1, std::memory_order_relaxed);

  for (size_t i = 1; i < concurrent_runs; ++i) {
    auto next_to_run = data_tasks_started_.fetch_add(1, std::memory_order_relaxed);
    if (next_to_run < c_.GetDataCount()) {
      data_tasks_inprogress_.fetch_add(1, std::memory_order_relaxed);
      DataTaskRequestContext::Request(on_data_task_cb_, tp_, c_, session_, &allocator_, next_to_run);
    } else {
      break;
    }
  }
  // This runs in this thread and we should invoke the callback for it.
  auto result = DataTaskRequestContext::Run(c_, session_, &allocator_, this_task_id);
  OnDataTaskComplete(this_task_id, result.first, result.second);
}

void TestCaseRequestContext::OnDataTaskComplete(size_t task_id, EXECUTE_RESULT result, const TIME_SPEC& spent_time) {
  TIME_SPEC zero;
  SetTimeSpecToZero(&zero);
  AccumulateTimeSpec(&test_case_time_, &zero, &spent_time);
  result_->SetResult(task_id, result);

  auto next_to_run = data_tasks_started_.fetch_add(1, std::memory_order_relaxed);
  if (next_to_run < c_.GetDataCount()) {
    data_tasks_inprogress_.fetch_add(1, std::memory_order_relaxed);
    DataTaskRequestContext::Request(on_data_task_cb_, tp_, c_, session_, &allocator_, next_to_run);
  }

  auto before_we_done = data_tasks_inprogress_.fetch_sub(1, std::memory_order_acq_rel);
  assert(before_we_done > 0);
  if (before_we_done == 1U) {
    CalculateAndLogStats();
    if (cb_) {
      std::unique_ptr<TestCaseRequestContext> self(this);
      cb_.Invoke(result_);
      // No member access beyond this point
    } else {
      std::unique_lock<std::mutex> g(mut_);
      finished_ = true;
      g.unlock();
      cond_.notify_one();
    }
  }
}

void TestCaseRequestContext::RunSequentially(size_t repeat_count) {

  SetupSession();

  TIME_SPEC zero;
  SetTimeSpecToZero(&zero);

  const size_t data_count = c_.GetDataCount();
  for (size_t idx_repeat = 0; idx_repeat < repeat_count; ++idx_repeat) {
    for (size_t idx_data = 0; idx_data != data_count; ++idx_data) {
      auto result = DataTaskRequestContext::Run(c_, session_, &allocator_, idx_data);
      result_->SetResult(idx_data, result.first);
      AccumulateTimeSpec(&test_case_time_, &zero, &result.second);
    }
  }
}

void TestCaseRequestContext::Wait() const {
  std::unique_lock<std::mutex> ul(mut_);
  while (!finished_) {
    cond_.wait(ul);
  }
}

void TestCaseRequestContext::CalculateAndLogStats() const {
  result_->SetSpentTime(test_case_time_);
  const auto& test_case_name = c_.GetTestCaseName();
  const std::vector<EXECUTE_RESULT>& er = result_->GetExcutionResult();
  for (size_t i = 0; i != er.size(); ++i) {
    EXECUTE_RESULT r = er[i];
    if (r == EXECUTE_RESULT::SUCCESS) continue;
    std::string s = c_.GetDatasetDebugInfoString(i);
    switch (r) {
      case EXECUTE_RESULT::RESULT_DIFFERS:
        LOGF_DEFAULT(ERROR, "%s: result differs. Dataset:%s\n", test_case_name.c_str(), s.c_str());
        break;
      case EXECUTE_RESULT::SHAPE_MISMATCH:
        LOGF_DEFAULT(ERROR, "%s: shape mismatch. Dataset:%s\n", test_case_name.c_str(), s.c_str());
        break;
      case EXECUTE_RESULT::TYPE_MISMATCH:
        LOGF_DEFAULT(ERROR, "%s: type mismatch. Dataset:%s\n", test_case_name.c_str(), s.c_str());
        break;
      case EXECUTE_RESULT::MODEL_SHAPE_MISMATCH:
        LOGF_DEFAULT(ERROR, "%s: shape in model file mismatch. Dataset:%s\n", test_case_name.c_str(), s.c_str());
        break;
      case EXECUTE_RESULT::MODEL_TYPE_MISMATCH:
        LOGF_DEFAULT(ERROR, "%s: type in model file mismatch. Dataset:%s\n", test_case_name.c_str(), s.c_str());
      default:
        //nothing to do
        break;
    }
    break;
  }
}
}
}


