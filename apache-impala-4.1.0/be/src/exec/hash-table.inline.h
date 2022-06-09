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

#ifndef IMPALA_EXEC_HASH_TABLE_INLINE_H
#define IMPALA_EXEC_HASH_TABLE_INLINE_H

#include "exec/hash-table.h"

namespace impala {

inline bool HashTableCtx::EvalAndHashBuild(const TupleRow* row) {
  uint8_t* expr_values = expr_values_cache_.cur_expr_values();
  uint8_t* expr_values_null = expr_values_cache_.cur_expr_values_null();
  bool has_null = EvalBuildRow(row, expr_values, expr_values_null);
  if (!stores_nulls() && has_null) return false;
  expr_values_cache_.SetCurExprValuesHash(HashRow(expr_values, expr_values_null));
  return true;
}

inline bool HashTableCtx::EvalAndHashProbe(const TupleRow* row) {
  uint8_t* expr_values = expr_values_cache_.cur_expr_values();
  uint8_t* expr_values_null = expr_values_cache_.cur_expr_values_null();
  bool has_null = EvalProbeRow(row, expr_values, expr_values_null);
  if (has_null && !(stores_nulls() && finds_some_nulls())) return false;
  expr_values_cache_.SetCurExprValuesHash(HashRow(expr_values, expr_values_null));
  return true;
}

inline void HashTableCtx::ExprValuesCache::NextRow() {
  cur_expr_values_ += expr_values_bytes_per_row_;
  cur_expr_values_null_ += num_exprs_;
  ++cur_expr_values_hash_;
  DCHECK_LE(cur_expr_values_hash_ - expr_values_hash_array_.get(), capacity_);
}

template <bool INCLUSIVE_EQUALITY, bool COMPARE_ROW, HashTable::BucketType TYPE>
inline int64_t HashTable::Probe(Bucket* buckets, uint32_t* hash_array,
    int64_t num_buckets, HashTableCtx* __restrict__ ht_ctx, uint32_t hash, bool* found,
    BucketData* bd) {
  DCHECK(ht_ctx != nullptr);
  DCHECK(buckets != nullptr);
  DCHECK_GT(num_buckets, 0);
  *found = false;
  ++ht_ctx->num_probes_;
  int64_t bucket_idx = hash & (num_buckets - 1);

  // In case of linear probing it counts the total number of steps for statistics and
  // for knowing when to exit the loop (e.g. by capping the total travel length). In case
  // of quadratic probing it is also used for calculating the length of the next jump.
  int64_t step = 0;
  do {
    Bucket* bucket = &buckets[bucket_idx];
    if (LIKELY(!bucket->IsFilled())) return bucket_idx;
    if (hash == hash_array[bucket_idx]) {
      if (COMPARE_ROW
          && ht_ctx->Equals<INCLUSIVE_EQUALITY>(
                 GetRow<TYPE>(bucket, ht_ctx->scratch_row_, bd))) {
        *found = true;
        return bucket_idx;
      }
      // Row equality failed, or not performed. This is a hash collision. Continue
      // searching.
      ++ht_ctx->num_hash_collisions_;
    }
    // Move to the next bucket.
    ++step;
    if (quadratic_probing()) {
      // The i-th probe location is idx = (hash + (step * (step + 1)) / 2) mod
      // num_buckets. This gives num_buckets unique idxs (between 0 and N-1) when
      // num_buckets is a power of 2.
      bucket_idx = (bucket_idx + step) & (num_buckets - 1);
    } else {
      // Linear probing
      bucket_idx = (bucket_idx + 1) & (num_buckets - 1);
    }
  } while (LIKELY(step < num_buckets));

  ht_ctx->travel_length_ += step;

  DCHECK_EQ(num_filled_buckets_, num_buckets)
      << "Probing of a non-full table "
      << "failed: " << quadratic_probing() << " " << hash;
  return Iterator::BUCKET_NOT_FOUND;
}

inline HashTable::Bucket* HashTable::InsertInternal(
    HashTableCtx* __restrict__ ht_ctx, Status* status) {
  bool found = false;
  uint32_t hash = ht_ctx->expr_values_cache()->CurExprValuesHash();
  BucketData bd;
  int64_t bucket_idx =
      Probe<true, true>(buckets_, hash_array_, num_buckets_, ht_ctx, hash, &found, &bd);
  DCHECK_NE(bucket_idx, Iterator::BUCKET_NOT_FOUND);
  if (found) {
    // We need to insert a duplicate node, note that this may fail to allocate memory.
    DuplicateNode* new_node = InsertDuplicateNode(bucket_idx, status, &bd);
    if (UNLIKELY(new_node == NULL)) return NULL;
  } else {
    PrepareBucketForInsert(bucket_idx, hash);
  }
  return &buckets_[bucket_idx];
}

inline bool HashTable::Insert(HashTableCtx* __restrict__ ht_ctx,
    BufferedTupleStream::FlatRowPtr flat_row, TupleRow* row, Status* status) {
  Bucket* bucket = InsertInternal(ht_ctx, status);
  if (UNLIKELY(bucket == NULL)) return false;
  // If successful insert, update the contents of the newly inserted entry with 'idx'.
  if (bucket->HasDuplicates()) {
    DuplicateNode* node = bucket->GetDuplicate();
    if (UNLIKELY(node == NULL)) return false;
    if (stores_tuples()) {
      node->htdata.tuple = row->GetTuple(0);
    } else {
      node->htdata.flat_row = flat_row;
    }
  } else {
    if (stores_tuples()) {
      Tuple* tuple = row->GetTuple(0);
      bucket->SetTuple(tuple);
    } else {
      bucket->SetFlatRow(flat_row);
    }
  }
  return true;
}

template <const bool READ>
inline void HashTable::PrefetchBucket(uint32_t hash) {
  int64_t bucket_idx = hash & (num_buckets_ - 1);
  // Two optional arguments:
  // 'rw': 1 means the memory access is write
  // 'locality': 0-3. 0 means no temporal locality. 3 means high temporal locality.
  // On x86, they map to instructions prefetchnta and prefetch{2-0} respectively.
  // TODO: Reconsider the locality level with smaller prefetch batch size.
  __builtin_prefetch(&buckets_[bucket_idx], READ ? 0 : 1, 1);
  __builtin_prefetch(&hash_array_[bucket_idx], READ ? 0 : 1, 1);
}

inline HashTable::Iterator HashTable::FindProbeRow(HashTableCtx* __restrict__ ht_ctx) {
  bool found = false;
  uint32_t hash = ht_ctx->expr_values_cache()->CurExprValuesHash();
  BucketData bd;
  int64_t bucket_idx =
      Probe<false, true>(buckets_, hash_array_, num_buckets_, ht_ctx, hash, &found, &bd);
  if (found) {
    return Iterator(this, ht_ctx->scratch_row(), bucket_idx,
        stores_duplicates() ? bd.duplicates : NULL);
  }
  return End();
}

// TODO: support lazy evaluation like HashTable::Insert().
template <HashTable::BucketType TYPE>
inline HashTable::Iterator HashTable::FindBuildRowBucket(
    HashTableCtx* __restrict__ ht_ctx, bool* found) {
  uint32_t hash = ht_ctx->expr_values_cache()->CurExprValuesHash();
  BucketData bd;
  int64_t bucket_idx = Probe<true, true, TYPE>(
      buckets_, hash_array_, num_buckets_, ht_ctx, hash, found, &bd);
  DuplicateNode* duplicates = NULL;
  if (stores_duplicates() && LIKELY(bucket_idx != Iterator::BUCKET_NOT_FOUND)) {
    duplicates = bd.duplicates;
  }
  return Iterator(this, ht_ctx->scratch_row(), bucket_idx, duplicates);
}

inline HashTable::Iterator HashTable::Begin(const HashTableCtx* ctx) {
  int64_t bucket_idx = Iterator::BUCKET_NOT_FOUND;
  DuplicateNode* node = NULL;
  NextFilledBucket(&bucket_idx, &node);
  return Iterator(this, ctx->scratch_row(), bucket_idx, node);
}

inline HashTable::Iterator HashTable::FirstUnmatched(HashTableCtx* ctx) {
  int64_t bucket_idx = Iterator::BUCKET_NOT_FOUND;
  DuplicateNode* node = NULL;
  NextFilledBucket(&bucket_idx, &node);
  Iterator it(this, ctx->scratch_row(), bucket_idx, node);
  // Check whether the bucket, or its first duplicate node, is matched. If it is not
  // matched, then return. Otherwise, move to the first unmatched entry (node or bucket).
  Bucket* bucket = &buckets_[bucket_idx];
  bool has_duplicates = stores_duplicates() && bucket->HasDuplicates();
  if ((!has_duplicates && bucket->IsMatched()) || (has_duplicates && node->IsMatched())) {
    it.NextUnmatched();
  }
  return it;
}

inline void HashTable::NextFilledBucket(int64_t* bucket_idx, DuplicateNode** node) {
  ++*bucket_idx;
  for (; *bucket_idx < num_buckets_; ++*bucket_idx) {
    if (buckets_[*bucket_idx].IsFilled()) {
      *node = stores_duplicates() ? buckets_[*bucket_idx].GetDuplicate() : NULL;
      return;
    }
  }
  // Reached the end of the hash table.
  *bucket_idx = Iterator::BUCKET_NOT_FOUND;
  *node = NULL;
}

inline void HashTable::PrepareBucketForInsert(int64_t bucket_idx, uint32_t hash) {
  DCHECK_GE(bucket_idx, 0);
  DCHECK_LT(bucket_idx, num_buckets_);
  Bucket* bucket = &buckets_[bucket_idx];
  DCHECK(!bucket->IsFilled());
  ++num_filled_buckets_;
  bucket->PrepareBucketForInsert();
  hash_array_[bucket_idx] = hash;
}

inline HashTable::DuplicateNode* HashTable::AppendNextNode(Bucket* bucket) {
  DCHECK_GT(node_remaining_current_page_, 0);
  bucket->SetDuplicate(next_node_);
  ++num_duplicate_nodes_;
  --node_remaining_current_page_;
  return next_node_++;
}

inline HashTable::DuplicateNode* HashTable::InsertDuplicateNode(
    int64_t bucket_idx, Status* status, BucketData* bucket_data) {
  DCHECK_GE(bucket_idx, 0);
  DCHECK_LT(bucket_idx, num_buckets_);
  Bucket* bucket = &buckets_[bucket_idx];
  DCHECK(bucket->IsFilled());
  DCHECK(stores_duplicates());
  bool has_duplicates = bucket->HasDuplicates();
  // Allocate one duplicate node for the new data and one for the preexisting data,
  // if needed.
  while (node_remaining_current_page_ < 1 + !has_duplicates) {
    if (UNLIKELY(!GrowNodeArray(status))) return NULL;
  }
  if (!has_duplicates) {
    // This is the first duplicate in this bucket. It means that we need to convert
    // the current entry in the bucket to a node and link it from the bucket.
    next_node_->htdata.flat_row = bucket_data->htdata.flat_row;
    DCHECK(!bucket->IsMatched());
    next_node_->SetNextUnMatched(nullptr);
    AppendNextNode(bucket);
    bucket->SetHasDuplicates();
    ++num_buckets_with_duplicates_;
  }
  // Link a new node and UnsetMatched
  next_node_->SetNextUnMatched(bucket->GetDuplicate());
  return AppendNextNode(bucket);
}

inline TupleRow* IR_ALWAYS_INLINE HashTable::GetRow(HtData& htdata, TupleRow* row) const {
  if (stores_tuples()) {
    row->SetTuple(0, htdata.tuple);
    // return reinterpret_cast<TupleRow*>(&htdata.tuple);
    return row;
  } else {
    // TODO: GetTupleRow() has interpreted code that iterates over the row's descriptor.
    tuple_stream_->GetTupleRow(htdata.flat_row, row);
    return row;
  }
}

template <HashTable::BucketType TYPE>
inline TupleRow* IR_ALWAYS_INLINE HashTable::GetRow(
    Bucket* bucket, TupleRow* row, BucketData* bucket_data) const {
  DCHECK(bucket != NULL);
  if (UNLIKELY(stores_duplicates() && bucket->HasDuplicates())) {
    *bucket_data = bucket->GetBucketData();
    DuplicateNode* duplicate = bucket_data->duplicates;
    DCHECK(duplicate != NULL);
    return GetRow(duplicate->htdata, row);
  } else {
    *bucket_data = bucket->GetBucketData<TYPE>();
    return GetRow(bucket_data->htdata, row);
  }
}

inline TupleRow* IR_ALWAYS_INLINE HashTable::Iterator::GetRow() const {
  DCHECK(!AtEnd());
  DCHECK(table_ != NULL);
  DCHECK(scratch_row_ != NULL);
  Bucket* bucket = &table_->buckets_[bucket_idx_];
  if (UNLIKELY(table_->stores_duplicates() && bucket->HasDuplicates())) {
    DCHECK(node_ != NULL);
    return table_->GetRow(node_->htdata, scratch_row_);
  } else {
    HtData htdata = bucket->GetBucketData().htdata;
    return table_->GetRow(htdata, scratch_row_);
  }
}

template <HashTable::BucketType TYPE>
inline Tuple* IR_ALWAYS_INLINE HashTable::Iterator::GetTuple() const {
  DCHECK(!AtEnd());
  DCHECK(table_->stores_tuples());
  Bucket* bucket = &table_->buckets_[bucket_idx_];
  // TODO: To avoid the has_duplicates check, store the HtData* in the Iterator.
  if (UNLIKELY(table_->stores_duplicates() && bucket->HasDuplicates())) {
    DCHECK(node_ != NULL);
    return node_->htdata.tuple;
  } else {
    return bucket->GetTuple<TYPE>();
  }
}

inline void HashTable::Iterator::SetTuple(Tuple* tuple, uint32_t hash) {
  DCHECK(!AtEnd());
  DCHECK(table_->stores_tuples());
  table_->PrepareBucketForInsert(bucket_idx_, hash);
  table_->buckets_[bucket_idx_].SetTuple<false>(tuple);
}

inline void HashTable::Iterator::SetMatched() {
  DCHECK(!AtEnd());
  Bucket* bucket = &table_->buckets_[bucket_idx_];
  if (table_->stores_duplicates() && bucket->HasDuplicates()) {
    node_->SetMatched();
  } else {
    bucket->SetMatched();
  }
  // Used for disabling spilling of hash tables in right and full-outer joins with
  // matches. See IMPALA-1488.
  table_->has_matches_ = true;
}

inline bool HashTable::Iterator::IsMatched() const {
  DCHECK(!AtEnd());
  Bucket* bucket = &table_->buckets_[bucket_idx_];
  if (table_->stores_duplicates() && bucket->HasDuplicates()) {
    return node_->IsMatched();
  }
  return bucket->IsMatched();
}

inline void HashTable::Iterator::SetAtEnd() {
  bucket_idx_ = BUCKET_NOT_FOUND;
  node_ = NULL;
}

template <const bool READ>
inline void HashTable::Iterator::PrefetchBucket() {
  if (LIKELY(!AtEnd())) {
    // HashTable::PrefetchBucket() takes a hash value to index into the hash bucket
    // array. Passing 'bucket_idx_' here is sufficient.
    DCHECK_EQ((bucket_idx_ & ~(table_->num_buckets_ - 1)), 0);
    table_->PrefetchBucket<READ>(bucket_idx_);
  }
}

inline void HashTable::Iterator::Next() {
  DCHECK(!AtEnd());
  if (table_->stores_duplicates() && table_->buckets_[bucket_idx_].HasDuplicates()
      && node_->Next() != NULL) {
    node_ = node_->Next();
  } else {
    table_->NextFilledBucket(&bucket_idx_, &node_);
  }
}

inline void HashTable::Iterator::NextDuplicate() {
  DCHECK(!AtEnd());
  if (table_->stores_duplicates() && table_->buckets_[bucket_idx_].HasDuplicates()
      && node_->Next() != NULL) {
    node_ = node_->Next();
  } else {
    bucket_idx_ = BUCKET_NOT_FOUND;
    node_ = NULL;
  }
}

inline void HashTable::Iterator::NextUnmatched() {
  DCHECK(!AtEnd());
  Bucket* bucket = &table_->buckets_[bucket_idx_];
  // Check if there is any remaining unmatched duplicate node in the current bucket.
  if (table_->stores_duplicates() && bucket->HasDuplicates()) {
    auto next_node = node_->Next();
    while (next_node != NULL) {
      node_ = next_node;
      if (!node_->IsMatched()) return;
      next_node = next_node->Next();
    }
  }
  // Move to the next filled bucket and return if this bucket is not matched or
  // iterate to the first not matched duplicate node.
  table_->NextFilledBucket(&bucket_idx_, &node_);
  while (bucket_idx_ != Iterator::BUCKET_NOT_FOUND) {
    bucket = &table_->buckets_[bucket_idx_];
    if (!table_->stores_duplicates() || !bucket->HasDuplicates()) {
      if (!bucket->IsMatched()) return;
    } else {
      auto next_node = node_->Next();
      while (node_->IsMatched() && next_node != NULL) {
        node_ = next_node;
        next_node = next_node->Next();
      }
      if (!node_->IsMatched()) return;
    }
    table_->NextFilledBucket(&bucket_idx_, &node_);
  }
}

inline void HashTableCtx::set_level(int level) {
  DCHECK_GE(level, 0);
  DCHECK_LT(level, seeds_.size());
  level_ = level;
}

inline int64_t HashTable::CurrentMemSize() const {
  return num_buckets_ * (sizeof(Bucket) + sizeof(uint32_t))
      + num_duplicate_nodes_ * sizeof(DuplicateNode);
}

inline int64_t HashTable::NumInsertsBeforeResize() const {
  return std::max<int64_t>(
      0, static_cast<int64_t>(num_buckets_ * MAX_FILL_FACTOR) - num_filled_buckets_);
}

} // namespace impala

#endif
