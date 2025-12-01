#pragma once
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <optional>
#include <memory>

struct Unit
{
    std::string str {};
    std::string base_str {};
    double ratio {1.0};
    double offset {0.0};

    Unit(const std::string& str_ = "", const std::string& base_str_ = "", double ratio_ = 1.0, double offset_ = 0.0) :
        str {str_},
        base_str {(base_str_.empty()) ? str_ : base_str_},
        ratio {ratio_},
        offset {offset_}
    {}
};

enum class VariableSpec { Fixed, Free };

using Ndouble = std::optional<double>;

class Variable
{
public:
    std::string name {"foo"};
    double value {0.0};
    double init {0.0};
    Ndouble lower {std::nullopt};
    Ndouble upper {std::nullopt};
    double scale {1.0};
    Unit unit {"none"};
    VariableSpec spec{ VariableSpec::Free };

    Variable(const std::string& name_ = "unnamed", const Unit& unit_ = Unit("none")) :
        name {name_},
        unit {unit_}
    {}

    void fix() {spec = VariableSpec::Fixed;}
    double to_base() const {return value * unit.ratio + unit.offset;}
    double to_base(double value_) const {return value_ * unit.ratio + unit.offset;}
    double from_base(double base_value) const {return (base_value - unit.offset) / unit.ratio;}
    double from_base(double base_value, const Unit& u) const {return (base_value - u.offset) / u.ratio;}
    double convert(const Unit& u) const {return from_base(to_base(value), u);}
    double convert(double value_, const Unit& u) const {return from_base(to_base(value_), u);}

    operator double() const {return to_base();}
};

using VariablePtr = std::shared_ptr<Variable>;

struct Constraint
{
    std::string name {};
    double value {0.0};

    Constraint(const std::string& name_ = "") :
        name {name_}
    {}

    Constraint& operator=(const double& val) {value = val; return *this;}
    Constraint& operator+=(const double& val) {value += val; return *this;}
    Constraint& operator-=(const double& val) {value -= val; return *this;}
};

using ConstraintPtr = std::shared_ptr<Constraint>;

struct JacobianElement
{
    ConstraintPtr con;
    VariablePtr var;
    double value {};

    JacobianElement(ConstraintPtr con_ = nullptr, VariablePtr var_ = nullptr) :
        con {con_},
        var {var_}
    {}
};

using JacobianElementPtr = std::shared_ptr<JacobianElement>;

struct HessianElement
{
    ConstraintPtr con;
    VariablePtr var1;
    VariablePtr var2;
    double value {};

    HessianElement(ConstraintPtr con_ = nullptr, VariablePtr var1_ = nullptr, VariablePtr var2_ = nullptr) :
        con {con_},
        var1 {var1_},
        var2 {var2_}
    {}
};

using HessianElementPtr = std::shared_ptr<HessianElement>;

class Model;
class Block;
class Flowsheet;

using ModelPtr = Model*;
using BlockPtr = Block*;
using FlowsheetPtr = std::shared_ptr<Flowsheet>;
using CompID = std::string;
using Comps = std::vector<CompID>;

Comps operator+(const Comps& c1, const Comps& c2);
Comps& operator+=(Comps& c1, const Comps& c2);

struct Stream 
{
    std::string name;
    FlowsheetPtr fs;
    Comps comps {};
    BlockPtr to {};
    BlockPtr from {};

    Stream() = default;
    Stream(const std::string& name_, FlowsheetPtr fs_, const Comps& comps_) :
        name {name_},
        fs {fs_},
        comps {comps_}
    {}

    bool has_comp(const CompID& compID) const
    {
        return std::find(comps.begin(), comps.end(), compID) != comps.end();
    }
};

using StreamPtr = std::shared_ptr<Stream>;

struct StreamVars
{
    VariablePtr total_mass {};
    std::unordered_map<CompID, VariablePtr> mass {};
    std::unordered_map<CompID, VariablePtr> massfrac {};
};

class Block
{
public:
    std::string name {};
    ModelPtr m {};
    FlowsheetPtr fs {};
    std::vector<StreamPtr> inlets {};
    std::vector<StreamPtr> outlets {};
    std::vector<VariablePtr> x {};
    std::unordered_map<StreamPtr, StreamVars> x_strm {};
    std::vector<ConstraintPtr> g {};
    std::vector<JacobianElementPtr> J {};
    std::vector<HessianElementPtr> H {};

    Block() = default;
    Block(const std::string&     name_,
          ModelPtr               m_,
          FlowsheetPtr           fs_,
          const std::vector<StreamPtr>& inlets_ = {},
          const std::vector<StreamPtr>& outlets_ = {}) :
        name {name_},
        m {m_},
        fs {fs_},
        inlets {inlets_},
        outlets {outlets_}
    {}
    virtual ~Block() = default;

    void make_stream_variables(const StreamPtr& strm);
    void make_stream_variables(const std::vector<StreamPtr>& strms1, const std::vector<StreamPtr>& strms2 = {});
    virtual void eval_constraints();
};

void finish_block(const BlockPtr blk);

class Flowsheet
{
public:
    std::string name;
    std::string path;
    std::string prefix;
    FlowsheetPtr parent;
    std::vector<FlowsheetPtr> children;
    std::unordered_map<std::string, BlockPtr> blocks;
    std::unordered_map<std::string, StreamPtr> streams;

    explicit Flowsheet(const std::string& name_ = "index", FlowsheetPtr parent_ = nullptr) :
        name {name_},
        parent {parent_}
    {
        if (parent == nullptr) {
            path = name;
            prefix = "";
        }
        else {
            path = parent->path + "." + name;
            prefix = parent->prefix + name + ".";
        }
    }
};

class Model
{
public:
    std::string name;
    FlowsheetPtr index_fs {};
    std::unordered_map<std::string, Unit> unit_set {};
    std::vector<VariablePtr> x {};
    std::unordered_map<std::string, VariablePtr> x_map {};
    std::vector<ConstraintPtr> g {};
    std::unordered_map<std::string, ConstraintPtr> g_map {};
    std::vector<JacobianElementPtr> J {};
    std::vector<HessianElementPtr> H {};

    Model(const std::string& name_, const std::unordered_map<std::string, Unit>& unit_set_) :
        name(name_),
        unit_set(unit_set_)
    {
        index_fs = std::make_shared<Flowsheet>("index", nullptr);
    }

    VariablePtr add_variable(const std::string &name_, const Unit &unit);
    ConstraintPtr add_constraint(const std::string& name_);
    JacobianElementPtr add_jacobian_element(const ConstraintPtr& con_, const VariablePtr& var_);
    HessianElementPtr add_hessian_element(const ConstraintPtr& con_, const VariablePtr& var1_, const VariablePtr& var2_);
    FlowsheetPtr add_flowsheet(const std::string& name_, FlowsheetPtr parent_) const;
    StreamPtr add_stream(const std::string& name_, FlowsheetPtr fs, Comps& comps) const;
};
