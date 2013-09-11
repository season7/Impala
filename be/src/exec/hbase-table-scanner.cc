// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exec/hbase-table-scanner.h"
#include "exec/hbase-scan-node.h"

#include <cstring>
#include <algorithm>

#include "util/bit-util.h"
#include "util/jni-util.h"
#include "runtime/descriptors.h"
#include "runtime/runtime-state.h"
#include "runtime/mem-pool.h"
#include "runtime/tuple.h"

using namespace std;
using namespace impala;

jclass HBaseTableScanner::scan_cl_ = NULL;
jclass HBaseTableScanner::resultscanner_cl_ = NULL;
jclass HBaseTableScanner::result_cl_ = NULL;
jclass HBaseTableScanner::cell_cl_ = NULL;
jclass HBaseTableScanner::hconstants_cl_ = NULL;
jclass HBaseTableScanner::filter_list_cl_ = NULL;
jclass HBaseTableScanner::filter_list_op_cl_ = NULL;
jclass HBaseTableScanner::single_column_value_filter_cl_ = NULL;
jclass HBaseTableScanner::compare_op_cl_ = NULL;
jmethodID HBaseTableScanner::scan_ctor_ = NULL;
jmethodID HBaseTableScanner::scan_set_max_versions_id_ = NULL;
jmethodID HBaseTableScanner::scan_set_caching_id_ = NULL;
jmethodID HBaseTableScanner::scan_set_cache_blocks_id_ = NULL;
jmethodID HBaseTableScanner::scan_add_column_id_ = NULL;
jmethodID HBaseTableScanner::scan_set_filter_id_ = NULL;
jmethodID HBaseTableScanner::scan_set_start_row_id_ = NULL;
jmethodID HBaseTableScanner::scan_set_stop_row_id_ = NULL;
jmethodID HBaseTableScanner::resultscanner_next_id_ = NULL;
jmethodID HBaseTableScanner::resultscanner_close_id_ = NULL;
jmethodID HBaseTableScanner::result_raw_id_ = NULL;
jmethodID HBaseTableScanner::cell_get_row_array_ = NULL;
jmethodID HBaseTableScanner::cell_get_family_array_ = NULL;
jmethodID HBaseTableScanner::cell_get_qualifier_array_ = NULL;
jmethodID HBaseTableScanner::cell_get_value_array_ = NULL;
jmethodID HBaseTableScanner::cell_get_family_offset_id_ = NULL;
jmethodID HBaseTableScanner::cell_get_family_length_id_ = NULL;
jmethodID HBaseTableScanner::cell_get_qualifier_offset_id_ = NULL;
jmethodID HBaseTableScanner::cell_get_qualifier_length_id_ = NULL;
jmethodID HBaseTableScanner::cell_get_row_offset_id_ = NULL;
jmethodID HBaseTableScanner::cell_get_row_length_id_ = NULL;
jmethodID HBaseTableScanner::cell_get_value_offset_id_ = NULL;
jmethodID HBaseTableScanner::cell_get_value_length_id_ = NULL;
jmethodID HBaseTableScanner::filter_list_ctor_ = NULL;
jmethodID HBaseTableScanner::filter_list_add_filter_id_ = NULL;
jmethodID HBaseTableScanner::single_column_value_filter_ctor_ = NULL;
jobject HBaseTableScanner::empty_row_ = NULL;
jobject HBaseTableScanner::must_pass_all_op_ = NULL;
jobjectArray HBaseTableScanner::compare_ops_ = NULL;

void HBaseTableScanner::ScanRange::DebugString(int indentation_level,
    stringstream* out) {
  *out << string(indentation_level * 2, ' ');
  if (!start_key_.empty()) {
    *out << " start_key=" << start_key_;
  }
  if (!stop_key_.empty()) {
    *out << " stop_key=" << stop_key_;
  }
}

HBaseTableScanner::HBaseTableScanner(
    HBaseScanNode* scan_node, HBaseTableFactory* htable_factory, RuntimeState* state)
  : scan_node_(scan_node),
    state_(state),
    htable_factory_(htable_factory),
    htable_(NULL),
    scan_(NULL),
    resultscanner_(NULL),
    cells_(NULL),
    cell_index_(0),
    num_requested_cells_(0),
    num_addl_requested_cols_(0),
    num_cells_(0),
    all_cells_present_(false),
    value_pool_(new MemPool(scan_node_->mem_tracker())),
    scan_setup_timer_(ADD_TIMER(scan_node_->runtime_profile(),
      "HBaseTableScanner.ScanSetup")) {
  const TQueryOptions& query_option = state->query_options();
  if (query_option.__isset.hbase_caching && query_option.hbase_caching > 0) {
    rows_cached_ = query_option.hbase_caching;
  } else {
    int max_caching = scan_node_->suggested_max_caching();
    rows_cached_ = (max_caching > 0 && max_caching < DEFAULT_ROWS_CACHED) ?
        max_caching : DEFAULT_ROWS_CACHED;
  }
  cache_blocks_ = query_option.__isset.hbase_cache_blocks &&
      query_option.hbase_cache_blocks;
}

Status HBaseTableScanner::Init() {
  // Get the JNIEnv* corresponding to current thread.
  JNIEnv* env = getJNIEnv();
  if (env == NULL) {
    return Status("Failed to get/create JVM");
  }

  // Global class references:
  // HTable, Scan, ResultScanner, Result, ImmutableBytesWritable, HConstants.
  RETURN_IF_ERROR(
      JniUtil::GetGlobalClassRef(env, "org/apache/hadoop/hbase/client/Scan", &scan_cl_));
  RETURN_IF_ERROR(
      JniUtil::GetGlobalClassRef(env, "org/apache/hadoop/hbase/client/ResultScanner",
          &resultscanner_cl_));
  RETURN_IF_ERROR(
      JniUtil::GetGlobalClassRef(env, "org/apache/hadoop/hbase/client/Result",
          &result_cl_));
  RETURN_IF_ERROR(
      JniUtil::GetGlobalClassRef(env, "org/apache/hadoop/hbase/HConstants",
          &hconstants_cl_));
  RETURN_IF_ERROR(
      JniUtil::GetGlobalClassRef(env, "org/apache/hadoop/hbase/filter/FilterList",
          &filter_list_cl_));
  RETURN_IF_ERROR(
      JniUtil::GetGlobalClassRef(env,
          "org/apache/hadoop/hbase/filter/FilterList$Operator", &filter_list_op_cl_));
  RETURN_IF_ERROR(
      JniUtil::GetGlobalClassRef(env,
          "org/apache/hadoop/hbase/filter/SingleColumnValueFilter",
          &single_column_value_filter_cl_));
  RETURN_IF_ERROR(
      JniUtil::GetGlobalClassRef(env,
          "org/apache/hadoop/hbase/filter/CompareFilter$CompareOp",
          &compare_op_cl_));

  // Distinguish HBase versions by checking for the existence of the Cell class.
  // HBase 0.95.2: Use Cell class and corresponding methods.
  // HBase prior to 0.95.2: Use the KeyValue class and Cell-equivalent methods.
  bool has_cell_class = true;
  Status status =
      JniUtil::GetGlobalClassRef(env, "org/apache/hadoop/hbase/Cell", &cell_cl_);
  if (!status.ok()) {
    // Assume a non-CDH5 HBase version because the Cell class wasn't found.
    RETURN_IF_ERROR(
        JniUtil::GetGlobalClassRef(env, "org/apache/hadoop/hbase/KeyValue",
            &cell_cl_));
    has_cell_class = false;
  }

  // Scan method ids.
  scan_ctor_ = env->GetMethodID(scan_cl_, "<init>", "()V");
  RETURN_ERROR_IF_EXC(env);
  scan_set_max_versions_id_ = env->GetMethodID(scan_cl_, "setMaxVersions",
      "(I)Lorg/apache/hadoop/hbase/client/Scan;");
  RETURN_ERROR_IF_EXC(env);
  scan_set_caching_id_ = env->GetMethodID(scan_cl_, "setCaching", "(I)V");
  RETURN_ERROR_IF_EXC(env);
  scan_set_cache_blocks_id_ = env->GetMethodID(scan_cl_, "setCacheBlocks", "(Z)V");
  RETURN_ERROR_IF_EXC(env);
  scan_add_column_id_ = env->GetMethodID(scan_cl_, "addColumn",
      "([B[B)Lorg/apache/hadoop/hbase/client/Scan;");
  RETURN_ERROR_IF_EXC(env);
  scan_set_filter_id_ = env->GetMethodID(scan_cl_, "setFilter",
      "(Lorg/apache/hadoop/hbase/filter/Filter;)Lorg/apache/hadoop/hbase/client/Scan;");
  RETURN_ERROR_IF_EXC(env);
  scan_set_start_row_id_ = env->GetMethodID(scan_cl_, "setStartRow",
      "([B)Lorg/apache/hadoop/hbase/client/Scan;");
  RETURN_ERROR_IF_EXC(env);
  scan_set_stop_row_id_ = env->GetMethodID(scan_cl_, "setStopRow",
      "([B)Lorg/apache/hadoop/hbase/client/Scan;");
  RETURN_ERROR_IF_EXC(env);

  // ResultScanner method ids.
  resultscanner_next_id_ = env->GetMethodID(resultscanner_cl_, "next",
      "()Lorg/apache/hadoop/hbase/client/Result;");
  RETURN_ERROR_IF_EXC(env);
  resultscanner_close_id_ = env->GetMethodID(resultscanner_cl_, "close", "()V");
  RETURN_ERROR_IF_EXC(env);

  // Result method ids.
  if (has_cell_class) {
    result_raw_id_ = env->GetMethodID(result_cl_, "raw",
        "()[Lorg/apache/hadoop/hbase/Cell;");
  } else {
    result_raw_id_ = env->GetMethodID(result_cl_, "raw",
        "()[Lorg/apache/hadoop/hbase/KeyValue;");
  }
  RETURN_ERROR_IF_EXC(env);

  // Cell or equivalent KeyValue method ids.
  // Method ids to retrieve buffers backing different portions of row data.
  if (has_cell_class) {
    cell_get_row_array_ = env->GetMethodID(cell_cl_, "getRowArray", "()[B");
    RETURN_ERROR_IF_EXC(env);
    cell_get_family_array_ = env->GetMethodID(cell_cl_, "getFamilyArray", "()[B");
    RETURN_ERROR_IF_EXC(env);
    cell_get_qualifier_array_ = env->GetMethodID(cell_cl_, "getQualifierArray", "()[B");
    RETURN_ERROR_IF_EXC(env);
    cell_get_value_array_ = env->GetMethodID(cell_cl_, "getValueArray", "()[B");
    RETURN_ERROR_IF_EXC(env);
  } else {
    // In HBase versions prior to 0.95.2 all data from a row is backed by the same buffer
    cell_get_row_array_ = cell_get_family_array_ =
        cell_get_qualifier_array_ = cell_get_value_array_ =
        env->GetMethodID(cell_cl_, "getBuffer", "()[B");
    RETURN_ERROR_IF_EXC(env);
  }
  // Method ids for retrieving lengths and offsets into buffers backing different
  // portions of row data. Both the Cell and KeyValue classes support these methods.
  cell_get_family_offset_id_ = env->GetMethodID(cell_cl_, "getFamilyOffset", "()I");
  RETURN_ERROR_IF_EXC(env);
  cell_get_family_length_id_ = env->GetMethodID(cell_cl_, "getFamilyLength", "()B");
  RETURN_ERROR_IF_EXC(env);
  cell_get_qualifier_offset_id_ =
      env->GetMethodID(cell_cl_, "getQualifierOffset", "()I");
  RETURN_ERROR_IF_EXC(env);
  cell_get_qualifier_length_id_ =
      env->GetMethodID(cell_cl_, "getQualifierLength", "()I");
  RETURN_ERROR_IF_EXC(env);
  cell_get_row_offset_id_ = env->GetMethodID(cell_cl_, "getRowOffset", "()I");
  RETURN_ERROR_IF_EXC(env);
  cell_get_row_length_id_ = env->GetMethodID(cell_cl_, "getRowLength", "()S");
  RETURN_ERROR_IF_EXC(env);
  cell_get_value_offset_id_ = env->GetMethodID(cell_cl_, "getValueOffset", "()I");
  RETURN_ERROR_IF_EXC(env);
  cell_get_value_length_id_ = env->GetMethodID(cell_cl_, "getValueLength", "()I");
  RETURN_ERROR_IF_EXC(env);

  // HConstants fields.
  jfieldID empty_start_row_id =
      env->GetStaticFieldID(hconstants_cl_, "EMPTY_START_ROW", "[B");
  RETURN_ERROR_IF_EXC(env);
  empty_row_ = env->GetStaticObjectField(hconstants_cl_, empty_start_row_id);
  RETURN_IF_ERROR(JniUtil::LocalToGlobalRef(env, empty_row_, &empty_row_));

  // FilterList method ids.
  filter_list_ctor_ = env->GetMethodID(filter_list_cl_, "<init>",
      "(Lorg/apache/hadoop/hbase/filter/FilterList$Operator;)V");
  RETURN_ERROR_IF_EXC(env);
  filter_list_add_filter_id_ = env->GetMethodID(filter_list_cl_, "addFilter",
      "(Lorg/apache/hadoop/hbase/filter/Filter;)V");
  RETURN_ERROR_IF_EXC(env);

  // FilterList.Operator fields.
  jfieldID must_pass_all_id = env->GetStaticFieldID(filter_list_op_cl_, "MUST_PASS_ALL",
      "Lorg/apache/hadoop/hbase/filter/FilterList$Operator;");
  RETURN_ERROR_IF_EXC(env);
  must_pass_all_op_ = env->GetStaticObjectField(filter_list_op_cl_, must_pass_all_id);
  RETURN_IF_ERROR(JniUtil::LocalToGlobalRef(env, must_pass_all_op_, &must_pass_all_op_));

  // SingleColumnValueFilter method ids.
  single_column_value_filter_ctor_ =
      env->GetMethodID(single_column_value_filter_cl_, "<init>",
          "([B[BLorg/apache/hadoop/hbase/filter/CompareFilter$CompareOp;[B)V");
  RETURN_ERROR_IF_EXC(env);

  // Get op array from CompareFilter.CompareOp.
  jmethodID compare_op_values = env->GetStaticMethodID(compare_op_cl_, "values",
      "()[Lorg/apache/hadoop/hbase/filter/CompareFilter$CompareOp;");
  RETURN_ERROR_IF_EXC(env);
  compare_ops_ =
      (jobjectArray) env->CallStaticObjectMethod(compare_op_cl_, compare_op_values);
  RETURN_IF_ERROR(JniUtil::LocalToGlobalRef(env, reinterpret_cast<jobject>(compare_ops_),
      reinterpret_cast<jobject*>(&compare_ops_)));

  return Status::OK;
}

Status HBaseTableScanner::ScanSetup(JNIEnv* env, const TupleDescriptor* tuple_desc,
    const vector<THBaseFilter>& filters) {
  SCOPED_TIMER(scan_setup_timer_);
  JniLocalFrame jni_frame;
  RETURN_IF_ERROR(jni_frame.push(env));

  const HBaseTableDescriptor* hbase_table =
      static_cast<const HBaseTableDescriptor*>(tuple_desc->table_desc());
  // Use global cache of HTables.
  RETURN_IF_ERROR(htable_factory_->GetTable(hbase_table->table_name(),
      &htable_));

  // Setup an Scan object without the range
  // scan_ = new Scan();
  DCHECK(scan_ == NULL);
  scan_ = env->NewObject(scan_cl_, scan_ctor_);
  RETURN_ERROR_IF_EXC(env);
  scan_ = env->NewGlobalRef(scan_);

  // scan_.setMaxVersions(1);
  env->CallObjectMethod(scan_, scan_set_max_versions_id_, 1);
  RETURN_ERROR_IF_EXC(env);

  // scan_.setCaching(rows_cached_);
  env->CallObjectMethod(scan_, scan_set_caching_id_, rows_cached_);
  RETURN_ERROR_IF_EXC(env);

  // scan_.setCacheBlocks(cache_blocks_);
  env->CallObjectMethod(scan_, scan_set_cache_blocks_id_, cache_blocks_);
  RETURN_ERROR_IF_EXC(env);

  const vector<SlotDescriptor*>& slots = tuple_desc->slots();
  // Restrict scan to materialized families/qualifiers.
  for (int i = 0; i < slots.size(); ++i) {
    if (!slots[i]->is_materialized()) continue;
    const string& family = hbase_table->cols()[slots[i]->col_pos()].family;
    const string& qualifier = hbase_table->cols()[slots[i]->col_pos()].qualifier;
    // The row key has an empty qualifier.
    if (qualifier.empty()) continue;
    JniLocalFrame jni_frame;
    RETURN_IF_ERROR(jni_frame.push(env));
    jbyteArray family_bytes;
    RETURN_IF_ERROR(CreateByteArray(env, family, &family_bytes));
    jbyteArray qualifier_bytes;
    RETURN_IF_ERROR(CreateByteArray(env, qualifier, &qualifier_bytes));
    // scan_.addColumn(family_bytes, qualifier_bytes);
    env->CallObjectMethod(scan_, scan_add_column_id_, family_bytes, qualifier_bytes);
    RETURN_ERROR_IF_EXC(env);
  }

  // circumvent hbase bug: make sure to select all cols that have filters,
  // otherwise the filter may not get applied;
  // see HBASE-4364 (https://issues.apache.org/jira/browse/HBASE-4364)
  num_addl_requested_cols_ = 0;
  for (vector<THBaseFilter>::const_iterator it = filters.begin(); it != filters.end();
       ++it) {
    bool requested = false;
    for (int i = 0; i < slots.size(); ++i) {
      if (!slots[i]->is_materialized()) continue;
      const string& family = hbase_table->cols()[slots[i]->col_pos()].family;
      const string& qualifier = hbase_table->cols()[slots[i]->col_pos()].qualifier;
      if (family == it->family && qualifier == it->qualifier) {
        requested = true;
        break;
      }
    }
    if (requested) continue;
    JniLocalFrame jni_frame;
    RETURN_IF_ERROR(jni_frame.push(env));
    jbyteArray family_bytes;
    RETURN_IF_ERROR(CreateByteArray(env, it->family, &family_bytes));
    jbyteArray qualifier_bytes;
    RETURN_IF_ERROR(CreateByteArray(env, it->qualifier, &qualifier_bytes));
    // scan_.addColumn(family_bytes, qualifier_bytes);
    env->CallObjectMethod(scan_, scan_add_column_id_, family_bytes, qualifier_bytes);
    RETURN_ERROR_IF_EXC(env);
    ++num_addl_requested_cols_;
  }

  // Add HBase Filters.
  if (!filters.empty()) {
    // filter_list = new FilterList(Operator.MUST_PASS_ALL);
    jobject filter_list =
        env->NewObject(filter_list_cl_, filter_list_ctor_, must_pass_all_op_);
    RETURN_ERROR_IF_EXC(env);
    vector<THBaseFilter>::const_iterator it;
    for (it = filters.begin(); it != filters.end(); ++it) {
      JniLocalFrame jni_frame;
      RETURN_IF_ERROR(jni_frame.push(env));
      // hbase_op = CompareFilter.CompareOp.values()[it->op_ordinal];
      jobject hbase_op = env->GetObjectArrayElement(compare_ops_, it->op_ordinal);
      RETURN_ERROR_IF_EXC(env);
      jbyteArray family_bytes;
      RETURN_IF_ERROR(CreateByteArray(env, it->family, &family_bytes));
      jbyteArray qualifier_bytes;
      RETURN_IF_ERROR(CreateByteArray(env, it->qualifier, &qualifier_bytes));
      jbyteArray value_bytes;
      RETURN_IF_ERROR(CreateByteArray(env, it->filter_constant, &value_bytes));
      // filter = new SingleColumnValueFilter(family_bytes, qualifier_bytes, hbase_op,
      //     value_bytes);
      jobject filter = env->NewObject(single_column_value_filter_cl_,
          single_column_value_filter_ctor_, family_bytes, qualifier_bytes, hbase_op,
          value_bytes);
      RETURN_ERROR_IF_EXC(env);
      // filter_list.add(filter);
      env->CallBooleanMethod(filter_list, filter_list_add_filter_id_, filter);
      RETURN_ERROR_IF_EXC(env);
    }
    // scan.setFilter(filter_list);
    env->CallObjectMethod(scan_, scan_set_filter_id_, filter_list);
    RETURN_ERROR_IF_EXC(env);
  }

  return Status::OK;
}

Status HBaseTableScanner::InitScanRange(JNIEnv* env, const ScanRange& scan_range) {
  JniLocalFrame jni_frame;
  RETURN_IF_ERROR(jni_frame.push(env));
  jbyteArray start_bytes;
  CreateByteArray(env, scan_range.start_key(), &start_bytes);
  jbyteArray end_bytes;
  CreateByteArray(env, scan_range.stop_key(), &end_bytes);

  // scan_.setStartRow(start_bytes);
  env->CallObjectMethod(scan_, scan_set_start_row_id_, start_bytes);
  RETURN_ERROR_IF_EXC(env);

  // scan_.setStopRow(end_bytes);
  env->CallObjectMethod(scan_, scan_set_stop_row_id_, end_bytes);
  RETURN_ERROR_IF_EXC(env);

  // resultscanner_ = htable_.getScanner(scan_);
  if (resultscanner_ != NULL) env->DeleteGlobalRef(resultscanner_);
  RETURN_IF_ERROR(htable_->GetResultScanner(scan_, &resultscanner_));

  resultscanner_ = env->NewGlobalRef(resultscanner_);

  RETURN_ERROR_IF_EXC(env);
  return Status::OK;
}

Status HBaseTableScanner::StartScan(JNIEnv* env, const TupleDescriptor* tuple_desc,
    const ScanRangeVector& scan_range_vector, const vector<THBaseFilter>& filters) {
  // Setup the scan without ranges first
  RETURN_IF_ERROR(ScanSetup(env, tuple_desc, filters));

  // Record the ranges
  scan_range_vector_ = &scan_range_vector;
  current_scan_range_idx_ = 0;

  // Now, scan the first range (we should have at least one range.)
  return InitScanRange(env, scan_range_vector_->at(current_scan_range_idx_));
}

Status HBaseTableScanner::CreateByteArray(JNIEnv* env, const string& s,
    jbyteArray* bytes) {
  if (!s.empty()) {
    *bytes = env->NewByteArray(s.size());
    if (*bytes == NULL) {
      return Status("Couldn't construct java byte array for key " + s);
    }
    env->SetByteArrayRegion(*bytes, 0, s.size(),
        reinterpret_cast<const jbyte*>(s.data()));
  } else {
    *bytes = reinterpret_cast<jbyteArray>(empty_row_);
  }
  return Status::OK;
}

Status HBaseTableScanner::Next(JNIEnv* env, bool* has_next) {
  JniLocalFrame jni_frame;
  RETURN_IF_ERROR(jni_frame.push(env));
  jobject result = NULL;
  {
    SCOPED_TIMER(scan_node_->read_timer());
    while (true) {
      // result_ = resultscanner_.next();
      result = env->CallObjectMethod(resultscanner_, resultscanner_next_id_);

      // jump to the next region when finished with the current region.
      if (result == NULL &&
          current_scan_range_idx_ + 1 < scan_range_vector_->size()) {
        ++current_scan_range_idx_;
        RETURN_IF_ERROR(InitScanRange(env,
            scan_range_vector_->at(current_scan_range_idx_)));
        continue;
      }
      break;
    }
  }

  if (result == NULL) {
    *has_next = false;
    return Status::OK;
  }

  if (cells_ != NULL) env->DeleteGlobalRef(cells_);
  // cells_ = result.raw();
  cells_ =
      reinterpret_cast<jobjectArray>(env->CallObjectMethod(result, result_raw_id_));
  cells_ = reinterpret_cast<jobjectArray>(env->NewGlobalRef(cells_));
  num_cells_ = env->GetArrayLength(cells_);
  // Check that raw() didn't return more cells than expected.
  // If num_requested_cells_ is 0 then only row key is asked for and this check
  // should pass.
  if (num_cells_ > num_requested_cells_ + num_addl_requested_cols_
      && num_requested_cells_ + num_addl_requested_cols_ != 0) {
    *has_next = false;
    return Status("Encountered more cells than expected.");
  }
  // If all requested columns are present, and we didn't ask for any extra ones to work
  // around an hbase bug, we avoid family-/qualifier comparisons in NextValue().
  if (num_cells_ == num_requested_cells_ && num_addl_requested_cols_ == 0) {
    all_cells_present_ = true;
  } else {
    all_cells_present_ = false;
  }
  cell_index_ = 0;

  value_pool_->Clear();
  *has_next = true;
  return Status::OK;
}

inline void HBaseTableScanner::WriteTupleSlot(const SlotDescriptor* slot_desc,
    Tuple* tuple, void* data) {
  void* slot = tuple->GetSlot(slot_desc->tuple_offset());
  BitUtil::ByteSwap(slot, data, GetByteSize(slot_desc->type()));
}

inline void HBaseTableScanner::GetRowKey(JNIEnv* env, jobject cell,
    void** data, int* length) {
  int offset = env->CallIntMethod(cell, cell_get_row_offset_id_);
  *length = env->CallShortMethod(cell, cell_get_row_length_id_);
  jbyteArray jdata =
      (jbyteArray) env->CallObjectMethod(cell, cell_get_row_array_);
  *data = value_pool_->Allocate(*length);
  env->GetByteArrayRegion(jdata, offset, *length, reinterpret_cast<jbyte*>(*data));
  COUNTER_UPDATE(scan_node_->bytes_read_counter(), *length);
}

inline void HBaseTableScanner::GetFamily(JNIEnv* env, jobject cell,
    void** data, int* length) {
  int offset = env->CallIntMethod(cell, cell_get_family_offset_id_);
  *length = env->CallShortMethod(cell, cell_get_family_length_id_);
  jbyteArray jdata =
      (jbyteArray) env->CallObjectMethod(cell, cell_get_family_array_);
  *data = value_pool_->Allocate(*length);
  env->GetByteArrayRegion(jdata, offset, *length, reinterpret_cast<jbyte*>(*data));
  COUNTER_UPDATE(scan_node_->bytes_read_counter(), *length);
}

inline void HBaseTableScanner::GetQualifier(JNIEnv* env, jobject cell,
    void** data, int* length) {
  int offset = env->CallIntMethod(cell, cell_get_qualifier_offset_id_);
  *length = env->CallShortMethod(cell, cell_get_qualifier_length_id_);
  jbyteArray jdata =
      (jbyteArray) env->CallObjectMethod(cell, cell_get_qualifier_array_);
  *data = value_pool_->Allocate(*length);
  env->GetByteArrayRegion(jdata, offset, *length, reinterpret_cast<jbyte*>(*data));
  COUNTER_UPDATE(scan_node_->bytes_read_counter(), *length);
}

inline void HBaseTableScanner::GetValue(JNIEnv* env, jobject cell,
    void** data, int* length) {
  int offset = env->CallIntMethod(cell, cell_get_value_offset_id_);
  *length = env->CallShortMethod(cell, cell_get_value_length_id_);
  jbyteArray jdata =
      (jbyteArray) env->CallObjectMethod(cell, cell_get_value_array_);
  *data = value_pool_->Allocate(*length);
  env->GetByteArrayRegion(jdata, offset, *length, reinterpret_cast<jbyte*>(*data));
  COUNTER_UPDATE(scan_node_->bytes_read_counter(), *length);
}

Status HBaseTableScanner::GetRowKey(JNIEnv* env, void** key, int* key_length) {
  jobject cell = env->GetObjectArrayElement(cells_, 0);
  GetRowKey(env, cell, key, key_length);
  RETURN_ERROR_IF_EXC(env);
  return Status::OK;
}

Status HBaseTableScanner::GetRowKey(JNIEnv* env, const SlotDescriptor* slot_desc,
    Tuple* tuple) {
  void* key;
  int key_length;
  jobject cell = env->GetObjectArrayElement(cells_, 0);
  GetRowKey(env, cell, &key, &key_length);
  DCHECK_EQ(key_length, GetByteSize(slot_desc->type()));
  WriteTupleSlot(slot_desc, tuple, reinterpret_cast<char*>(key));
  RETURN_ERROR_IF_EXC(env);
  return Status::OK;
}

Status HBaseTableScanner::GetCurrentValue(JNIEnv* env, const string& family,
    const string& qualifier, void** data, int* length, bool* is_null) {
  // Current row doesn't have any more cells. All remaining values are NULL.
  if (cell_index_ >= num_cells_) {
    *is_null = true;
    return Status::OK;
  }
  JniLocalFrame jni_frame;
  RETURN_IF_ERROR(jni_frame.push(env));
  jobject cell = env->GetObjectArrayElement(cells_, cell_index_);
  if (!all_cells_present_) {
    // Check family. If it doesn't match, we have a NULL value.
    void* family_data;
    int family_length;
    GetFamily(env, cell, &family_data, &family_length);
    if (CompareStrings(family, family_data, family_length) != 0) {
      *is_null = true;
      return Status::OK;
    }

    // Check qualifier. If it doesn't match, we have a NULL value.
    void* qualifier_data;
    int qualifier_length;
    GetQualifier(env, cell, &qualifier_data, &qualifier_length);
    if (CompareStrings(qualifier, qualifier_data, qualifier_length) != 0) {
      *is_null = true;
      return Status::OK;
    }
  }
  GetValue(env, cell, data, length);
  *is_null = false;
  return Status::OK;
}

Status HBaseTableScanner::GetValue(JNIEnv* env, const string& family,
    const string& qualifier, void** value, int* value_length) {
  bool is_null;
  GetCurrentValue(env, family, qualifier, value, value_length, &is_null);
  RETURN_ERROR_IF_EXC(env);
  if (is_null) {
    *value = NULL;
    *value_length = 0;
    return Status::OK;
  }
  ++cell_index_;
  return Status::OK;
}

Status HBaseTableScanner::GetValue(JNIEnv* env, const string& family,
    const string& qualifier, const SlotDescriptor* slot_desc, Tuple* tuple) {
  void* value;
  int value_length;
  bool is_null;
  GetCurrentValue(env, family, qualifier, &value, &value_length, &is_null);
  RETURN_ERROR_IF_EXC(env);
  if (is_null) {
    tuple->SetNull(slot_desc->null_indicator_offset());
    return Status::OK;
  }
  DCHECK_EQ(value_length, GetByteSize(slot_desc->type()));
  WriteTupleSlot(slot_desc, tuple, reinterpret_cast<char*>(value));
  ++cell_index_;
  return Status::OK;
}

int HBaseTableScanner::CompareStrings(const string& s, void* data, int length) {
  int slength = static_cast<int>(s.length());
  if (slength == 0 && length == 0) return 0;
  if (length == 0) return 1;
  if (slength == 0) return -1;
  int result = memcmp(s.data(), reinterpret_cast<char*>(data), min(slength, length));
  if (result == 0 && slength != length) {
    return (slength < length ? -1 : 1);
  } else {
    return result;
  }
}

void HBaseTableScanner::Close(JNIEnv* env) {
  if (resultscanner_ != NULL) {
    // resultscanner_.close();
    env->CallObjectMethod(resultscanner_, resultscanner_close_id_);
    env->DeleteGlobalRef(resultscanner_);
  }
  if (scan_ != NULL) env->DeleteGlobalRef(scan_);
  if (cells_ != NULL) env->DeleteGlobalRef(cells_);

  // Close the HTable so that the connections are not kept around.
  if (htable_.get() != NULL) htable_->Close(state_);

  value_pool_->FreeAll();
}
