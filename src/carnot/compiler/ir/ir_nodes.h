#pragma once
#include <algorithm>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pypa/ast/ast.hh>

#include "src/carnot/compiler/compiler_error_context/compiler_error_context.h"
#include "src/carnot/compiler/compilerpb/compiler_status.pb.h"
#include "src/carnot/metadatapb/metadata.pb.h"
#include "src/carnot/plan/dag.h"
#include "src/carnot/plan/operators.h"
#include "src/carnot/udfspb/udfs.pb.h"
#include "src/common/base/base.h"
#include "src/table_store/table_store.h"

namespace pl {
namespace carnot {
namespace compiler {

class IR;
class IRNode;
using IRNodePtr = std::unique_ptr<IRNode>;

/**
 * @brief NameToNode is a struct to store string, node pairs. This enables Arg data structures to
 * preserve input order of these arguments which is probably expected by the user and gives
 * deterministic guarantees that hashmaps can't.
 *
 */
struct NameToNode {
  NameToNode(std::string_view n, IRNode* nd) : name(n), node(nd) {}
  std::string name;
  IRNode* node;
};

struct ArgMap {
  // Kwargs is a vector because we want to preserve the input order for display of the tables.
  std::vector<NameToNode> kwargs;
  std::vector<IRNode*> args;
};

enum class IRNodeType {
  kAny = -1,
  kMemorySource,
  kMemorySink,
  kMap,
  kDrop,
  kBlockingAgg,
  kFilter,
  kLimit,
  kString,
  kUInt128,
  kFloat,
  kInt,
  kBool,
  kFunc,
  kList,
  kTuple,
  kColumn,
  kTime,
  kMetadata,
  kMetadataResolver,
  kMetadataLiteral,
  kGRPCSourceGroup,
  kGRPCSource,
  kGRPCSink,
  kUnion,
  kJoin,
  kTabletSourceGroup,
  kGroupBy,
  kUDTFSource,
  number_of_types  // This is not a real type, but is used to verify strings are inline
                   // with enums.
};
static constexpr const char* kIRNodeStrings[] = {"MemorySource",
                                                 "MemorySink",
                                                 "Map",
                                                 "Drop",
                                                 "BlockingAgg",
                                                 "Filter",
                                                 "Limit",
                                                 "String",
                                                 "UInt128Value",
                                                 "Float",
                                                 "Int",
                                                 "Bool",
                                                 "Func",
                                                 "List",
                                                 "Tuple",
                                                 "Column",
                                                 "Time",
                                                 "Metadata",
                                                 "MetadataResolver",
                                                 "MetadataLiteral",
                                                 "GRPCSourceGroup",
                                                 "GRPCSource",
                                                 "GRPCSink",
                                                 "Union",
                                                 "Join",
                                                 "TabletSourceGroup",
                                                 "GroupBy",
                                                 "UDTFSource"};
inline std::ostream& operator<<(std::ostream& out, IRNodeType node_type) {
  return out << kIRNodeStrings[static_cast<int64_t>(node_type)];
}

inline static constexpr char kPLFuncPrefix[] = "pl";

/**
 * @brief Node class for the IR.
 *
 */
class IRNode {
 public:
  IRNode() = delete;
  virtual ~IRNode() = default;
  int64_t line() const { return line_; }
  int64_t col() const { return col_; }
  bool line_col_set() const { return line_col_set_; }

  virtual std::string DebugString() const;
  virtual bool IsOperator() const = 0;
  virtual bool IsExpression() const = 0;
  IRNodeType type() const { return type_; }
  std::string type_string() const { return TypeString(type()); }
  static std::string TypeString(const IRNodeType& node_type) {
    return kIRNodeStrings[static_cast<int64_t>(node_type)];
  }

  /**
   * @brief Set the pointer to the graph.
   * The pointer is passed in by the Node factory of the graph
   * (see IR::MakeNode) so that we can add edges between this
   * object and any other objects created later on.
   *
   * @param graph_ptr : pointer to the graph object.
   */
  void SetGraphPtr(IR* graph_ptr) { graph_ptr_ = graph_ptr; }
  // Returns the ID of the operator.
  int64_t id() const { return id_; }
  IR* graph_ptr() const { return graph_ptr_; }
  pypa::AstPtr ast_node() const { return ast_node_; }
  /**
   * @brief Create an error that incorporates line, column of ir node into the error message.
   *
   * @param args: the arguments to the substitute that is called.
   * @return Status: Status with CompilerError context.
   */
  template <typename... Args>
  Status CreateIRNodeError(Args... args) const {
    compilerpb::CompilerErrorGroup context =
        LineColErrorPb(line(), col(), absl::Substitute(args...));
    return Status(statuspb::INVALID_ARGUMENT, "",
                  std::make_unique<compilerpb::CompilerErrorGroup>(context));
  }
  /**
   * @brief Errors out if in debug mode, otherwise floats up an error.
   *
   * @return Status: status if not in debug mode.
   */
  template <typename... Args>
  Status DExitOrIRNodeError(Args... args) const {
    DCHECK(false) << absl::Substitute(args...);
    return CreateIRNodeError(args...);
  }

  /*
   * @brief Copy data from the input node into this node. All children classes need to implement
   * CopyFromNodeImpl. If a child class is itself a parent of other classes, then it must override
   * this class and call this method, followed by whatever operations that all of its child
   * classes must do during a CopyFromNode.
   *
   * @param node
   * @return Status
   */
  virtual Status CopyFromNode(const IRNode* node,
                              absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map);
  void SetLineCol(int64_t line, int64_t col);
  void SetLineCol(const pypa::AstPtr& ast_node);

 protected:
  explicit IRNode(int64_t id, IRNodeType type) : type_(type), id_(id) {}
  virtual Status CopyFromNodeImpl(
      const IRNode* node, absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) = 0;

  IRNodeType type_;

 private:
  int64_t id_;
  // line and column where the parser read the data for this node.
  // used for highlighting errors in queries.
  int64_t line_;
  int64_t col_;
  IR* graph_ptr_;
  bool line_col_set_ = false;
  pypa::AstPtr ast_node_;
};

class OperatorIR;
/**
 * IR contains the intermediate representation of the query
 * before compiling into the logical plan.
 */
class IR {
 public:
  /**
   * @brief Node factory that adds a node to the list,
   * updates an id, then returns a pointer to manipulate.
   *
   * The object will be owned by the IR object that created it.
   *
   * @tparam TOperator the type of the operator.
   * @return StatusOr<TOperator *> - the node will be owned
   * by this IR object.
   */
  template <typename TOperator>
  StatusOr<TOperator*> MakeNode(const pypa::AstPtr& ast) {
    return MakeNode<TOperator>(id_node_counter, ast);
  }
  template <typename TOperator>
  StatusOr<TOperator*> MakeNode(int64_t id) {
    return MakeNode<TOperator>(id, nullptr);
  }
  template <typename TOperator>
  StatusOr<TOperator*> MakeNode(int64_t id, const pypa::AstPtr& ast) {
    id_node_counter = std::max(id + 1, id_node_counter);
    auto node = std::make_unique<TOperator>(id);
    dag_.AddNode(node->id());
    node->SetGraphPtr(this);
    if (ast != nullptr) {
      node->SetLineCol(ast);
    }
    TOperator* raw = node.get();
    id_node_map_.emplace(node->id(), std::move(node));
    return raw;
  }
  StatusOr<IRNode*> MakeNodeWithType(IRNodeType node_type, int64_t new_node_id);

  template <typename TOperator, typename... Args>
  StatusOr<TOperator*> CreateNode(const pypa::AstPtr& ast, Args... args) {
    PL_ASSIGN_OR_RETURN(TOperator * op, MakeNode<TOperator>(ast));
    PL_RETURN_IF_ERROR(op->Init(args...));
    return op;
  }

  template <typename TIRNodeType>
  StatusOr<TIRNodeType*> CopyNode(const TIRNodeType* source) {
    absl::flat_hash_map<const IRNode*, IRNode*> mapping;
    return CopyNode(source, &mapping);
  }

  /**
   * @brief Copies a node into this IR. Uses the source's node ID if the source is from a different
   * IR. Note that it does not set the parent of the copy, but children of this node will have their
   * parents set.
   *
   * @param source the input node
   * @param copied_nodes_map The mapping of nodes that have already been copied by other calls to
   * CopyNode in the current top-level invocation.
   * @return StatusOr<IRNode*> the copied node
   */
  template <typename TIRNodeType>
  StatusOr<TIRNodeType*> CopyNode(const TIRNodeType* source,
                                  absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) {
    CHECK(copied_nodes_map != nullptr);
    // If this node has already been copied over, don't copy it again. This will happen when a node
    // has multiple nodes using it.
    if (copied_nodes_map->contains(source)) {
      return static_cast<TIRNodeType*>(copied_nodes_map->at(source));
    }
    // Use the source's ID if we are copying in to a different graph.
    auto new_node_id = this == source->graph_ptr() ? id_node_counter : source->id();
    PL_ASSIGN_OR_RETURN(IRNode * new_node, MakeNodeWithType(source->type(), new_node_id));
    PL_RETURN_IF_ERROR(new_node->CopyFromNode(source, copied_nodes_map));
    copied_nodes_map->emplace(source, new_node);
    CHECK_EQ(new_node->type(), source->type());
    return static_cast<TIRNodeType*>(new_node);
  }

  Status AddEdge(int64_t from_node, int64_t to_node);
  bool HasEdge(int64_t from_node, int64_t to_node) const;
  bool HasNode(int64_t node_id) const { return dag().HasNode(node_id); }

  Status AddEdge(const IRNode* from_node, const IRNode* to_node);
  bool HasEdge(const IRNode* from_node, const IRNode* to_node) const;
  Status DeleteEdge(int64_t from_node, int64_t to_node);
  Status DeleteEdge(IRNode* from_node, IRNode* to_node);
  Status DeleteNode(int64_t node);
  Status DeleteNodeAndChildren(int64_t node);

  /**
   * @brief Adds an edge between the parent and child nodes in the DAG.
   * If there is already a link between them, then the child is cloned and an edge is added between
   * the parent and the clone instead.
   *
   * @param parent the parent node
   * @param parent the child node
   * @return StatusOr<IRNode*> the child that now has the edge (either a clone or the input child)
   */
  template <typename TChildType>
  StatusOr<TChildType*> OptionallyCloneWithEdge(IRNode* parent, TChildType* child) {
    TChildType* returned_child = child;
    if (HasEdge(parent, child)) {
      PL_ASSIGN_OR_RETURN(returned_child, CopyNode(child));
    }
    PL_RETURN_IF_ERROR(AddEdge(parent, returned_child));
    return returned_child;
  }

  plan::DAG& dag() { return dag_; }
  const plan::DAG& dag() const { return dag_; }
  std::string DebugString();
  IRNode* Get(int64_t id) const {
    DCHECK(dag_.HasNode(id)) << "DAG doesn't have node: " << id;
    auto iterator = id_node_map_.find(id);
    DCHECK(iterator != id_node_map_.end()) << "id to node map doesn't contain id: " << id;
    return iterator->second.get();
  }
  size_t size() const { return id_node_map_.size(); }
  std::vector<IRNode*> GetSinks() {
    std::vector<IRNode*> nodes;
    for (auto& i : dag().TopologicalSort()) {
      IRNode* node = Get(i);
      if (node->type() == IRNodeType::kMemorySink) {
        nodes.push_back(node);
        DCHECK(node->IsOperator());
      }
    }
    return nodes;
  }

  std::vector<OperatorIR*> GetSources() const;

  StatusOr<std::unique_ptr<IR>> Clone() const;

  StatusOr<planpb::Plan> ToProto() const;

  /**
   * @brief Removes the nodes and edges listed in the following set.
   *
   * @param ids_to_prune: the ids which to prune from the graph.
   * @return Status: error if something not found or missing.
   */
  Status Prune(const absl::flat_hash_set<int64_t>& ids_to_prune);

  std::vector<IRNode*> FindNodesOfType(IRNodeType type) const;

  template <typename Matcher>
  std::vector<IRNode*> FindNodesThatMatch(Matcher matcher) const {
    std::vector<IRNode*> nodes;
    for (int64_t i : dag().TopologicalSort()) {
      if (Match(Get(i), matcher)) {
        nodes.push_back(Get(i));
      }
    }
    return nodes;
  }

  friend std::ostream& operator<<(std::ostream& os, const std::shared_ptr<IR>&) {
    return os << "ir";
  }

 private:
  Status OutputProto(planpb::PlanFragment* pf, const OperatorIR* op_node) const;
  plan::DAG dag_;
  std::unordered_map<int64_t, IRNodePtr> id_node_map_;
  int64_t id_node_counter = 0;
};

// Forward declaration for types that are used in OperatorIR. They are declared later in this
// header.
class ColumnIR;
class ExpressionIR;

/**
 * @brief Node class for the operator
 *
 */
class OperatorIR : public IRNode {
 public:
  OperatorIR() = delete;
  bool IsOperator() const override { return true; }
  bool IsExpression() const override { return false; }
  table_store::schema::Relation relation() const { return relation_; }
  Status SetRelation(table_store::schema::Relation relation) {
    relation_init_ = true;
    relation_ = relation;
    return Status::OK();
  }
  bool IsRelationInit() const { return relation_init_; }
  bool HasParents() const { return parents_.size() != 0; }
  bool IsChildOf(OperatorIR* parent) {
    return std::find(parents_.begin(), parents_.end(), parent) != parents_.end();
  }
  const std::vector<OperatorIR*>& parents() const { return parents_; }
  Status AddParent(OperatorIR* node);
  Status RemoveParent(OperatorIR* op);

  /**
   * @brief Replaces the old_parent with the new parent. Errors out if the old_parent isn't actually
   * a parent.
   *
   * @param old_parent: operator's parent that will be replaced.
   * @param new_parent: the new operator to replace old_parent with.
   * @return Status: Error if old_parent not an actualy parent.
   */
  Status ReplaceParent(OperatorIR* old_parent, OperatorIR* new_parent);

  virtual Status ToProto(planpb::Operator*) const = 0;

  std::string ParentsDebugString();
  Status CopyParentsFrom(const OperatorIR* og_op);

  /**
   * @brief Override of CopyFromNode that adds special handling for Operators.
   */
  Status CopyFromNode(const IRNode* node,
                      absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

  virtual bool IsBlocking() const { return false; }

  /**
   * @brief Returns the Operator children of this node.
   *
   * @return std::vector<OperatorIR*>: the vector of operator children of this node.
   */
  std::vector<OperatorIR*> Children() const;

  bool is_source() const { return is_source_; }

 protected:
  explicit OperatorIR(int64_t id, IRNodeType type, bool has_parents, bool is_source)
      : IRNode(id, type), is_source_(is_source), can_have_parents_(has_parents) {}

  /**
   * @brief Support for operators that have the same parent multiple times, like a self-join.
   */
  StatusOr<std::vector<OperatorIR*>> HandleDuplicateParents(
      const std::vector<OperatorIR*>& parents);

 private:
  bool is_source_ = false;
  bool relation_init_ = false;
  bool can_have_parents_;
  std::vector<OperatorIR*> parents_;
  table_store::schema::Relation relation_;
};

class ExpressionIR : public IRNode {
 public:
  ExpressionIR() = delete;

  bool IsOperator() const override { return false; }
  bool IsExpression() const override { return true; }
  virtual types::DataType EvaluatedDataType() const = 0;
  virtual bool IsDataTypeEvaluated() const = 0;
  virtual bool IsColumn() const { return false; }
  virtual bool IsData() const { return false; }
  virtual bool IsCollection() const { return false; }
  virtual bool IsFunction() const { return false; }
  virtual Status ToProto(planpb::ScalarExpression* expr) const = 0;

 protected:
  ExpressionIR(int64_t id, IRNodeType type) : IRNode(id, type) {}
};

class MetadataProperty : public NotCopyable {
 public:
  MetadataProperty() = delete;
  virtual ~MetadataProperty() = default;

  /**
    @brief Returns a bool that notifies whether an expression fits the expected format for this
   * property when comparing.  This is used to make sure comparison operations (==, >, <, !=) are
   * pre-checked during compilation, preventing unnecssary operations during execution and exposing
   * query errors to the user.
   *
   * For example, we expect values compared to POD_NAMES to be Strings of the format
   * `<namespace>/<pod-name>`.
   *
   * ExplainFormat should describe the expected format of this method in string form.
   *
   * @param value: the ExpressionIR node that
   */
  virtual bool ExprFitsFormat(ExpressionIR* value) const = 0;

  /**
   * @brief Describes the Expression format that ExprFitsFormat expects.
   * This will be passed up to query writing users so you should prioritize pythonic descriptions
   * over internal compiler jargon.
   */
  virtual std::string ExplainFormat() const = 0;

  /**
   * @brief Returns the Carnot column-name as a string.
   */
  inline std::string GetColumnRepr() const { return FormatMetadataColumn(name_); }

  /**
   * @brief Returns the key columns formatted as metadata columns.
   */
  std::vector<std::string> GetKeyColumnReprs() const {
    std::vector<std::string> columns;
    for (const auto& c : key_columns_) {
      columns.push_back(FormatMetadataColumn(c));
    }
    return columns;
  }

  /**
   * @brief Returns whether this metadata key can be obtained by the provided key column.
   * @param key: the column name string as seen in Carnot.
   */
  inline bool HasKeyColumn(const std::string_view key) {
    auto columns = GetKeyColumnReprs();
    return std::find(columns.begin(), columns.end(), key) != columns.end();
  }

  /**
   * @brief Returns the udf-string that converts a given key column to the Metadata
   * represented by this property.
   */
  StatusOr<std::string> UDFName(const std::string_view key) {
    if (!HasKeyColumn(key)) {
      return error::InvalidArgument(
          "Key column $0 invalid for metadata value $1. Expected one of [$2].", key, name_,
          absl::StrJoin(key_columns_, ","));
    }
    return absl::Substitute("$1_to_$0", name_, ExtractMetadataFromColumnName(key));
  }

  // Getters.
  inline std::string name() const { return name_; }
  inline metadatapb::MetadataType metadata_type() const { return metadata_type_; }
  inline types::DataType column_type() const { return column_type_; }

  /**
   * @brief Return a string that adds the Metadata column prefix to the passed in argument.
   */
  inline static std::string FormatMetadataColumn(const std::string_view col_name) {
    return absl::Substitute("$0$1", kMetadataColumnPrefix, col_name);
  }
  inline static std::string GetMetadataString(metadatapb::MetadataType metadata_type) {
    if (metadata_type == metadatapb::MetadataType::UPID) {
      return kUniquePIDColumn;
    }
    std::string name = metadatapb::MetadataType_Name(metadata_type);
    absl::AsciiStrToLower(&name);
    return name;
  }
  inline static std::string FormatMetadataColumn(metadatapb::MetadataType metadata_type) {
    if (metadata_type == metadatapb::MetadataType::UPID) {
      return kUniquePIDColumn;
    }
    return FormatMetadataColumn(GetMetadataString(metadata_type));
  }

  /**
   * @brief Strips the Metadata prefix from a given Carnot, column-name representation.
   * If you pass in a column without the prefix, it _does not_ throw an error.
   * @param column_name
   */
  inline static std::string ExtractMetadataFromColumnName(const std::string_view column_name) {
    return std::string(absl::StripPrefix(column_name, kMetadataColumnPrefix));
  }

  /**
   * @brief The metadata prefix for columns in Carnot.
   */
  inline static const char kMetadataColumnPrefix[] = "_attr_";
  /**
   * @brief The string representation of the unique pid column.
   */
  inline static constexpr char kUniquePIDColumn[] = "upid";

 protected:
  MetadataProperty(metadatapb::MetadataType metadata_type, types::DataType column_type,
                   std::vector<metadatapb::MetadataType> key_columns)
      : metadata_type_(metadata_type), column_type_(column_type), key_columns_(key_columns) {
    name_ = GetMetadataString(metadata_type);
  }

 private:
  metadatapb::MetadataType metadata_type_;
  types::DataType column_type_;
  std::string name_;
  std::vector<metadatapb::MetadataType> key_columns_;
};

class DataIR : public ExpressionIR {
 public:
  types::DataType EvaluatedDataType() const override { return evaluated_data_type_; }
  bool IsDataTypeEvaluated() const override { return true; }
  bool IsData() const override { return true; }

  /**
   * @brief ToProto method override that writes ScalarValues to the ScalarExpression message.
   *
   * @param expr
   * @return Status
   */
  Status ToProto(planpb::ScalarExpression* expr) const override;

  /**
   * @brief ToProto method that takes in a scalar message instead of a ScalarExpression.
   *
   * @param column_pb
   * @return Status
   */
  Status ToProto(planpb::ScalarValue* column_pb) const;

  /**
   * @brief The implementation of ToProto for DataIR derived classes.
   * Each implementation should only be one line such as
   * `value->set_int64_value`
   *
   * @param value the value protobuf to use.
   * @return Status
   */
  virtual Status ToProtoImpl(planpb::ScalarValue* value) const = 0;

 protected:
  DataIR(int64_t id, IRNodeType type, types::DataType data_type)
      : ExpressionIR(id, type), evaluated_data_type_(data_type) {}

 private:
  types::DataType evaluated_data_type_;
};

/**
 * @brief ColumnIR wraps around columns in the plan.
 *
 * Columns have two important relationships that can easily get confused with one another.
 *
 * _Relationships_:
 * 1. A column is contained by an Operator.
 * 2. A column has an Operator that it references.
 *
 * The first relationship applies to Operators that utilize expressions (ie Map, Agg). Any
 * ColumnIR that is a progeny of an expression in an Operator means the Operator contains that
 * ColumnIR.
 *
 * The second relationship is between the ColumnIR and the parent of the Containing Operator (in
 * relationship #1) that the ColumnIR refers to. A ColumnIR that references an operator means it's
 * value is determined by the Operator and subsequently the Operator's output relation must
 * contain the column name used by this column.
 *
 * Because we are constantly shuffling parents of each Operator, ColumnIR's
 * methods are meant to be decoupled from the Operator that it references. Instead, ColumnIR
 * finds the referenced operator by first getting the containing operator, then grabbing the
 * saved index of that containing operator to point to a parent of that operator. This is under the
 * assumption that once we initialize an operator, the number of parents it has never changes.
 *
 * To get the Referenced Operator for a column, the Column accesses its saved index of the
 * Containing Operator's parents vector.
 *
 */
class ColumnIR : public ExpressionIR {
 public:
  ColumnIR() = delete;
  explicit ColumnIR(int64_t id) : ExpressionIR(id, IRNodeType::kColumn) {}

  /**
   * @brief Creates a new column that references the passed in operator.
   *
   * @param col_name: The name of the column.
   * @param parent_op_idx: The index of the parent operator that this column references.
   * @return Status: error container.
   */
  Status Init(const std::string& col_name, int64_t parent_op_idx);

  std::string col_name() const { return col_name_; }

  bool IsColumn() const override { return true; }
  void ResolveColumn(int64_t col_idx, types::DataType type) {
    col_idx_ = col_idx;
    evaluated_data_type_ = type;
    is_data_type_evaluated_ = true;
  }

  /**
   * @brief The operators containing this column. There can be multiple, but all of these operators
   * should share a common parent (ReferencedOperators).
   *
   * @return OperatorIR*: the operators containing this column
   */
  StatusOr<std::vector<OperatorIR*>> ContainingOperators() const;

  /**
   * @brief The operator that this column references. This should be the parent of the operator that
   * contains this column.
   *
   * @return OperatorIR*: the operator this column references.
   */
  StatusOr<OperatorIR*> ReferencedOperator() const;

  // TODO(philkuz) (PL-826): redo DebugString to have a DebugStringImpl instead of override.
  // NOLINTNEXTLINE(readability/inheritance)
  virtual std::string DebugString() const override;
  StatusOr<int64_t> ReferenceID() const {
    PL_ASSIGN_OR_RETURN(OperatorIR * referenced_op, ReferencedOperator());
    return referenced_op->id();
  }
  types::DataType EvaluatedDataType() const override { return evaluated_data_type_; }
  bool IsDataTypeEvaluated() const override { return is_data_type_evaluated_; }

  int64_t col_idx() const { return col_idx_; }
  int64_t container_op_parent_idx() const { return container_op_parent_idx_; }
  bool container_op_parent_idx_set() const { return container_op_parent_idx_set_; }

  /**
   * @brief Override CopyFromNode to make sure all Column classes save the column attributes.
   */
  Status CopyFromNode(const IRNode* node,
                      absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;
  void SetContainingOperatorParentIdx(int64_t container_op_parent_idx);

  /**
   * @brief ToProto method for the Column class and derived classes.
   *
   * @param expr
   * @return error if we can't convert the Column into an expression.
   */
  Status ToProto(planpb::ScalarExpression* expr) const override;

  /**
   * @brief ToProto method that takes in a column message instead of a ScalarExpression.
   *
   * @param column_pb
   * @return Status
   */
  Status ToProto(planpb::Column* column_pb) const;

 protected:
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;
  /**
   * @brief Optional protected constructor for children types.
   */
  ColumnIR(int64_t id, IRNodeType type) : ExpressionIR(id, type) {}

  /**
   * @brief Point to the index of the Containing operator's parents that this column references.
   *
   * @param container_op_parent_idx: the index of the container_op.
   */

  void SetColumnName(const std::string& col_name) {
    col_name_ = col_name;
    col_name_set_ = true;
  }

 private:
  std::string col_name_;
  bool col_name_set_ = false;
  // The column index in the relation.
  int64_t col_idx_;
  types::DataType evaluated_data_type_;
  bool is_data_type_evaluated_ = false;

  int64_t container_op_parent_idx_ = -1;
  bool container_op_parent_idx_set_ = false;
};

/**
 * @brief StringIR wraps around the String AST node
 * and only contains the value of that string.
 *
 */
class StringIR : public DataIR {
 public:
  StringIR() = delete;
  explicit StringIR(int64_t id) : DataIR(id, IRNodeType::kString, types::DataType::STRING) {}
  Status Init(std::string str);
  std::string str() const { return str_; }
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

  Status ToProtoImpl(planpb::ScalarValue* value) const override;

 private:
  std::string str_;
};

class UInt128IR : public DataIR {
 public:
  UInt128IR() = delete;
  explicit UInt128IR(int64_t id) : DataIR(id, IRNodeType::kUInt128, types::DataType::UINT128) {}

  /**
   * @brief Inits the UInt128 from a string.
   *
   * @param val
   * @return Status
   */
  Status Init(absl::uint128 val);
  absl::uint128 val() const { return val_; }
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

  Status ToProtoImpl(planpb::ScalarValue* value) const override;

 private:
  absl::uint128 val_;
};

/**
 * @brief CollectionIR wraps around collections (lists, tuples). Will maintain a
 * vector of pointers to the contained nodes in the collection.
 *
 */
class CollectionIR : public ExpressionIR {
 public:
  CollectionIR() = delete;
  CollectionIR(int64_t id, IRNodeType type) : ExpressionIR(id, type) {}
  Status Init(const std::vector<ExpressionIR*>& children);

  std::vector<ExpressionIR*> children() const { return children_; }
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override =
      0;

  bool IsCollection() const override { return true; }
  bool IsDataTypeEvaluated() const override { return true; }
  types::DataType EvaluatedDataType() const override { return types::DATA_TYPE_UNKNOWN; }

  Status ToProto(planpb::ScalarExpression*) const override {
    return error::Unimplemented("Collections aren't supported in expressions.");
  }

 protected:
  Status CopyFromCollection(const CollectionIR* source,
                            absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map);
  Status SetChildren(const std::vector<ExpressionIR*>& children);

 private:
  std::vector<ExpressionIR*> children_;
};

class ListIR : public CollectionIR {
 public:
  ListIR() = delete;
  explicit ListIR(int64_t id) : CollectionIR(id, IRNodeType::kList) {}
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;
};

class TupleIR : public CollectionIR {
 public:
  TupleIR() = delete;
  explicit TupleIR(int64_t id) : CollectionIR(id, IRNodeType::kTuple) {}
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;
};

struct ColumnExpression {
  ColumnExpression(std::string col_name, ExpressionIR* expr) : name(col_name), node(expr) {}
  std::string name;
  ExpressionIR* node;
};
using ColExpressionVector = std::vector<ColumnExpression>;

/**
 * @brief Represents functions with arbitrary number of values
 */
class FuncIR : public ExpressionIR {
 public:
  enum Opcode {
    non_op = -1,
    mult,
    sub,
    add,
    div,
    eq,
    neq,
    lteq,
    gteq,
    lt,
    gt,
    logand,
    logor,
    mod,
    number_of_ops
  };
  struct Op {
    Opcode op_code;
    std::string python_op;
    std::string carnot_op_name;
  };
  static std::unordered_map<std::string, Op> op_map;

  FuncIR() = delete;
  Opcode opcode() const { return op_.op_code; }
  const Op& op() const { return op_; }
  explicit FuncIR(int64_t id) : ExpressionIR(id, IRNodeType::kFunc) {}
  Status Init(Op op, const std::vector<ExpressionIR*>& args);

  // NOLINTNEXTLINE(readability/inheritance)
  virtual std::string DebugString() const override {
    return absl::Substitute("$0(id=$1, $2)", func_name(), id(),
                            absl::StrJoin(args_, ",", [](std::string* out, IRNode* in) {
                              absl::StrAppend(out, in->DebugString());
                            }));
  }

  std::string func_name() const {
    return absl::Substitute("$0.$1", func_prefix_, op_.carnot_op_name);
  }
  std::string carnot_op_name() const { return op_.carnot_op_name; }

  int64_t func_id() const { return func_id_; }
  void set_func_id(int64_t func_id) { func_id_ = func_id; }
  const std::vector<ExpressionIR*>& args() const { return args_; }
  const std::vector<types::DataType>& args_types() const { return args_types_; }
  void SetArgsTypes(std::vector<types::DataType> args_types) { args_types_ = args_types; }
  // TODO(philkuz) figure out how to combine this with set_func_id.
  void SetOutputDataType(types::DataType type) {
    evaluated_data_type_ = type;
    is_data_type_evaluated_ = true;
  }
  Status UpdateArg(int64_t idx, ExpressionIR* arg) {
    CHECK_LT(idx, static_cast<int64_t>(args_.size()))
        << "Tried to update arg of index greater than number of args.";
    ExpressionIR* old_arg = args_[idx];
    args_[idx] = arg;
    PL_RETURN_IF_ERROR(graph_ptr()->DeleteEdge(this, old_arg));
    PL_RETURN_IF_ERROR(graph_ptr()->AddEdge(this, arg));
    return Status::OK();
  }

  Status AddArg(ExpressionIR* arg);
  // Adds the arg if it isn't already present in the func, otherwise clones it so that there is no
  // duplicate edge.
  Status AddOrCloneArg(ExpressionIR* arg);

  types::DataType EvaluatedDataType() const override { return evaluated_data_type_; }
  bool IsDataTypeEvaluated() const override { return is_data_type_evaluated_; }
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

  Status ToProto(planpb::ScalarExpression* expr) const override;
  bool IsFunction() const override { return true; }

 private:
  std::string func_prefix_ = kPLFuncPrefix;
  Op op_;
  std::string func_name_;
  std::vector<ExpressionIR*> args_;
  std::vector<types::DataType> args_types_;
  int64_t func_id_ = 0;
  types::DataType evaluated_data_type_ = types::DataType::DATA_TYPE_UNKNOWN;
  bool is_data_type_evaluated_ = false;
};

/**
 * @brief Primitive values.
 */
class FloatIR : public DataIR {
 public:
  FloatIR() = delete;
  explicit FloatIR(int64_t id) : DataIR(id, IRNodeType::kFloat, types::DataType::FLOAT64) {}
  Status Init(double val);

  double val() const { return val_; }
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

  Status ToProtoImpl(planpb::ScalarValue* value) const override;

 private:
  double val_;
};

class IntIR : public DataIR {
 public:
  IntIR() = delete;
  explicit IntIR(int64_t id) : DataIR(id, IRNodeType::kInt, types::DataType::INT64) {}
  Status Init(int64_t val);

  int64_t val() const { return val_; }
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

  std::string DebugString() const override {
    return absl::Substitute("$0, $1)", DataIR::DebugString(), val());
  }
  Status ToProtoImpl(planpb::ScalarValue* value) const override;

 private:
  int64_t val_;
};

class BoolIR : public DataIR {
 public:
  BoolIR() = delete;
  explicit BoolIR(int64_t id) : DataIR(id, IRNodeType::kBool, types::DataType::BOOLEAN) {}
  Status Init(bool val);

  bool val() const { return val_; }
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

  Status ToProtoImpl(planpb::ScalarValue* value) const override;

 private:
  bool val_;
};

class TimeIR : public DataIR {
 public:
  TimeIR() = delete;
  explicit TimeIR(int64_t id) : DataIR(id, IRNodeType::kTime, types::DataType::TIME64NS) {}
  Status Init(int64_t val);

  bool val() const { return val_ != 0; }
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;
  Status ToProtoImpl(planpb::ScalarValue* value) const override;

 private:
  int64_t val_;
};

class MetadataResolverIR;

class MetadataIR : public ColumnIR {
 public:
  MetadataIR() = delete;
  explicit MetadataIR(int64_t id) : ColumnIR(id, IRNodeType::kMetadata) {}
  Status Init(const std::string& metadata_val, int64_t parent_op_idx);

  std::string name() const { return metadata_name_; }
  bool HasMetadataResolver() const { return has_metadata_resolver_; }

  Status ResolveMetadataColumn(MetadataResolverIR* resolver_op, MetadataProperty* property);
  MetadataResolverIR* resolver() const { return resolver_; }
  MetadataProperty* property() const { return property_; }

  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

  std::string DebugString() const override;

 private:
  std::string metadata_name_;
  bool has_metadata_resolver_ = false;
  MetadataResolverIR* resolver_;
  MetadataProperty* property_;
};

/**
 * @brief MetadataLiteral is a representation for any literal (DataIR)
 * that we know is properly formatted for some function.
 */
class MetadataLiteralIR : public ExpressionIR {
 public:
  MetadataLiteralIR() = delete;
  explicit MetadataLiteralIR(int64_t id) : ExpressionIR(id, IRNodeType::kMetadataLiteral) {}
  Status Init(DataIR* literal);

  IRNodeType literal_type() const {
    CHECK(literal_ != nullptr);
    return literal_->type();
  }
  DataIR* literal() const { return literal_; }
  bool IsDataTypeEvaluated() const override { return literal_->IsDataTypeEvaluated(); }
  types::DataType EvaluatedDataType() const override { return literal_->EvaluatedDataType(); }

  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;
  Status SetLiteral(DataIR* literal);

  Status ToProto(planpb::ScalarExpression* expr) const override;

 private:
  DataIR* literal_;
};

/**
 * @brief The MemorySourceIR is a dual logical plan
 * and IR node operator. It inherits from both classes
 */
class MemorySourceIR : public OperatorIR {
 public:
  MemorySourceIR() = delete;
  explicit MemorySourceIR(int64_t id)
      : OperatorIR(id, IRNodeType::kMemorySource, /* has_parents */ false, /* is_source */ true) {}

  /**
   * @brief Initialize the memory source.
   *
   * @param table_name the table to load.
   * @param select_columns the columns to select. If vector is empty, then select all columns.
   * @return Status
   */
  Status Init(const std::string& table_name, const std::vector<std::string>& select_columns);

  std::string table_name() const { return table_name_; }

  Status SetTimeExpressions(ExpressionIR* start_time_expr, ExpressionIR* end_time_expr);

  // Sets the time expressions that eventually get converted
  ExpressionIR* start_time_expr() const { return start_time_expr_; }
  ExpressionIR* end_time_expr() const { return end_time_expr_; }
  bool HasTimeExpressions() const { return has_time_expressions_; }

  void SetTimeValuesNS(int64_t time_start_ns, int64_t time_stop_ns) {
    time_start_ns_ = time_start_ns;
    time_stop_ns_ = time_stop_ns;
    time_set_ = true;
  }
  bool IsTimeSet() const { return time_set_; }

  int64_t time_start_ns() const { return time_start_ns_; }
  int64_t time_stop_ns() const { return time_stop_ns_; }

  const std::vector<int64_t>& column_index_map() const { return column_index_map_; }
  bool column_index_map_set() const { return column_index_map_set_; }
  void SetColumnIndexMap(const std::vector<int64_t>& column_index_map) {
    column_index_map_set_ = true;
    column_index_map_ = column_index_map;
  }

  Status ToProto(planpb::Operator*) const override;

  bool select_all() const { return column_names_.size() == 0; }

  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;
  const std::vector<std::string>& column_names() const { return column_names_; }
  void SetTabletValue(const types::TabletID& tablet_value) {
    tablet_value_ = tablet_value;
    has_tablet_value_ = true;
  }
  bool HasTablet() const { return has_tablet_value_; }

  const types::TabletID& tablet_value() const {
    DCHECK(HasTablet());
    return tablet_value_;
  }

 private:
  std::string table_name_;

  bool has_time_expressions_ = false;
  ExpressionIR* start_time_expr_ = nullptr;
  ExpressionIR* end_time_expr_ = nullptr;

  bool time_set_ = false;
  int64_t time_start_ns_ = 0;
  int64_t time_stop_ns_ = 0;

  // Hold of columns in the order that they are selected.
  std::vector<std::string> column_names_;

  // The mapping of the source's column indices to the current columns, as given by column_names_.
  std::vector<int64_t> column_index_map_;
  bool column_index_map_set_ = false;

  types::TabletID tablet_value_;
  bool has_tablet_value_ = false;
};

/**
 * The MemorySinkIR describes the MemorySink operator.
 */
class MemorySinkIR : public OperatorIR {
 public:
  MemorySinkIR() = delete;
  explicit MemorySinkIR(int64_t id)
      : OperatorIR(id, IRNodeType::kMemorySink, /* has_parents */ true, /* is_source */ false) {}

  std::string name() const { return name_; }
  void set_name(const std::string& name) { name_ = name; }
  Status ToProto(planpb::Operator*) const override;

  Status Init(OperatorIR* parent, const std::string& name,
              const std::vector<std::string> out_columns);

  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;
  const std::vector<std::string>& out_columns() const { return out_columns_; }
  bool IsBlocking() const override { return true; }

 private:
  std::string name_;
  std::vector<std::string> out_columns_;
};

/**
 * @brief The MetadataResolverIR is a IR-only operation that
 * adds metadata as a column into the query.
 * At the end of the analyzer stage of the compiler this becomes a map node.
 */
class MetadataResolverIR : public OperatorIR {
 public:
  MetadataResolverIR() = delete;
  explicit MetadataResolverIR(int64_t id)
      : OperatorIR(id, IRNodeType::kMetadataResolver, true, false) {}
  Status ToProto(planpb::Operator*) const override {
    return error::Unimplemented("Calling ToProto on $0, which lacks a Protobuf representation.",
                                type_string());
  }

  Status Init(OperatorIR* parent) { return AddParent(parent); }

  Status AddMetadata(MetadataProperty* md_property);
  bool HasMetadataColumn(const std::string& type);
  std::map<std::string, MetadataProperty*> metadata_columns() const { return metadata_columns_; }
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

 private:
  std::map<std::string, MetadataProperty*> metadata_columns_;
};

/**
 * @brief The MapIR is a container for Map operators.
 * Describes a projection, which is describe in col_exprs().
 */
class MapIR : public OperatorIR {
 public:
  MapIR() = delete;
  explicit MapIR(int64_t id) : OperatorIR(id, IRNodeType::kMap, true, false) {}

  Status Init(OperatorIR* parent, const ColExpressionVector& col_exprs, bool keep_input_columns);

  const ColExpressionVector& col_exprs() const { return col_exprs_; }
  Status SetColExprs(const ColExpressionVector& exprs);
  Status AddColExpr(const ColumnExpression& expr);
  Status UpdateColExpr(std::string_view name, ExpressionIR* expr);
  Status ToProto(planpb::Operator*) const override;
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

  bool keep_input_columns() const { return keep_input_columns_; }
  void set_keep_input_columns(bool keep_input_columns) { keep_input_columns_ = keep_input_columns; }

 private:
  // The map from new column_names to expressions.
  ColExpressionVector col_exprs_;
  bool keep_input_columns_ = false;
};

/**
 * @brief The DropIR is a container for Drop column operators.
 * It eventually compiles down to a Map node.
 */
class DropIR : public OperatorIR {
 public:
  DropIR() = delete;
  explicit DropIR(int64_t id) : OperatorIR(id, IRNodeType::kDrop, true, false) {}
  Status Init(OperatorIR* parent, const std::vector<std::string>& drop_cols);

  Status ToProto(planpb::Operator*) const override;

  const std::vector<std::string>& col_names() const { return col_names_; }

  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

 private:
  // Names of the columns to drop.
  std::vector<std::string> col_names_;
};

/**
 * @brief The BlockingAggIR is the IR representation for the Agg operator.
 * GroupBy groups() and Aggregate columns according to aggregate_expressions().
 */
class BlockingAggIR : public OperatorIR {
 public:
  BlockingAggIR() = delete;
  explicit BlockingAggIR(int64_t id) : OperatorIR(id, IRNodeType::kBlockingAgg, true, false) {}

  std::vector<ColumnIR*> groups() const { return groups_; }
  bool group_by_all() const { return groups_.size() == 0; }
  ColExpressionVector aggregate_expressions() const { return aggregate_expressions_; }
  Status ToProto(planpb::Operator*) const override;
  Status EvaluateAggregateExpression(planpb::AggregateExpression* expr,
                                     const ExpressionIR& ir_node) const;

  Status Init(OperatorIR* parent, const std::vector<ColumnIR*>& groups,
              const ColExpressionVector& agg_expr);

  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

  inline bool IsBlocking() const override { return true; }

  Status AddGroup(ColumnIR* new_group) {
    groups_.push_back(new_group);
    return graph_ptr()->AddEdge(this, new_group);
  }

 private:
  Status SetAggExprs(const ColExpressionVector& agg_expr);
  Status SetGroups(const std::vector<ColumnIR*>& groups);

  // Contains group_names and groups columns.
  std::vector<ColumnIR*> groups_;
  // The map from value_names to values
  ColExpressionVector aggregate_expressions_;
};

class GroupByIR : public OperatorIR {
 public:
  GroupByIR() = delete;
  explicit GroupByIR(int64_t id) : OperatorIR(id, IRNodeType::kGroupBy, true, false) {}
  Status Init(OperatorIR* parent, const std::vector<ColumnIR*>& groups);
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

  std::vector<ColumnIR*> groups() const { return groups_; }

  // GroupBy does not exist as a protobuf object.
  Status ToProto(planpb::Operator*) const override {
    return error::Unimplemented("ToProto not implemented.");
  }

 private:
  Status SetGroups(const std::vector<ColumnIR*>& groups);
  // contains group_names and groups columns.
  std::vector<ColumnIR*> groups_;
};

class FilterIR : public OperatorIR {
 public:
  FilterIR() = delete;
  explicit FilterIR(int64_t id) : OperatorIR(id, IRNodeType::kFilter, true, false) {}

  ExpressionIR* filter_expr() const { return filter_expr_; }
  Status SetFilterExpr(ExpressionIR* expr);
  Status ToProto(planpb::Operator*) const override;

  Status Init(OperatorIR* parent, ExpressionIR* expr);

  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

 private:
  ExpressionIR* filter_expr_ = nullptr;
};

class LimitIR : public OperatorIR {
 public:
  LimitIR() = delete;
  explicit LimitIR(int64_t id) : OperatorIR(id, IRNodeType::kLimit, true, false) {}

  Status ToProto(planpb::Operator*) const override;
  void SetLimitValue(int64_t value) {
    limit_value_ = value;
    limit_value_set_ = true;
  }
  bool limit_value_set() const { return limit_value_set_; }
  int64_t limit_value() const { return limit_value_; }

  Status Init(OperatorIR* parent, int64_t limit_value);

  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

 private:
  int64_t limit_value_;
  bool limit_value_set_ = false;
};

/**
 * @brief IR for the network sink operator that passes batches over GRPC to the destination.
 *
 * Setting up the Sink Operator requires three steps.
 * 0. Init(int destination_id): Set the destination id.
 * 1. SetDistributedID(string): Set the name of the node same as the query broker.
 * 2. SetDestinationAddress(string): the GRPC address where batches should be sent.
 */
class GRPCSinkIR : public OperatorIR {
 public:
  explicit GRPCSinkIR(int64_t id)
      : OperatorIR(id, IRNodeType::kGRPCSink, /* has_parents */ true,
                   /* is_source */ false) {}

  Status Init(OperatorIR* parent, int64_t destination_id) {
    PL_RETURN_IF_ERROR(AddParent(parent));
    destination_id_ = destination_id;
    return Status::OK();
  }
  Status ToProto(planpb::Operator* op_pb) const override;

  /**
   * @brief The id used for initial mapping. This associates a GRPCSink with a subsequent
   * GRPCSourceGroup.
   *
   * Once the Distributed Plan is established, you should use DistributedDestinationID().
   */
  int64_t destination_id() const { return destination_id_; }
  void SetDestinationID(int64_t destination_id) { destination_id_ = destination_id; }
  void SetDestinationAddress(const std::string& address) { destination_address_ = address; }

  const std::string& destination_address() const { return destination_address_; }
  bool DestinationAddressSet() const { return destination_address_ != ""; }
  inline bool IsBlocking() const override { return true; }

 protected:
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

 private:
  int64_t destination_id_ = -1;
  std::string destination_address_ = "";
};

/**
 * @brief This is the GRPC Source group that is what will appear in the physical plan.
 * Each GRPCSourceGroupIR will end up being converted into a set of GRPCSourceIR for the
 * corresponding remote_source_ids.
 */
class GRPCSourceIR : public OperatorIR {
 public:
  explicit GRPCSourceIR(int64_t id)
      : OperatorIR(id, IRNodeType::kGRPCSource, /* has_parents */ false,
                   /* is_source */ true) {}
  Status ToProto(planpb::Operator* op_pb) const override;

  /**
   * @brief Special Init that skips around the Operator init function.
   *
   * @param relation
   * @return Status
   */
  Status Init(const table_store::schema::Relation& relation) { return SetRelation(relation); }

 protected:
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;
};

/**
 * @brief This is an IR only node that is used to mark where a GRPC source should go.
 * For the actual plan, this operator is replaced with a series of GRPCSourceOperators that are
 * Unioned together.
 */
class GRPCSourceGroupIR : public OperatorIR {
 public:
  explicit GRPCSourceGroupIR(int64_t id)
      : OperatorIR(id, IRNodeType::kGRPCSourceGroup, /* has_parents */ false,
                   /* is_source */ true) {}
  Status ToProto(planpb::Operator* op_pb) const override;

  /**
   * @brief Special Init that skips around the Operator init function.
   *
   * @param source_id
   * @param relation
   * @return Status
   */
  Status Init(int64_t source_id, const table_store::schema::Relation& relation) {
    source_id_ = source_id;
    return SetRelation(relation);
  }

  void SetGRPCAddress(const std::string& grpc_address) { grpc_address_ = grpc_address; }

  /**
   * @brief Associate the passed in GRPCSinkOperator with this Source Group. The sink_op passed in
   * will most likely exist outside of the graph this contains, so instead of holding a pointer, we
   * hold the information that will be used during execution in Vizier.
   *
   * @param sink_op: the sink operator that should be connected with this source operator.
   * @return Status: error if this->source_id and sink_op->destination_id don't line up.
   */
  Status AddGRPCSink(GRPCSinkIR* sink_op);
  bool GRPCAddressSet() const { return grpc_address_ != ""; }
  const std::string& grpc_address() const { return grpc_address_; }
  int64_t source_id() const { return source_id_; }
  std::vector<GRPCSinkIR*> dependent_sinks() { return dependent_sinks_; }

 protected:
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

 private:
  int64_t source_id_ = -1;
  std::string grpc_address_ = "";
  std::vector<GRPCSinkIR*> dependent_sinks_;
};

struct ColumnMapping {
  std::vector<int64_t> input_column_map;
};

class UnionIR : public OperatorIR {
 public:
  UnionIR() = delete;
  explicit UnionIR(int64_t id)
      : OperatorIR(id, IRNodeType::kUnion, /* has_parents */ true, /* is_source */ false) {}

  bool IsBlocking() const override { return true; }

  Status ToProto(planpb::Operator*) const override;
  Status Init(const std::vector<OperatorIR*>& parents);
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;
  Status SetRelationFromParents();
  bool HasColumnMappings() const { return column_mappings_.size() == parents().size(); }
  const std::vector<ColumnMapping>& column_mappings() const { return column_mappings_; }

 private:
  /**
   * @brief Set a parent operator's column mapping.
   *
   * @param parent_idx: the parents() idx this mapping applies to.
   * @param input_column_map: the mapping from 1 columns unions to another. [4] would indicate the
   * index-4 column of the input parent relation maps to index 0 of this relation.
   * @return Status
   */
  Status AddColumnMapping(const std::vector<int64_t>& input_column_map);
  std::vector<ColumnMapping> column_mappings_;
};

/**
 * @brief The Join Operator plan node.
 *
 */
class JoinIR : public OperatorIR {
 public:
  enum class JoinType { kLeft, kRight, kOuter, kInner };

  JoinIR() = delete;
  explicit JoinIR(int64_t id)
      : OperatorIR(id, IRNodeType::kJoin, /* has_parents */ true, /* is_source */ false) {}

  bool IsBlocking() const override { return true; }

  Status ToProto(planpb::Operator*) const override;

  /**
   * @brief JoinIR init to directly initialize the operator.
   *
   * @param parents
   * @param how_type
   * @param left_on_cols
   * @param right_on_cols
   * @param suffix_strs
   * @return Status
   */
  Status Init(const std::vector<OperatorIR*>& parents, const std::string& how_type,
              const std::vector<ColumnIR*>& left_on_cols,
              const std::vector<ColumnIR*>& right_on_cols,
              const std::vector<std::string>& suffix_strs);
  Status CopyFromNodeImpl(const IRNode* node,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;

  JoinType join_type() const { return join_type_; }
  const std::vector<ColumnIR*>& output_columns() const { return output_columns_; }
  const std::vector<std::string>& column_names() const { return column_names_; }
  Status SetJoinType(JoinType join_type) {
    join_type_ = join_type;
    return Status::OK();
  }
  Status SetJoinType(const std::string& join_type) {
    PL_ASSIGN_OR_RETURN(join_type_, GetJoinEnum(join_type));
    if (join_type_ == JoinType::kRight) {
      specified_as_right_ = true;
    }
    return Status::OK();
  }

  const std::vector<ColumnIR*>& left_on_columns() const { return left_on_columns_; }
  const std::vector<ColumnIR*>& right_on_columns() const { return right_on_columns_; }
  const std::vector<std::string>& suffix_strs() const { return suffix_strs_; }
  void SetSuffixStrs(const std::vector<std::string>& suffix_strs) { suffix_strs_ = suffix_strs; }
  Status SetOutputColumns(const std::vector<std::string>& column_names,
                          const std::vector<ColumnIR*> columns) {
    DCHECK_EQ(column_names.size(), columns.size());
    output_columns_ = columns;
    column_names_ = column_names;
    for (auto g : output_columns_) {
      PL_RETURN_IF_ERROR(graph_ptr()->AddEdge(this, g));
    }
    return Status::OK();
  }

  bool specified_as_right() const { return specified_as_right_; }

 private:
  /**
   * @brief Converts the string type to JoinIR::JoinType or errors out if it doesn't exist.
   *
   * @param join_type string representation of the join.
   * @return StatusOr<planpb::JoinOperator::JoinType> the join enum or an error if not found.
   */
  StatusOr<JoinType> GetJoinEnum(const std::string& join_type) const;

  /**
   * @brief Get the Protobuf JoinType for the JoinIR::JoinType
   *
   * @param join_type
   * @return planpb::JoinOperator::JoinType
   */
  static planpb::JoinOperator::JoinType GetPbJoinEnum(JoinType join_type);

  Status SetJoinColumns(const std::vector<ColumnIR*>& left_columns,
                        const std::vector<ColumnIR*>& right_columns);

  // Join type
  JoinType join_type_;
  // The columns that are output by this join operator.
  std::vector<ColumnIR*> output_columns_;
  // The column names to set.
  std::vector<std::string> column_names_;
  // The columns we join from the left parent.
  std::vector<ColumnIR*> left_on_columns_;
  // The columns we join from the right parent.
  std::vector<ColumnIR*> right_on_columns_;
  // The suffixes to add to the left columns and to the right columns.
  std::vector<std::string> suffix_strs_;

  // Whether this join was originally specified as a right join.
  // Used because we transform left joins into right joins but need to do some back transform.
  bool specified_as_right_ = false;
};

/*
 * @brief TabletSourceGroup should is the container for Tablets in the system.
 * It is a temporary representation that can then be used to convert a previous Memory
 * Source into a Union of sources with tablet key values.
 *
 */
class TabletSourceGroupIR : public OperatorIR {
 public:
  TabletSourceGroupIR() = delete;

  Status Init(MemorySourceIR* memory_source_ir, const std::vector<types::TabletID>& tablets,
              const std::string& tablet_key) {
    tablets_ = tablets;
    memory_source_ir_ = memory_source_ir;
    DCHECK(memory_source_ir->IsRelationInit());
    PL_RETURN_IF_ERROR(SetRelation(memory_source_ir->relation()));
    DCHECK(relation().HasColumn(tablet_key));
    tablet_key_ = tablet_key;
    return Status::OK();
  }

  explicit TabletSourceGroupIR(int64_t id)
      : OperatorIR(id, IRNodeType::kTabletSourceGroup, /* has_parents */ false,
                   /* is_source */ true) {}

  bool IsBlocking() const override { return false; }

  Status ToProto(planpb::Operator*) const override {
    return error::Unimplemented("$0::ToProto not implemented because no use found for it yet.",
                                DebugString());
  }
  Status CopyFromNodeImpl(const IRNode*, absl::flat_hash_map<const IRNode*, IRNode*>*) override {
    return error::Unimplemented("$0::CopyFromNode not implemented because no use found for it yet.",
                                DebugString());
  }

  const std::vector<types::TabletID>& tablets() const { return tablets_; }
  /**
   * @brief Returns the Memory source that was replaced by this node.
   * @return MemorySourceIR*
   */
  MemorySourceIR* ReplacedMemorySource() const { return memory_source_ir_; }

  const std::string tablet_key() const { return tablet_key_; }

 private:
  // The key in the relation that is used as a tablet_key.
  std::string tablet_key_;
  // The tablets that are associated with this node.
  std::vector<types::TabletID> tablets_;
  // The memory source that this node replaces. Deleted from the graph when this node is deleted.
  MemorySourceIR* memory_source_ir_;
};

class UDTFSourceIR : public OperatorIR {
 public:
  UDTFSourceIR() = delete;
  explicit UDTFSourceIR(int64_t id)
      : OperatorIR(id, IRNodeType::kUDTFSource, /* has_parents */ false,
                   /* is_source */ true) {}

  Status Init(std::string_view func_name,
              const absl::flat_hash_map<std::string, ExpressionIR*>& arg_values,
              const udfspb::UDTFSourceSpec& udtf_spec);

  /**
   * @brief Manages the overhead of setting argument values of the UDTFSourceOperator.
   *
   * @param expr
   * @return OK or error if one occurs.
   */
  Status SetArgValues(const std::vector<ExpressionIR*>& arg_values);

  Status InitArgValues(const absl::flat_hash_map<std::string, ExpressionIR*>& arg_values,
                       const udfspb::UDTFSourceSpec& udtf_spec);

  Status ToProto(planpb::Operator*) const override;
  std::string func_name() const { return func_name_; }
  udfspb::UDTFSourceSpec udtf_spec() const { return udtf_spec_; }

  Status CopyFromNodeImpl(const IRNode* source,
                          absl::flat_hash_map<const IRNode*, IRNode*>* copied_nodes_map) override;
  const std::vector<DataIR*>& arg_values() const { return arg_values_; }

 private:
  /**
   * @brief Subroutine for SetArgValues that converts expression into a vector.
   *
   * @param expr
   * @return StatusOr<DataIR*> the data that the expr represents, or an error if it
   * doesn't fit a format.
   */
  StatusOr<DataIR*> ProcessArgValue(ExpressionIR* expr);

  std::string func_name_;
  std::vector<DataIR*> arg_values_;
  udfspb::UDTFSourceSpec udtf_spec_;
};

}  // namespace compiler
}  // namespace carnot
}  // namespace pl
