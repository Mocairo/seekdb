/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/code_generator/ob_static_engine_cg.h"
#include "sql/das/ob_das_scan_op.h"
#include "sql/optimizer/ob_optimizer_context.h"
#include "sql/optimizer/ob_select_log_plan.h"

namespace oceanbase
{
namespace sql
{
class TscCgServiceTestPeer
{
public:
  static int extract_columns(ObTscCgService &service, const ObLogTableScan &op,
      const ObTextRetrievalInfo &info, const ObDASScanCtDef &scan,
      common::ObIArray<ObRawExpr *> &access, common::ObIArray<uint64_t> &output)
  {
    int ret = service.extract_text_ir_access_columns(op, info, scan, access);
    if (OB_SUCC(ret)) {
      ret = service.extract_text_ir_das_output_column_ids(info, scan, output);
    }
    return ret;
  }
};

namespace
{
using namespace common;

class FtsScanColumns : public testing::Test
{
protected:
  FtsScanColumns()
    : allocator_(), expr_factory_(allocator_), stmt_(), global_hint_(), address_(),
      context_(nullptr, nullptr, nullptr, nullptr, allocator_, nullptr, address_,
          global_hint_, expr_factory_, &stmt_, false),
      plan_(context_, &stmt_), table_scan_(plan_), cg_(), service_(cg_)
  {
    token_.set_ref_id(50001, 16);
    frequency_.set_ref_id(50001, 17);
    id_.set_ref_id(50001, 18);
    length_.set_ref_id(50001, 19);
  }

  void check_inv_scan(const bool scoring)
  {
    ObTextRetrievalInfo info;
    info.need_calc_relevance_ = scoring;
    info.docid_or_rowkey_column_ = &id_;
    // Leaving the unused expressions null also verifies that membership does
    // not depend on a scoring column being present in the scan.
    info.token_cnt_column_ = scoring ? &frequency_ : nullptr;
    info.doc_length_column_ = scoring ? &length_ : nullptr;
    ObDASScanCtDef scan(allocator_);
    scan.ref_table_id_ = 50001;
    scan.ir_scan_type_ = ObTSCIRScanType::OB_IR_INV_IDX_SCAN;
    ObSEArray<ObRawExpr *, 4> access;
    ObSEArray<uint64_t, 4> output;
    ASSERT_EQ(OB_SUCCESS, TscCgServiceTestPeer::extract_columns(
        service_, table_scan_, info, scan, access, output));
    const int64_t expected_count = scoring ? 3 : 1;
    ASSERT_EQ(expected_count, access.count());
    ASSERT_EQ(expected_count, output.count());
    ObRawExpr *expected[] = {scoring ? &frequency_ : &id_, &id_, &length_};
    ObExpr rt_exprs[3];
    ASSERT_EQ(OB_SUCCESS, scan.access_column_ids_.init(expected_count));
    ASSERT_EQ(OB_SUCCESS, scan.pd_expr_spec_.access_exprs_.init(expected_count));
    for (int64_t i = 0; i < expected_count; ++i) {
      EXPECT_EQ(expected[i], access.at(i));
      const uint64_t column_id = static_cast<ObColumnRefRawExpr *>(expected[i])->get_column_id();
      EXPECT_EQ(column_id, output.at(i));
      ASSERT_EQ(OB_SUCCESS, scan.access_column_ids_.push_back(column_id));
      ASSERT_EQ(OB_SUCCESS, scan.pd_expr_spec_.access_exprs_.push_back(&rt_exprs[i]));
    }
    ObSEArray<ObExpr *, 1> domain_exprs;
    ObSEArray<uint64_t, 1> domain_ids;
    ASSERT_EQ(OB_SUCCESS, service_.generate_das_result_output(
        output, domain_exprs, domain_ids, scan, nullptr));
    ASSERT_EQ(expected_count, scan.result_output_.count());
    for (int64_t i = 0; i < expected_count; ++i) {
      EXPECT_EQ(&rt_exprs[i], scan.result_output_.at(i));
    }
  }

  ObArenaAllocator allocator_;
  ObRawExprFactory expr_factory_;
  ObSelectStmt stmt_;
  ObGlobalHint global_hint_;
  ObAddr address_;
  ObOptimizerContext context_;
  ObSelectLogPlan plan_;
  ObLogTableScan table_scan_;
  ObStaticEngineCG cg_;
  ObTscCgService service_;
  ObColumnRefRawExpr token_;
  ObColumnRefRawExpr frequency_;
  ObColumnRefRawExpr id_;
  ObColumnRefRawExpr length_;
};

TEST_F(FtsScanColumns, membership_scan_reads_and_outputs_only_id)
{
  check_inv_scan(false);
}

TEST_F(FtsScanColumns, ranked_scan_retains_frequency_id_and_length)
{
  check_inv_scan(true);
}

TEST_F(FtsScanColumns, block_max_scan_retains_all_score_bound_columns)
{
  ObTextRetrievalInfo info;
  info.need_calc_relevance_ = false;
  info.token_column_ = &token_;
  info.token_cnt_column_ = &frequency_;
  info.docid_or_rowkey_column_ = &id_;
  info.doc_length_column_ = &length_;
  ObDASScanCtDef scan(allocator_);
  scan.ref_table_id_ = 50001;
  scan.ir_scan_type_ = ObTSCIRScanType::OB_IR_BLOCK_MAX_SCAN;
  ObSEArray<ObRawExpr *, 4> access;
  ObSEArray<uint64_t, 4> output;
  ASSERT_EQ(OB_SUCCESS, TscCgServiceTestPeer::extract_columns(
      service_, table_scan_, info, scan, access, output));
  ASSERT_EQ(4, access.count());
  EXPECT_EQ(&token_, access.at(0));
  EXPECT_EQ(&id_, access.at(1));
  EXPECT_EQ(&frequency_, access.at(2));
  EXPECT_EQ(&length_, access.at(3));
  EXPECT_TRUE(output.empty());
}
} // namespace
} // namespace sql
} // namespace oceanbase
