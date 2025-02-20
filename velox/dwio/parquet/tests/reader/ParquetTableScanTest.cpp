/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/init/Init.h>

#include "velox/common/base/tests/GTestUtils.h"
#include "velox/dwio/common/tests/utils/DataFiles.h"
#include "velox/dwio/parquet/RegisterParquetReader.h"
#include "velox/dwio/parquet/reader/ParquetReader.h"
#include "velox/exec/tests/utils/HiveConnectorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/type/tests/SubfieldFiltersBuilder.h"

#include "velox/connectors/hive/HiveConfig.h"

using namespace facebook::velox;
using namespace facebook::velox::exec;
using namespace facebook::velox::exec::test;
using namespace facebook::velox::parquet;

class ParquetTableScanTest : public HiveConnectorTestBase {
 protected:
  using OperatorTestBase::assertQuery;

  void SetUp() {
    registerParquetReaderFactory();

    auto hiveConnector =
        connector::getConnectorFactory(
            connector::hive::HiveConnectorFactory::kHiveConnectorName)
            ->newConnector(
                kHiveConnectorId, std::make_shared<core::MemConfig>());
    connector::registerConnector(hiveConnector);
  }

  void assertSelect(
      std::vector<std::string>&& outputColumnNames,
      const std::string& sql) {
    auto rowType = getRowType(std::move(outputColumnNames));

    auto plan = PlanBuilder().tableScan(rowType).planNode();

    assertQuery(plan, splits_, sql);
  }

  void assertSelectWithFilter(
      std::vector<std::string>&& outputColumnNames,
      const std::vector<std::string>& subfieldFilters,
      const std::string& remainingFilter,
      const std::string& sql) {
    auto rowType = getRowType(std::move(outputColumnNames));
    parse::ParseOptions options;
    options.parseDecimalAsDouble = false;

    auto plan = PlanBuilder(pool_.get())
                    .setParseOptions(options)
                    .tableScan(rowType, subfieldFilters, remainingFilter)
                    .planNode();

    assertQuery(plan, splits_, sql);
  }

  void assertSelectWithFilter(
      std::vector<std::string>&& outputColumnNames,
      const std::vector<std::string>& subfieldFilters,
      const std::string& remainingFilter,
      const std::string& sql,
      bool isFilterPushdownEnabled) {
    auto rowType = getRowType(std::move(outputColumnNames));
    parse::ParseOptions options;
    options.parseDecimalAsDouble = false;

    auto plan = PlanBuilder(pool_.get())
                    .setParseOptions(options)
                    // Function extractFiltersFromRemainingFilter will extract
                    // filters to subfield filters, but for some types, filter
                    // pushdown is not supported.
                    .tableScan(
                        "hive_table",
                        rowType,
                        {},
                        subfieldFilters,
                        remainingFilter,
                        nullptr,
                        isFilterPushdownEnabled)
                    .planNode();

    assertQuery(plan, splits_, sql);
  }

  void assertSelectWithAgg(
      std::vector<std::string>&& outputColumnNames,
      const std::vector<std::string>& aggregates,
      const std::vector<std::string>& groupingKeys,
      const std::string& sql) {
    auto rowType = getRowType(std::move(outputColumnNames));

    auto plan = PlanBuilder()
                    .tableScan(rowType)
                    .singleAggregation(groupingKeys, aggregates)
                    .planNode();

    assertQuery(plan, splits_, sql);
  }

  void assertSelectWithFilterAndAgg(
      std::vector<std::string>&& outputColumnNames,
      const std::vector<std::string>& filters,
      const std::vector<std::string>& aggregates,
      const std::vector<std::string>& groupingKeys,
      const std::string& sql) {
    auto rowType = getRowType(std::move(outputColumnNames));

    auto plan = PlanBuilder()
                    .tableScan(rowType, filters)
                    .singleAggregation(groupingKeys, aggregates)
                    .planNode();

    assertQuery(plan, splits_, sql);
  }

  void
  loadData(const std::string& filePath, RowTypePtr rowType, RowVectorPtr data) {
    splits_ = {makeSplit(filePath)};
    rowType_ = rowType;
    createDuckDbTable({data});
  }

  std::string getExampleFilePath(const std::string& fileName) {
    return facebook::velox::test::getDataFilePath(
        "velox/dwio/parquet/tests/reader", "../examples/" + fileName);
  }

  std::shared_ptr<connector::hive::HiveConnectorSplit> makeSplit(
      const std::string& filePath) {
    return makeHiveConnectorSplits(
        filePath, 1, dwio::common::FileFormat::PARQUET)[0];
  }

 private:
  RowTypePtr getRowType(std::vector<std::string>&& outputColumnNames) const {
    std::vector<TypePtr> types;
    for (auto colName : outputColumnNames) {
      types.push_back(rowType_->findChild(colName));
    }

    return ROW(std::move(outputColumnNames), std::move(types));
  }

  RowTypePtr rowType_;
  std::vector<std::shared_ptr<connector::ConnectorSplit>> splits_;
};

TEST_F(ParquetTableScanTest, basic) {
  loadData(
      getExampleFilePath("sample.parquet"),
      ROW({"a", "b"}, {BIGINT(), DOUBLE()}),
      makeRowVector(
          {"a", "b"},
          {
              makeFlatVector<int64_t>(20, [](auto row) { return row + 1; }),
              makeFlatVector<double>(20, [](auto row) { return row + 1; }),
          }));

  // Plain select.
  assertSelect({"a"}, "SELECT a FROM tmp");
  assertSelect({"b"}, "SELECT b FROM tmp");
  assertSelect({"a", "b"}, "SELECT a, b FROM tmp");
  assertSelect({"b", "a"}, "SELECT b, a FROM tmp");

  // With filters.
  assertSelectWithFilter({"a"}, {"a < 3"}, "", "SELECT a FROM tmp WHERE a < 3");
  assertSelectWithFilter(
      {"a", "b"}, {"a < 3"}, "", "SELECT a, b FROM tmp WHERE a < 3");
  assertSelectWithFilter(
      {"b", "a"}, {"a < 3"}, "", "SELECT b, a FROM tmp WHERE a < 3");
  assertSelectWithFilter(
      {"a", "b"}, {"a < 0"}, "", "SELECT a, b FROM tmp WHERE a < 0");

  assertSelectWithFilter(
      {"b"}, {"b < DOUBLE '2.0'"}, "", "SELECT b FROM tmp WHERE b < 2.0");
  assertSelectWithFilter(
      {"a", "b"},
      {"b >= DOUBLE '2.0'"},
      "",
      "SELECT a, b FROM tmp WHERE b >= 2.0");
  assertSelectWithFilter(
      {"b", "a"},
      {"b <= DOUBLE '2.0'"},
      "",
      "SELECT b, a FROM tmp WHERE b <= 2.0");
  assertSelectWithFilter(
      {"a", "b"},
      {"b < DOUBLE '0.0'"},
      "",
      "SELECT a, b FROM tmp WHERE b < 0.0");

  // With aggregations.
  assertSelectWithAgg({"a"}, {"sum(a)"}, {}, "SELECT sum(a) FROM tmp");
  assertSelectWithAgg({"b"}, {"max(b)"}, {}, "SELECT max(b) FROM tmp");
  assertSelectWithAgg(
      {"a", "b"}, {"min(a)", "max(b)"}, {}, "SELECT min(a), max(b) FROM tmp");
  assertSelectWithAgg(
      {"b", "a"}, {"max(b)"}, {"a"}, "SELECT max(b), a FROM tmp GROUP BY a");
  assertSelectWithAgg(
      {"a", "b"}, {"max(a)"}, {"b"}, "SELECT max(a), b FROM tmp GROUP BY b");

  // With filter and aggregation.
  assertSelectWithFilterAndAgg(
      {"a"}, {"a < 3"}, {"sum(a)"}, {}, "SELECT sum(a) FROM tmp WHERE a < 3");
  assertSelectWithFilterAndAgg(
      {"a", "b"},
      {"a < 3"},
      {"sum(b)"},
      {},
      "SELECT sum(b) FROM tmp WHERE a < 3");
  assertSelectWithFilterAndAgg(
      {"a", "b"},
      {"a < 3"},
      {"min(a)", "max(b)"},
      {},
      "SELECT min(a), max(b) FROM tmp WHERE a < 3");
  assertSelectWithFilterAndAgg(
      {"b", "a"},
      {"a < 3"},
      {"max(b)"},
      {"a"},
      "SELECT max(b), a FROM tmp WHERE a < 3 GROUP BY a");
}

TEST_F(ParquetTableScanTest, countStar) {
  // sample.parquet holds two columns (a: BIGINT, b: DOUBLE) and
  // 20 rows.
  auto filePath = getExampleFilePath("sample.parquet");
  auto split = makeSplit(filePath);

  // Output type does not have any columns.
  auto rowType = ROW({}, {});
  auto plan = PlanBuilder()
                  .tableScan(rowType)
                  .singleAggregation({}, {"count(0)"})
                  .planNode();

  assertQuery(plan, {split}, "SELECT 20");
}

TEST_F(ParquetTableScanTest, decimalSubfieldFilter) {
  // decimal.parquet holds two columns (a: DECIMAL(5, 2), b: DECIMAL(20, 5)) and
  // 20 rows (10 rows per group). Data is in plain uncompressed format:
  //   a: [100.01 .. 100.20]
  //   b: [100000000000000.00001 .. 100000000000000.00020]
  std::vector<int64_t> unscaledShortValues(20);
  std::iota(unscaledShortValues.begin(), unscaledShortValues.end(), 10001);
  loadData(
      getExampleFilePath("decimal.parquet"),
      ROW({"a"}, {DECIMAL(5, 2)}),
      makeRowVector(
          {"a"},
          {
              makeFlatVector(unscaledShortValues, DECIMAL(5, 2)),
          }));

  assertSelectWithFilter(
      {"a"}, {"a < 100.07"}, "", "SELECT a FROM tmp WHERE a < 100.07");
  assertSelectWithFilter(
      {"a"}, {"a <= 100.07"}, "", "SELECT a FROM tmp WHERE a <= 100.07");
  assertSelectWithFilter(
      {"a"}, {"a > 100.07"}, "", "SELECT a FROM tmp WHERE a > 100.07");
  assertSelectWithFilter(
      {"a"}, {"a >= 100.07"}, "", "SELECT a FROM tmp WHERE a >= 100.07");
  assertSelectWithFilter(
      {"a"}, {"a = 100.07"}, "", "SELECT a FROM tmp WHERE a = 100.07");
  assertSelectWithFilter(
      {"a"},
      {"a BETWEEN 100.07 AND 100.12"},
      "",
      "SELECT a FROM tmp WHERE a BETWEEN 100.07 AND 100.12");

  VELOX_ASSERT_THROW(
      assertSelectWithFilter(
          {"a"}, {"a < 1000.7"}, "", "SELECT a FROM tmp WHERE a < 1000.7"),
      "Scalar function signature is not supported: lt(DECIMAL(5, 2), DECIMAL(5, 1))");
  VELOX_ASSERT_THROW(
      assertSelectWithFilter(
          {"a"}, {"a = 1000.7"}, "", "SELECT a FROM tmp WHERE a = 1000.7"),
      "Scalar function signature is not supported: eq(DECIMAL(5, 2), DECIMAL(5, 1))");
}

// Core dump is fixed.
TEST_F(ParquetTableScanTest, map) {
  auto vector = makeMapVector<StringView, StringView>({{{"name", "gluten"}}});

  loadData(
      getExampleFilePath("types.parquet"),
      ROW({"map"}, {MAP(VARCHAR(), VARCHAR())}),
      makeRowVector(
          {"map"},
          {
              vector,
          }));

  assertSelectWithFilter({"map"}, {}, "", "SELECT map FROM tmp");
}

// Core dump is fixed.
TEST_F(ParquetTableScanTest, singleRowStruct) {
  auto vector = makeArrayVector<int32_t>({{}});
  loadData(
      getExampleFilePath("single_row_struct.parquet"),
      ROW({"s"}, {ROW({"a", "b"}, {BIGINT(), BIGINT()})}),
      makeRowVector(
          {"s"},
          {
              vector,
          }));

  assertSelectWithFilter({"s"}, {}, "", "SELECT (0, 1)");
}

// Core dump and incorrect result are fixed.
TEST_F(ParquetTableScanTest, DISABLED_array) {
  auto vector = makeArrayVector<int32_t>({{1, 2, 3}});

  loadData(
      getExampleFilePath("old_repeated_int.parquet"),
      ROW({"repeatedInt"}, {ARRAY(INTEGER())}),
      makeRowVector(
          {"repeatedInt"},
          {
              vector,
          }));

  assertSelectWithFilter(
      {"repeatedInt"}, {}, "", "SELECT repeatedInt FROM tmp");
}

// Optional array with required elements.
// Incorrect result.
TEST_F(ParquetTableScanTest, DISABLED_optArrayReqEle) {
  auto vector = makeArrayVector<StringView>({});

  loadData(
      getExampleFilePath("array_0.parquet"),
      ROW({"_1"}, {ARRAY(VARCHAR())}),
      makeRowVector(
          {"_1"},
          {
              vector,
          }));

  assertSelectWithFilter(
      {"_1"},
      {},
      "",
      "SELECT UNNEST(array[array['a', 'b'], array['c', 'd'], array['e', 'f'], array[], null])");
}

// Required array with required elements.
// Core dump is fixed, but the result is incorrect.
TEST_F(ParquetTableScanTest, DISABLED_reqArrayReqEle) {
  auto vector = makeArrayVector<StringView>({});

  loadData(
      getExampleFilePath("array_1.parquet"),
      ROW({"_1"}, {ARRAY(VARCHAR())}),
      makeRowVector(
          {"_1"},
          {
              vector,
          }));

  assertSelectWithFilter(
      {"_1"},
      {},
      "",
      "SELECT UNNEST(array[array['a', 'b'], array['c', 'd'], array[]])");
}

// Required array with optional elements.
// Incorrect result.
TEST_F(ParquetTableScanTest, DISABLED_reqArrayOptEle) {
  auto vector = makeArrayVector<StringView>({});

  loadData(
      getExampleFilePath("array_2.parquet"),
      ROW({"_1"}, {ARRAY(VARCHAR())}),
      makeRowVector(
          {"_1"},
          {
              vector,
          }));

  assertSelectWithFilter(
      {"_1"},
      {},
      "",
      "SELECT UNNEST(array[array['a', null], array[], array[null, 'b']])");
}

// Required array with legacy format.
// Incorrect result.
TEST_F(ParquetTableScanTest, DISABLED_reqArrayLegacy) {
  auto vector = makeArrayVector<StringView>({});

  loadData(
      getExampleFilePath("array_3.parquet"),
      ROW({"_1"}, {ARRAY(VARCHAR())}),
      makeRowVector(
          {"_1"},
          {
              vector,
          }));

  assertSelectWithFilter(
      {"_1"},
      {},
      "",
      "SELECT UNNEST(array[array['a', 'b'], array[], array['c', 'd']])");
}

TEST_F(ParquetTableScanTest, readAsLowerCase) {
  auto plan = PlanBuilder(pool_.get())
                  .tableScan(ROW({"a"}, {BIGINT()}), {}, "")
                  .planNode();
  CursorParameters params;
  std::shared_ptr<folly::Executor> executor =
      std::make_shared<folly::CPUThreadPoolExecutor>(
          std::thread::hardware_concurrency());
  std::shared_ptr<core::QueryCtx> queryCtx =
      std::make_shared<core::QueryCtx>(executor.get());
  std::unordered_map<std::string, std::string> session = {
      {std::string(
           connector::hive::HiveConfig::kFileColumnNamesReadAsLowerCaseSession),
       "true"}};
  queryCtx->setConnectorSessionOverridesUnsafe(
      kHiveConnectorId, std::move(session));
  params.queryCtx = queryCtx;
  params.planNode = plan;
  const int numSplitsPerFile = 1;

  bool noMoreSplits = false;
  auto addSplits = [&](exec::Task* task) {
    if (!noMoreSplits) {
      auto const splits = HiveConnectorTestBase::makeHiveConnectorSplits(
          {getExampleFilePath("upper.parquet")},
          numSplitsPerFile,
          dwio::common::FileFormat::PARQUET);
      for (const auto& split : splits) {
        task->addSplit("0", exec::Split(split));
      }
      task->noMoreSplits("0");
    }
    noMoreSplits = true;
  };
  auto result = readCursor(params, addSplits);
  ASSERT_TRUE(waitForTaskCompletion(result.first->task().get()));
  assertEqualResults(
      result.second, {makeRowVector({"a"}, {makeFlatVector<int64_t>({0, 1})})});
}

TEST_F(ParquetTableScanTest, structSelection) {
  auto vector = makeArrayVector<StringView>({{}});
  loadData(
      getExampleFilePath("contacts.parquet"),
      ROW({"name"}, {ROW({"first", "last"}, {VARCHAR(), VARCHAR()})}),
      makeRowVector(
          {"t"},
          {
              vector,
          }));
  assertSelectWithFilter({"name"}, {}, "", "SELECT ('Janet', 'Jones')");

  loadData(
      getExampleFilePath("contacts.parquet"),
      ROW({"name"},
          {ROW(
              {"first", "middle", "last"}, {VARCHAR(), VARCHAR(), VARCHAR()})}),
      makeRowVector(
          {"t"},
          {
              vector,
          }));
  assertSelectWithFilter({"name"}, {}, "", "SELECT ('Janet', null, 'Jones')");

  loadData(
      getExampleFilePath("contacts.parquet"),
      ROW({"name"}, {ROW({"first", "middle"}, {VARCHAR(), VARCHAR()})}),
      makeRowVector(
          {"t"},
          {
              vector,
          }));
  assertSelectWithFilter({"name"}, {}, "", "SELECT ('Janet', null)");

  loadData(
      getExampleFilePath("contacts.parquet"),
      ROW({"name"}, {ROW({"middle", "last"}, {VARCHAR(), VARCHAR()})}),
      makeRowVector(
          {"t"},
          {
              vector,
          }));
  assertSelectWithFilter({"name"}, {}, "", "SELECT (null, 'Jones')");

  loadData(
      getExampleFilePath("contacts.parquet"),
      ROW({"name"}, {ROW({"middle"}, {VARCHAR()})}),
      makeRowVector(
          {"t"},
          {
              vector,
          }));
  assertSelectWithFilter({"name"}, {}, "", "SELECT row(null)");

  loadData(
      getExampleFilePath("contacts.parquet"),
      ROW({"name"}, {ROW({"middle", "info"}, {VARCHAR(), VARCHAR()})}),
      makeRowVector(
          {"t"},
          {
              vector,
          }));
  assertSelectWithFilter({"name"}, {}, "", "SELECT NULL");

  loadData(
      getExampleFilePath("contacts.parquet"),
      ROW({"name"}, {ROW({}, {})}),
      makeRowVector(
          {"t"},
          {
              vector,
          }));

  assertSelectWithFilter({"name"}, {}, "", "SELECT t from tmp");
}

TEST_F(ParquetTableScanTest, timestampFilter) {
  // Timestamp-int96.parquet holds one column (t: TIMESTAMP) and
  // 10 rows in one row group. Data is in SNAPPY compressed format.
  // The values are:
  // |t                  |
  // +-------------------+
  // |2015-06-01 19:34:56|
  // |2015-06-02 19:34:56|
  // |2001-02-03 03:34:06|
  // |1998-03-01 08:01:06|
  // |2022-12-23 03:56:01|
  // |1980-01-24 00:23:07|
  // |1999-12-08 13:39:26|
  // |2023-04-21 09:09:34|
  // |2000-09-12 22:36:29|
  // |2007-12-12 04:27:56|
  // +-------------------+
  auto vector = makeFlatVector<Timestamp>(
      {Timestamp(1433116800, 70496000000000),
       Timestamp(1433203200, 70496000000000),
       Timestamp(981158400, 12846000000000),
       Timestamp(888710400, 28866000000000),
       Timestamp(1671753600, 14161000000000),
       Timestamp(317520000, 1387000000000),
       Timestamp(944611200, 49166000000000),
       Timestamp(1682035200, 32974000000000),
       Timestamp(968716800, 81389000000000),
       Timestamp(1197417600, 16076000000000)});

  loadData(
      getExampleFilePath("timestamp_int96.parquet"),
      ROW({"t"}, {TIMESTAMP()}),
      makeRowVector(
          {"t"},
          {
              vector,
          }));

  assertSelectWithFilter({"t"}, {}, "", "SELECT t from tmp", false);
  assertSelectWithFilter(
      {"t"},
      {},
      "t < TIMESTAMP '2000-09-12 22:36:29'",
      "SELECT t from tmp where t < TIMESTAMP '2000-09-12 22:36:29'",
      false);
  assertSelectWithFilter(
      {"t"},
      {},
      "t <= TIMESTAMP '2000-09-12 22:36:29'",
      "SELECT t from tmp where t <= TIMESTAMP '2000-09-12 22:36:29'",
      false);
  assertSelectWithFilter(
      {"t"},
      {},
      "t > TIMESTAMP '1980-01-24 00:23:07'",
      "SELECT t from tmp where t > TIMESTAMP '1980-01-24 00:23:07'",
      false);
  assertSelectWithFilter(
      {"t"},
      {},
      "t >= TIMESTAMP '1980-01-24 00:23:07'",
      "SELECT t from tmp where t >= TIMESTAMP '1980-01-24 00:23:07'",
      false);
  assertSelectWithFilter(
      {"t"},
      {},
      "t == TIMESTAMP '2022-12-23 03:56:01'",
      "SELECT t from tmp where t == TIMESTAMP '2022-12-23 03:56:01'",
      false);
  VELOX_ASSERT_THROW(
      assertSelectWithFilter(
          {"t"},
          {"t < TIMESTAMP '2000-09-12 22:36:29'"},
          "",
          "SELECT t from tmp where t < TIMESTAMP '2000-09-12 22:36:29'"),
      "testInt128() is not supported");
}

TEST_F(ParquetTableScanTest, timestampINT96) {
  auto a = makeFlatVector<Timestamp>({Timestamp(1, 0), Timestamp(2, 0)});
  auto expected = makeRowVector({"time"}, {a});
  createDuckDbTable("expected", {expected});

  auto vector = makeArrayVector<Timestamp>({{}});
  loadData(
      getExampleFilePath("timestamp_dict_int96.parquet"),
      ROW({"time"}, {TIMESTAMP()}),
      makeRowVector(
          {"time"},
          {
              vector,
          }));
  assertSelect({"time"}, "SELECT time from expected");

  loadData(
      getExampleFilePath("timestamp_plain_int96.parquet"),
      ROW({"time"}, {TIMESTAMP()}),
      makeRowVector(
          {"time"},
          {
              vector,
          }));
  assertSelect({"time"}, "SELECT time from expected");
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv, false);
  return RUN_ALL_TESTS();
}
