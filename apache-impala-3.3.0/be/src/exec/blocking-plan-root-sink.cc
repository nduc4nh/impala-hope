// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/blocking-plan-root-sink.h"
#include "exprs/scalar-expr-evaluator.h"
#include "exprs/scalar-expr.h"
#include "runtime/row-batch.h"
#include "runtime/tuple-row.h"
#include "service/query-result-set.h"
#include "util/pretty-printer.h"

#include <memory>
#include <boost/thread/mutex.hpp>

using namespace std;
using boost::unique_lock;
using boost::mutex;

namespace impala {

BlockingPlanRootSink::BlockingPlanRootSink(
    TDataSinkId sink_id, const RowDescriptor* row_desc, RuntimeState* state)
  : PlanRootSink(sink_id, row_desc, state) {}

Status BlockingPlanRootSink::Send(RuntimeState* state, RowBatch* batch) {
  SCOPED_TIMER(profile()->total_time_counter());
  PlanRootSink::ValidateCollectionSlots(*row_desc_, batch);
  RETURN_IF_ERROR(PlanRootSink::UpdateAndCheckRowsProducedLimit(state, batch));
  int current_batch_row = 0;

  // Don't enter the loop if batch->num_rows() == 0; no point triggering the consumer with
  // 0 rows to return. Be wary of ever returning 0-row batches to the client; some poorly
  // written clients may not cope correctly with them. See IMPALA-4335.
  while (current_batch_row < batch->num_rows()) {
    unique_lock<mutex> l(lock_);
    // Wait until the consumer gives us a result set to fill in, or the fragment
    // instance has been cancelled.
    while (results_ == nullptr && !state->is_cancelled()) {
      SCOPED_TIMER(profile_->inactive_timer());
      sender_cv_.Wait(l);
    }
    RETURN_IF_CANCELLED(state);

    // Otherwise the consumer is ready. Fill out the rows.
    DCHECK(results_ != nullptr);
    int num_to_fetch = batch->num_rows() - current_batch_row;
    if (num_rows_requested_ > 0) num_to_fetch = min(num_to_fetch, num_rows_requested_);
    RETURN_IF_ERROR(
        results_->AddRows(output_expr_evals_, batch, current_batch_row, num_to_fetch));
    current_batch_row += num_to_fetch;
    // Prevent expr result allocations from accumulating.
    expr_results_pool_->Clear();
    // Signal the consumer.
    results_ = nullptr;
    consumer_cv_.NotifyAll();
  }
  return Status::OK();
}

Status BlockingPlanRootSink::FlushFinal(RuntimeState* state) {
  SCOPED_TIMER(profile()->total_time_counter());
  unique_lock<mutex> l(lock_);
  sender_state_ = SenderState::EOS;
  // All rows have been sent by the producer, so wake up the producer so it can set eos to
  // true.
  consumer_cv_.NotifyAll();
  return Status::OK();
}

void BlockingPlanRootSink::Close(RuntimeState* state) {
  SCOPED_TIMER(profile()->total_time_counter());
  unique_lock<mutex> l(lock_);
  // FlushFinal() won't have been called when the fragment instance encounters an error
  // before sending all rows.
  if (sender_state_ == SenderState::ROWS_PENDING) {
    sender_state_ = SenderState::CLOSED_NOT_EOS;
  }
  consumer_cv_.NotifyAll();
  DataSink::Close(state);
}

void BlockingPlanRootSink::Cancel(RuntimeState* state) {
  DCHECK(state->is_cancelled());
  sender_cv_.NotifyAll();
  consumer_cv_.NotifyAll();
}

Status BlockingPlanRootSink::GetNext(
    RuntimeState* state, QueryResultSet* results, int num_results, bool* eos) {
  unique_lock<mutex> l(lock_);

  // Set the shared QueryResultSet pointer 'results_' to the given 'results' object and
  // wake up the sender thread so it can add rows to 'results_'.
  results_ = results;
  num_rows_requested_ = num_results;
  sender_cv_.NotifyAll();

  // Wait while the sender is still producing rows and hasn't filled in the current
  // result set.
  while (sender_state_ == SenderState::ROWS_PENDING && results_ != nullptr
      && !state->is_cancelled()) {
    consumer_cv_.Wait(l);
  }

  *eos = sender_state_ == SenderState::EOS;
  return state->GetQueryStatus();
}
}
