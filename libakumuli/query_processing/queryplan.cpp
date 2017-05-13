#include "queryplan.h"
#include "storage_engine/nbtree.h"
#include "storage_engine/column_store.h"
#include "storage_engine/operators/operator.h"
#include "storage_engine/operators/scan.h"
#include "storage_engine/operators/merge.h"
#include "storage_engine/operators/aggregate.h"
#include "storage_engine/operators/join.h"

namespace Akumuli {
namespace QP {

using namespace StorageEngine;

/**
 * Tier-1 operator
 */
struct ProcessingPrelude {
    virtual ~ProcessingPrelude() = default;
    //! Compute processing step result (list of low level operators)
    virtual aku_Status apply(const ColumnStore& cstore) = 0;
    //! Get result of the processing step
    virtual aku_Status extract_result(std::vector<std::unique_ptr<RealValuedOperator>>* dest) = 0;
    //! Get result of the processing step
    virtual aku_Status extract_result(std::vector<std::unique_ptr<AggregateOperator>>* dest) = 0;
};

/**
 * Tier-N operator (materializer)
 */
struct MaterializationStep {

    virtual ~MaterializationStep() = default;

    //! Compute processing step result (list of low level operators)
    virtual aku_Status apply(ProcessingPrelude* prelude) = 0;

    /**
     * Get result of the processing step, this method should add cardinality() elements
     * to the `dest` array.
     */
    virtual aku_Status extract_result(std::unique_ptr<ColumnMaterializer>* dest) = 0;
};

// -------------------------------- //
//              Tier-1              //
// -------------------------------- //

struct ScanProcessingStep : ProcessingPrelude {
    std::vector<std::unique_ptr<RealValuedOperator>> scanlist_;
    aku_Timestamp begin_;
    aku_Timestamp end_;
    std::vector<aku_ParamId> ids_;

    template<class T>
    ScanProcessingStep(aku_Timestamp begin, aku_Timestamp end, T&& t)
        : begin_(begin)
        , end_(end)
        , ids_(std::forward<T>(t))
    {
    }

    virtual aku_Status apply(const ColumnStore& cstore) {
        return cstore.scan(ids_, begin_, end_, &scanlist_);
    }

    virtual aku_Status extract_result(std::vector<std::unique_ptr<RealValuedOperator>>* dest) {
        if (scanlist_.empty()) {
            return AKU_ENO_DATA;
        }
        *dest = std::move(scanlist_);
        return AKU_SUCCESS;
    }

    virtual aku_Status extract_result(std::vector<std::unique_ptr<AggregateOperator>>* dest) {
        return AKU_ENO_DATA;
    }
};

struct AggregateProcessingStep : ProcessingPrelude {
    std::vector<std::unique_ptr<AggregateOperator>> agglist_;
    aku_Timestamp begin_;
    aku_Timestamp end_;
    std::vector<aku_ParamId> ids_;

    template<class T>
    AggregateProcessingStep(aku_Timestamp begin, aku_Timestamp end, T&& t)
        : begin_(begin)
        , end_(end)
        , ids_(std::forward<T>(t))
    {
    }

    virtual aku_Status apply(const ColumnStore& cstore) {
        return cstore.aggregate(ids_, begin_, end_, &agglist_);
    }

    virtual aku_Status extract_result(std::vector<std::unique_ptr<RealValuedOperator>>* dest) {
        return AKU_ENO_DATA;
    }

    virtual aku_Status extract_result(std::vector<std::unique_ptr<AggregateOperator>>* dest) {
        if (agglist_.empty()) {
            return AKU_ENO_DATA;
        }
        *dest = std::move(agglist_);
        return AKU_SUCCESS;
    }
};


struct GroupAggregateProcessingStep : ProcessingPrelude {
    std::vector<std::unique_ptr<AggregateOperator>> agglist_;
    aku_Timestamp begin_;
    aku_Timestamp end_;
    aku_Timestamp step_;
    std::vector<aku_ParamId> ids_;

    template<class T>
    GroupAggregateProcessingStep(aku_Timestamp begin, aku_Timestamp end, aku_Timestamp step, T&& t)
        : begin_(begin)
        , end_(end)
        , step_(step)
        , ids_(std::forward<T>(t))
    {
    }

    virtual aku_Status apply(const ColumnStore& cstore) {
        return cstore.group_aggregate(ids_, begin_, end_, step_, &agglist_);
    }

    virtual aku_Status extract_result(std::vector<std::unique_ptr<RealValuedOperator>>* dest) {
        return AKU_ENO_DATA;
    }

    virtual aku_Status extract_result(std::vector<std::unique_ptr<AggregateOperator>>* dest) {
        if (agglist_.empty()) {
            return AKU_ENO_DATA;
        }
        *dest = std::move(agglist_);
        return AKU_SUCCESS;
    }
};


// -------------------------------- //
//              Tier-2              //
// -------------------------------- //

/**
 * Merge several series (order by series).
 * Used in scan query.
 */
template<OrderBy order>
struct MergeBy : MaterializationStep {
    std::vector<aku_ParamId> ids_;
    std::unique_ptr<ColumnMaterializer> mat_;

    template<class IdVec>
    MergeBy(IdVec&& ids)
        : ids_(std::forward<IdVec>(ids))
    {
    }

    aku_Status apply(ProcessingPrelude* prelude) {
        std::vector<std::unique_ptr<RealValuedOperator>> iters;
        auto status = prelude->extract_result(&iters);
        if (status != AKU_SUCCESS) {
            return status;
        }
        if (order == OrderBy::SERIES) {
            mat_.reset(new MergeMaterializer<SeriesOrder>(std::move(ids_), std::move(iters)));
        } else {
            mat_.reset(new MergeMaterializer<TimeOrder>(std::move(ids_), std::move(iters)));
        }
        return AKU_SUCCESS;
    }

    aku_Status extract_result(std::unique_ptr<ColumnMaterializer> *dest) {
        if (!mat_) {
            return AKU_ENO_DATA;
        }
        *dest = std::move(mat_);
        return AKU_SUCCESS;
    }
};

struct Chain : MaterializationStep {
    std::vector<aku_ParamId> ids_;
    std::unique_ptr<ColumnMaterializer> mat_;

    template<class IdVec>
    Chain(IdVec&& vec)
        : ids_(std::forward<IdVec>(vec))
    {
    }

    aku_Status apply(ProcessingPrelude *prelude) {
        std::vector<std::unique_ptr<RealValuedOperator>> iters;
        auto status = prelude->extract_result(&iters);
        if (status != AKU_SUCCESS) {
            return status;
        }
        mat_.reset(new ChainMaterializer(std::move(ids_), std::move(iters)));
        return AKU_SUCCESS;
    }

    aku_Status extract_result(std::unique_ptr<ColumnMaterializer> *dest) {
        if (!mat_) {
            return AKU_ENO_DATA;
        }
        *dest = std::move(mat_);
        return AKU_SUCCESS;
    }
};

/**
 * Aggregate materializer.
 * Accepts the list of ids and the list of aggregate operators.
 * Maps each id to the corresponding operators 1-1.
 * All ids should be different.
 */
struct Aggregate : MaterializationStep {
    std::vector<aku_ParamId> ids_;
    AggregationFunction fn_;
    std::unique_ptr<ColumnMaterializer> mat_;

    template<class IdVec>
    Aggregate(IdVec&& vec, AggregationFunction fn)
        : ids_(std::forward<IdVec>(vec))
        , fn_(fn)
    {
    }

    aku_Status apply(ProcessingPrelude *prelude) {
        std::vector<std::unique_ptr<AggregateOperator>> iters;
        auto status = prelude->extract_result(&iters);
        if (status != AKU_SUCCESS) {
            return status;
        }
        mat_.reset(new AggregateMaterializer(std::move(ids_), std::move(iters), fn_));
        return AKU_SUCCESS;

    }

    aku_Status extract_result(std::unique_ptr<ColumnMaterializer> *dest) {
        if (!mat_) {
            return AKU_ENO_DATA;
        }
        *dest = std::move(mat_);
        return AKU_SUCCESS;
    }
};

/**
 * Combines the aggregate operators.
 * Accepts list of ids (shouldn't be different) and list of aggregate
 * operators. Maps each id to operator and then combines operators
 * with the same id (used to implement aggregate + group-by).
 */
struct AggregateCombiner : MaterializationStep {
    std::vector<aku_ParamId> ids_;
    AggregationFunction fn_;
    std::unique_ptr<ColumnMaterializer> mat_;

    template<class IdVec>
    AggregateCombiner(IdVec&& vec, AggregationFunction fn)
        : ids_(std::forward<IdVec>(vec))
        , fn_(fn)
    {
    }

    aku_Status apply(ProcessingPrelude *prelude) {
        std::vector<std::unique_ptr<AggregateOperator>> iters;
        auto status = prelude->extract_result(&iters);
        if (status != AKU_SUCCESS) {
            return status;
        }
        std::vector<std::unique_ptr<AggregateOperator>> agglist;
        std::map<aku_ParamId, std::vector<std::unique_ptr<AggregateOperator>>> groupings;
        for (size_t i = 0; i < ids_.size(); i++) {
            auto id = ids_.at(i);
            auto it = std::move(iters.at(i));
            groupings[id].push_back(std::move(it));
        }
        std::vector<aku_ParamId> ids;
        for (auto& kv: groupings) {
            auto& vec = kv.second;
            ids.push_back(kv.first);
            std::unique_ptr<CombineAggregateOperator> it(new CombineAggregateOperator(std::move(vec)));
            agglist.push_back(std::move(it));
        }
        mat_.reset(new AggregateMaterializer(std::move(ids),
                                             std::move(agglist),
                                             fn_));
        return AKU_SUCCESS;

    }

    aku_Status extract_result(std::unique_ptr<ColumnMaterializer> *dest) {
        if (!mat_) {
            return AKU_ENO_DATA;
        }
        *dest = std::move(mat_);
        return AKU_SUCCESS;
    }
};

/**
 * Joins several operators into one.
 * Number of joined operators is defined by the cardinality.
 * Number of ids should be `cardinality` times smaller than number
 * of operators because every `cardinality` operators are joined into
 * one.
 */
struct Join : MaterializationStep {
    std::vector<aku_ParamId> ids_;
    int cardinality_;
    OrderBy order_;
    aku_Timestamp begin_;
    aku_Timestamp end_;
    std::unique_ptr<ColumnMaterializer> mat_;

    template<class IdVec>
    Join(IdVec&& vec, int cardinality, OrderBy order, aku_Timestamp begin, aku_Timestamp end)
        : ids_(std::forward<IdVec>(vec))
        , cardinality_(cardinality)
        , order_(order)
        , begin_(begin)
        , end_(end)
    {
    }

    aku_Status apply(ProcessingPrelude *prelude) {
        int inc = cardinality_;
        std::vector<std::unique_ptr<RealValuedOperator>> scanlist;
        auto status = prelude->extract_result(&scanlist);
        if (status != AKU_SUCCESS) {
            return status;
        }
        std::vector<std::unique_ptr<ColumnMaterializer>> iters;
        for (size_t i = 0; i < ids_.size(); i++) {
            // ids_ contain ids of the joined series that corresponds
            // to the names in the series matcher
            std::vector<std::unique_ptr<RealValuedOperator>> joined;
            for (int j = 0; j < inc; j++) {
                // `inc` number of storage level operators correspond to one
                // materializer
                joined.push_back(std::move(scanlist.at(i*inc + j)));
            }
            std::unique_ptr<ColumnMaterializer> it;
            it.reset(new JoinMaterializer(std::move(joined), ids_.at(i)));
            iters.push_back(std::move(it));
        }
        if (order_ == OrderBy::SERIES) {
            mat_.reset(new JoinConcatMaterializer(std::move(iters)));
        } else {
            bool forward = begin_ < end_;
            typedef MergeJoinMaterializer<MergeJoinUtil::OrderByTimestamp> Materializer;
            mat_.reset(new Materializer(std::move(iters), forward));
        }
        return AKU_SUCCESS;
    }

    aku_Status extract_result(std::unique_ptr<ColumnMaterializer> *dest) {
        if (!mat_) {
            return AKU_ENO_DATA;
        }
        *dest = std::move(mat_);
        return AKU_SUCCESS;
    }
};

/**
 * Merges several group-aggregate operators by chaining
 */
struct SeriesOrderAggregate : MaterializationStep {
    std::vector<aku_ParamId> ids_;
    std::vector<AggregationFunction> fn_;
    std::unique_ptr<ColumnMaterializer> mat_;

    template<class IdVec, class FnVec>
    SeriesOrderAggregate(IdVec&& vec, FnVec&& fn)
        : ids_(std::forward<IdVec>(vec))
        , fn_(std::forward<FnVec>(fn))
    {
    }

    aku_Status apply(ProcessingPrelude *prelude) {
        std::vector<std::unique_ptr<AggregateOperator>> iters;
        auto status = prelude->extract_result(&iters);
        if (status != AKU_SUCCESS) {
            return status;
        }
        mat_.reset(new SeriesOrderAggregateMaterializer(std::move(ids_), std::move(iters), fn_));
        return AKU_SUCCESS;
    }

    aku_Status extract_result(std::unique_ptr<ColumnMaterializer> *dest) {
        if (!mat_) {
            return AKU_ENO_DATA;
        }
        *dest = std::move(mat_);
        return AKU_SUCCESS;
    }
};

struct TimeOrderAggregate : MaterializationStep {
    std::vector<aku_ParamId> ids_;
    std::vector<AggregationFunction> fn_;
    std::unique_ptr<ColumnMaterializer> mat_;

    template<class IdVec, class FnVec>
    TimeOrderAggregate(IdVec&& vec, FnVec&& fn)
        : ids_(std::forward<IdVec>(vec))
        , fn_(std::forward<FnVec>(fn))
    {
    }

    aku_Status apply(ProcessingPrelude *prelude) {
        std::vector<std::unique_ptr<AggregateOperator>> iters;
        auto status = prelude->extract_result(&iters);
        if (status != AKU_SUCCESS) {
            return status;
        }
        mat_.reset(new TimeOrderAggregateMaterializer(ids_, iters, fn_));
        return AKU_SUCCESS;
    }

    aku_Status extract_result(std::unique_ptr<ColumnMaterializer> *dest) {
        if (!mat_) {
            return AKU_ENO_DATA;
        }
        *dest = std::move(mat_);
        return AKU_SUCCESS;
    }
};

struct TwoStepQueryPlan : IQueryPlan {
    std::unique_ptr<ProcessingPrelude> prelude_;
    std::unique_ptr<MaterializationStep> mater_;
    std::unique_ptr<ColumnMaterializer> column_;

    template<class T1, class T2>
    TwoStepQueryPlan(T1&& t1, T2&& t2)
        : prelude_(std::forward<T1>(t1))
        , mater_(std::forward<T2>(t2))
    {
    }

    aku_Status execute(const ColumnStore &cstore) {
        auto status = prelude_->apply(cstore);
        if (status != AKU_SUCCESS) {
            return status;
        }
        status = mater_->apply(prelude_.get());
        if (status != AKU_SUCCESS) {
            return status;
        }
        return mater_->extract_result(&column_);
    }

    std::tuple<aku_Status, size_t> read(u8 *dest, size_t size) {
        if (!column_) {
            AKU_PANIC("Successful execute step required");
        }
        return column_->read(dest, size);
    }
};

// ----------- Query plan builder ------------ //

static std::unique_ptr<IQueryPlan> scan_query_plan(ReshapeRequest const& req) {
    // Hardwired query plan for scan query
    // Tier1
    // - List of range scan operators
    // Tier2
    // - If group-by is enabled:
    //   - Transform ids and matcher (generate new names)
    //   - Add merge materialization step (series or time order, depending on the
    //     order-by clause.
    // - Otherwise
    //   - If oreder-by is series add chain materialization step.
    //   - Otherwise add merge materializer.

    std::unique_ptr<IQueryPlan> result;

    if (req.agg.enabled || req.select.columns.size() != 1) {
        AKU_PANIC("Invalid request");
    }

    std::unique_ptr<ProcessingPrelude> t1stage;
    t1stage.reset(new ScanProcessingStep(req.select.begin, req.select.end, req.select.columns.at(0).ids));

    std::unique_ptr<MaterializationStep> t2stage;
    if (req.group_by.enabled) {
        std::vector<aku_ParamId> ids;
        for(auto id: req.select.columns.at(0).ids) {
            auto it = req.group_by.transient_map.find(id);
            if (it != req.group_by.transient_map.end()) {
                ids.push_back(it->second);
            }
        }
        if (req.order_by == OrderBy::SERIES) {
            t2stage.reset(new MergeBy<OrderBy::SERIES>(std::move(ids)));
        } else {
            t2stage.reset(new MergeBy<OrderBy::TIME>(std::move(ids)));
        }
    } else {
        auto ids = req.select.columns.at(0).ids;
        if (req.order_by == OrderBy::SERIES) {
            t2stage.reset(new Chain(std::move(ids)));
        } else {
            t2stage.reset(new MergeBy<OrderBy::TIME>(std::move(ids)));
        }
    }

    result.reset(new TwoStepQueryPlan(std::move(t1stage), std::move(t2stage)));
    return result;
}

static std::unique_ptr<IQueryPlan> aggregate_query_plan(ReshapeRequest const& req) {
    // Hardwired query plan for aggregate query
    // Tier1
    // - List of aggregate operators
    // Tier2
    // - If group-by is enabled:
    //   - Transform ids and matcher (generate new names)
    //   - Add merge materialization step (series or time order, depending on the
    //     order-by clause.
    // - Otherwise
    //   - If oreder-by is series add chain materialization step.
    //   - Otherwise add merge materializer.

    std::unique_ptr<IQueryPlan> result;

    if (req.order_by == OrderBy::TIME || req.agg.enabled == false ||
        req.agg.func.size() != 1 || req.agg.step != 0)
    {
        AKU_PANIC("Invalid request");
    }

    std::unique_ptr<ProcessingPrelude> t1stage;
    t1stage.reset(new AggregateProcessingStep(req.select.begin, req.select.end, req.select.columns.at(0).ids));

    std::unique_ptr<MaterializationStep> t2stage;
    if (req.group_by.enabled) {
        std::vector<aku_ParamId> ids;
        for(auto id: req.select.columns.at(0).ids) {
            auto it = req.group_by.transient_map.find(id);
            if (it != req.group_by.transient_map.end()) {
                ids.push_back(it->second);
            }
        }
        t2stage.reset(new AggregateCombiner(std::move(ids), req.agg.func.front()));
    } else {
        auto ids = req.select.columns.at(0).ids;
        t2stage.reset(new Aggregate(std::move(ids), req.agg.func.front()));
    }

    result.reset(new TwoStepQueryPlan(std::move(t1stage), std::move(t2stage)));
    return result;
}

std::unique_ptr<IQueryPlan> QueryPlanBuilder::create(const ReshapeRequest& req) {
    if (req.agg.enabled && req.agg.step == 0) {
        return aggregate_query_plan(req);
    }
    return scan_query_plan(req);
}


typedef std::vector<std::unique_ptr<QueryPlanStage>> StagesT;

static StagesT create_scan(ReshapeRequest const& req) {
    // Hardwired query plan for scan query
    // Tier1
    // - List of range scan operators
    // Tier2
    // - If group-by is enabled:
    //   - Transform ids and matcher (generate new names)
    //   - Add merge materialization step (series or time order, depending on the
    //     order-by clause.
    // - Otherwise
    //   - If oreder-by is series add chain materialization step.
    //   - Otherwise add merge materializer.

    StagesT result;

    if (req.agg.enabled || req.select.columns.size() != 1) {
        AKU_PANIC("Invalid request");
    }

    std::unique_ptr<QueryPlanStage> t1stage;
    t1stage.reset(new QueryPlanStage());

    aku_Timestamp begin   = req.select.begin;
    aku_Timestamp end     = req.select.end;
    const auto &tier1ids  = req.select.columns.at(0).ids;
    t1stage->op_.tier1    = Tier1Operator::SCAN_RANGE;
    t1stage->tier_        = 1;
    t1stage->opt_ids_     = tier1ids;
    t1stage->opt_matcher_ = req.select.matcher;
    t1stage->time_range_  = std::make_pair(begin, end);

    result.push_back(std::move(t1stage));

    if (req.group_by.enabled) {
        std::vector<aku_ParamId> ids;
        for(auto id: req.select.columns.at(0).ids) {
            auto it = req.group_by.transient_map.find(id);
            if (it != req.group_by.transient_map.end()) {
                ids.push_back(it->second);
            }
        }
        std::unique_ptr<QueryPlanStage> t2stage;
        t2stage.reset(new QueryPlanStage());
        Tier2Operator op      = req.order_by == OrderBy::SERIES
                              ? Tier2Operator::MERGE_SERIES_ORDER
                              : Tier2Operator::MERGE_TIME_ORDER;
        t2stage->op_.tier2    = op;
        t2stage->tier_        = 2;
        t2stage->opt_matcher_ = req.group_by.matcher;
        t2stage->opt_ids_     = ids;
        t2stage->time_range_  = std::make_pair(begin, end);

        result.push_back(std::move(t2stage));
    } else {

        std::unique_ptr<QueryPlanStage> t2stage;
        t2stage.reset(new QueryPlanStage());

        Tier2Operator op      = req.order_by == OrderBy::SERIES
                              ? Tier2Operator::CHAIN_SERIES
                              : Tier2Operator::MERGE_TIME_ORDER;
        t2stage->op_.tier2    = op;
        t2stage->tier_        = 2;
        t2stage->opt_ids_     = req.select.columns.at(0).ids;
        t2stage->time_range_  = std::make_pair(begin, end);  // not needed here but anyway
        t2stage->opt_matcher_ = req.select.matcher;

        result.push_back(std::move(t2stage));
    }
    return result;
}

static StagesT create_aggregate(ReshapeRequest const& req) {
    // Hardwired query plan for aggregate query
    // Tier1
    // - List of aggregate operators
    // Tier2
    // - If group-by is enabled:
    //   - Transform ids and matcher (generate new names)
    //   - Add merge materialization step (series or time order, depending on the
    //     order-by clause.
    // - Otherwise
    //   - If oreder-by is series add chain materialization step.
    //   - Otherwise add merge materializer.

    StagesT result;

    if (req.order_by == OrderBy::TIME) {
        AKU_PANIC("Invalid request");
    }

    std::unique_ptr<QueryPlanStage> t1stage;
    t1stage.reset(new QueryPlanStage());

    aku_Timestamp begin   = req.select.begin;
    aku_Timestamp end     = req.select.end;
    const auto &tier1ids  = req.select.columns.at(0).ids;
    t1stage->op_.tier1    = Tier1Operator::AGGREGATE_RANGE;
    t1stage->tier_        = 1;
    t1stage->opt_ids_     = tier1ids;
    t1stage->opt_matcher_ = req.select.matcher;
    t1stage->time_range_  = std::make_pair(begin, end);

    result.push_back(std::move(t1stage));

    if (req.group_by.enabled) {
        // Stage2 - combine aggregate
        std::vector<aku_ParamId> ids;
        for(auto id: req.select.columns.at(0).ids) {
            auto it = req.group_by.transient_map.find(id);
            if (it != req.group_by.transient_map.end()) {
                ids.push_back(it->second);
            }
        }
        std::unique_ptr<QueryPlanStage> t2stage;
        t2stage.reset(new QueryPlanStage());
        Tier2Operator op2     = Tier2Operator::AGGREGATE_COMBINE;
        t2stage->op_.tier2    = op2;
        t2stage->tier_        = 2;
        t2stage->opt_matcher_ = req.group_by.matcher;
        t2stage->opt_ids_     = ids;
        t2stage->time_range_  = std::make_pair(begin, end);
        t2stage->opt_func_    = req.agg.func;

        result.push_back(std::move(t2stage));
    } else {
        // Stage2 - materialize aggregate
        std::unique_ptr<QueryPlanStage> t2stage;
        t2stage.reset(new QueryPlanStage());

        Tier2Operator op      = Tier2Operator::AGGREGATE;
        t2stage->op_.tier2    = op;
        t2stage->tier_        = 2;
        t2stage->opt_ids_     = req.select.columns.at(0).ids;
        t2stage->time_range_  = std::make_pair(begin, end);  // not needed here but anyway
        t2stage->opt_matcher_ = req.select.matcher;
        t2stage->opt_func_    = req.agg.func;

        result.push_back(std::move(t2stage));
    }

    return result;
}

static StagesT create_join(ReshapeRequest const& req) {
    StagesT result;

    // Group-by and aggregation is not supported currently
    if (req.agg.enabled || req.group_by.enabled || req.select.columns.size() < 2) {
        AKU_PANIC("Invalid request");
    }

    std::unique_ptr<QueryPlanStage> t1stage;
    t1stage.reset(new QueryPlanStage());
    std::vector<aku_ParamId> t1ids;

    int cardinality       = static_cast<int>(req.select.columns.size());
    for (size_t i = 0; i < req.select.columns.at(0).ids.size(); i++) {
        for (int c = 0; c < cardinality; c++) {
            t1ids.push_back(req.select.columns.at(static_cast<size_t>(c)).ids.at(i));
        }
    }
    aku_Timestamp begin   = req.select.begin;
    aku_Timestamp end     = req.select.end;
    t1stage->op_.tier1    = Tier1Operator::SCAN_RANGE;
    t1stage->tier_        = 1;
    t1stage->opt_ids_     = t1ids;
    t1stage->opt_matcher_ = req.select.matcher;
    t1stage->time_range_  = std::make_pair(begin, end);

    result.push_back(std::move(t1stage));
    if (req.group_by.enabled) {
        // Not supported
        AKU_PANIC("Group-by not supported");
    } else {
        // Stage2 - materialize aggregate
        std::unique_ptr<QueryPlanStage> t2stage;
        t2stage.reset(new QueryPlanStage());

        Tier2Operator op      = req.order_by == OrderBy::SERIES ?
                                Tier2Operator::MERGE_JOIN_SERIES_ORDER :
                                Tier2Operator::MERGE_JOIN_TIME_ORDER;
        t2stage->op_.tier2    = op;
        t2stage->tier_        = 2;
        t2stage->opt_join_cardinality_
                              = cardinality;
        t2stage->opt_ids_     = req.select.columns.at(0).ids;  // Join will use ids from the first row
        t2stage->time_range_  = std::make_pair(begin, end);  // not needed here but anyway
        t2stage->opt_matcher_ = req.select.matcher;

        result.push_back(std::move(t2stage));
    }
    return result;
}

static StagesT create_group_aggregate(ReshapeRequest const& req) {
    // Hardwired query plan for group aggregate query
    // Tier1
    // - List of group aggregate operators
    // Tier2
    // - If group-by is enabled:
    //   - Transform ids and matcher (generate new names)
    //   - Add merge materialization step (series or time order, depending on the
    //     order-by clause.
    // - Otherwise
    //   - If oreder-by is series add chain materialization step.
    //   - Otherwise add merge materializer.
    StagesT result;

    // Group-by and aggregation is not supported currently
    if (!req.agg.enabled || req.agg.step == 0) {
        AKU_PANIC("Invalid request");
    }

    std::unique_ptr<QueryPlanStage> t1stage;
    t1stage.reset(new QueryPlanStage());

    aku_Timestamp begin   = req.select.begin;
    aku_Timestamp end     = req.select.end;
    const auto &tier1ids  = req.select.columns.at(0).ids;
    t1stage->op_.tier1    = Tier1Operator::GROUP_AGGREGATE_RANGE;
    t1stage->tier_        = 1;
    t1stage->opt_ids_     = tier1ids;
    t1stage->opt_matcher_ = req.select.matcher;
    t1stage->time_range_  = std::make_pair(begin, end);
    t1stage->opt_step_    = req.agg.step;

    result.push_back(std::move(t1stage));

    if (req.group_by.enabled) {
        AKU_PANIC("Not implemented");
    } else {
        // Stage2 - materialize aggregate
        std::unique_ptr<QueryPlanStage> t2stage;
        t2stage.reset(new QueryPlanStage());

        Tier2Operator op      = req.order_by == OrderBy::SERIES
                              ? Tier2Operator::SERIES_ORDER_AGGREGATE_MATERIALIZER
                              : Tier2Operator::TIME_ORDER_AGGREGATE_MATERIALIZER;
        t2stage->op_.tier2    = op;
        t2stage->tier_        = 2;
        t2stage->opt_ids_     = req.select.columns.at(0).ids;
        t2stage->time_range_  = std::make_pair(begin, end);
        t2stage->opt_matcher_ = req.select.matcher;
        t2stage->opt_func_    = req.agg.func;

        result.push_back(std::move(t2stage));
    }

    return result;
}

static StagesT create_plan(ReshapeRequest const& req) {
    if (req.agg.enabled && req.agg.step == 0) {
        // Aggregate query
        return create_aggregate(req);
    } else if (req.agg.enabled && req.agg.step != 0) {
        // Group aggregate query
        return create_group_aggregate(req);
    } else if (req.agg.enabled == false && req.select.columns.size() > 1) {
        // Join query
        return create_join(req);
    }
    // Scan query
    return create_scan(req);
}


QueryPlan::QueryPlan(ReshapeRequest const& req)
    : stages_(create_plan(req))
{
}

}} // namespaces
