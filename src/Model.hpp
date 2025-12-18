#pragma once
#include <iostream>
#include <string>
#include <format>
#include <vector>
#include <set>
#include <unordered_map>
#include <map>
#include <utility>
#include <optional>
#include <memory>
#include "IpTNLP.hpp"

#define NO_BOUND 1.0e20;

using std::string;
using std::vector;
using std::unordered_map;
using std::shared_ptr;
using std::make_shared;
using std::format;
using std::ostream;
using std::cout;

using Ipopt::TNLP;
using Ipopt::Index;
using Ipopt::Number;
using Ipopt::IpoptData;
using Ipopt::IpoptCalculatedQuantities;
using Ipopt::SolverReturn;

struct Unit
{
    string str      {};
    string base_str {};
    double ratio    {1.0};
    double offset   {0.0};

    Unit(const string& str_      = "",
         const string& base_str_ = "",
         double        ratio_    = 1.0,
         double        offset_   = 0.0) :
        str      {str_},
        base_str {base_str_.empty() ? str_ : base_str_},
        ratio    {ratio_},
        offset   {offset_}
    {}
};

//---------------------------------------------------------

enum class VariableSpec { Fixed, Free };
using Ndouble = std::optional<double>;

string str(double d);
string str(Ndouble nd);
string str(VariableSpec spec);

class Variable
{
public:
    Index        ix    {};
    string       name  {};
    double       value {0.0};
    Ndouble      lower {};
    Ndouble      upper {};
    Unit         unit  {"none"};
    VariableSpec spec  {VariableSpec::Free};

    Variable(const string& name_ = "unnamed",
             const Unit& unit_   = Unit("none")) :
        name {name_},
        unit {unit_}
    {}

    void      fix() {spec = VariableSpec::Fixed;}
    double    to_base() const {return value * unit.ratio + unit.offset;}
    double    to_base(double value_) const {return value_ * unit.ratio + unit.offset;}
    Variable& from_base(double base_value) {value = (base_value - unit.offset) / unit.ratio; return *this;}
    //double from_base(double base_value, const Unit& u) const {return (base_value - u.offset) / u.ratio;}
    //double convert(const Unit& u) const {return from_base(to_base(value), u);}
    //double convert(double value_, const Unit& u) const {return from_base(to_base(value_), u);}

    string to_str() {return format("{:32}|{}|{}|{}|{}|{:8}|", name, str(spec), str(value),
        str(lower), str(upper), unit.str);}
    Variable& operator=(const double& val) {value = val; return *this;}
    operator double() const {return to_base();}
};

using VariablePtr = shared_ptr<Variable>;

ostream& operator<<(ostream& os, const VariablePtr& var);

//---------------------------------------------------------

struct Constraint
{
    Index  ix    {};
    string name  {};
    double value {0.0};

    Constraint(const string& name_ = "") :
        name {name_}
    {}

    Constraint& operator=(const double& val) {value = val; return *this;}
    Constraint& operator+=(const double& val) {value += val; return *this;}
    Constraint& operator-=(const double& val) {value -= val; return *this;}
};

using ConstraintPtr = shared_ptr<Constraint>;

//---------------------------------------------------------

struct JacobianElement
{
    ConstraintPtr con;
    VariablePtr var;
    double value {};

    JacobianElement(ConstraintPtr con_ = nullptr,
                    VariablePtr   var_ = nullptr) :
        con {con_},
        var {var_}
    {}

    JacobianElement& operator=(const double& val) {value = val; return *this;}
};

using JacobianElementPtr = shared_ptr<JacobianElement>;

//---------------------------------------------------------

struct HessianElement
{
    ConstraintPtr con;
    VariablePtr var1;
    VariablePtr var2;
    double value {};

    HessianElement(ConstraintPtr con_  = nullptr,
                   VariablePtr   var1_ = nullptr,
                   VariablePtr   var2_ = nullptr) :
        con  {con_},
        var1 {var1_},
        var2 {var2_}
    {}

    HessianElement& operator=(const double& val) {value = val; return *this;}
};

using HessianElementPtr = shared_ptr<HessianElement>;

class Model;
class Block;
class Flowsheet;

using ModelPtr = Model*;
using BlockPtr = shared_ptr<Block>;
using FlowsheetPtr = shared_ptr<Flowsheet>;
using CompID = string;
using Comps = vector<CompID>;

Comps operator+(const Comps& c1, const Comps& c2);
Comps& operator+=(Comps& c1, const Comps& c2);

//---------------------------------------------------------

struct Stream 
{
    string       name;
    FlowsheetPtr fs;
    Comps        comps {};
    BlockPtr     to    {};
    BlockPtr     from  {};

    Stream() = default;
    Stream(const string& name_,
           FlowsheetPtr  fs_, 
           const Comps&  comps_) :
        name  {name_},
        fs    {fs_},
        comps {comps_}
    {}

    bool has_comp(const CompID& compID) const
    {
        return std::find(comps.begin(), comps.end(), compID) != comps.end();
    }
};

using StreamPtr = shared_ptr<Stream>;

//---------------------------------------------------------

struct StreamVars
{
    VariablePtr                        total_mass {};
    unordered_map<CompID, VariablePtr> mass       {};
    unordered_map<CompID, VariablePtr> massfrac   {};
};

//---------------------------------------------------------

class Block
{
public:
    string                               name    {};
    FlowsheetPtr                         fs      {};
    vector<StreamPtr>                    inlets  {};
    vector<StreamPtr>                    outlets {};
    string                               prefix  {};
    vector<VariablePtr>                  x       {};
    unordered_map<StreamPtr, StreamVars> x_strm  {};
    vector<ConstraintPtr>                g       {};
    vector<JacobianElementPtr>           J       {};
    vector<HessianElementPtr>            H       {};

    Block() = default;
    Block(const string&            name_,
          FlowsheetPtr             fs_,
          const vector<StreamPtr>& inlets_,
          const vector<StreamPtr>& outlets_);
    virtual ~Block() = default;
    virtual void initialize() = 0;
    virtual void eval_constraints() = 0;
    virtual void eval_jacobian() = 0;
    virtual void eval_hessian() = 0;

private:
    void make_stream_variables(const StreamPtr& strm);
    void make_stream_variables();
    void set_inlet_stream_specs();
};

//---------------------------------------------------------

class Flowsheet : public std::enable_shared_from_this<Flowsheet>
{
public:
    string                           name;
    ModelPtr                         m;
    string                           path;
    string                           prefix;
    FlowsheetPtr                     parent;
    vector<FlowsheetPtr>             children;
    vector<BlockPtr>                 blocks;
    unordered_map<string, BlockPtr>  blocks_map;
    unordered_map<string, StreamPtr> streams;

    Flowsheet(const string& name_, 
              ModelPtr      m_,
              FlowsheetPtr  parent_ = nullptr) :
        name   {name_},
        m      {m_},
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

    FlowsheetPtr add_child(const string& name_);
    StreamPtr    add_stream(const string& name_,
                            const Comps&  comps);

    template<typename T, typename... blk_params_T>
    shared_ptr<T> add_block(const string&            name_,
                            const vector<StreamPtr>& inlets_,
                            const vector<StreamPtr>& outlets_,
                            blk_params_T&            ...blk_params)
    {
        auto fs = shared_from_this();
        auto blk = make_shared<T>(name_, fs, inlets_, outlets_, blk_params...);
        fs->blocks.push_back(blk);
        fs->blocks_map[blk->name] = blk;
        for (const auto& sin : blk->inlets)
            sin->to = blk;
        for (const auto& sout : blk->outlets)
            sout->from = blk;
        return blk;
    }

    void initialize();
    void eval_constraints();
    void eval_jacobian();
    void eval_hessian();
};

//---------------------------------------------------------

class Model : public TNLP
{
public:
    string name;
    FlowsheetPtr index_fs {};
    unordered_map<string, Unit> unit_set {};
    vector<VariablePtr> x_vec {};
    unordered_map<string, VariablePtr> x_map {};
    vector<ConstraintPtr> g_vec {};
    unordered_map<string, ConstraintPtr> g_map {};
    vector<JacobianElementPtr> J {};
    std::map<std::pair<Index, Index>, vector<HessianElementPtr>> H {};

    Model(const string& name_, const unordered_map<string, Unit>& unit_set_) :
        name(name_),
        unit_set(unit_set_)
    {
        index_fs = make_shared<Flowsheet>("index", this, nullptr);
        bool printiterate = true;
    }

    VariablePtr        add_variable(const string &name_, const Unit &unit);
    ConstraintPtr      add_constraint(const string& name_);
    JacobianElementPtr add_jacobian_element(const ConstraintPtr& con, const VariablePtr& var);
    HessianElementPtr  add_hessian_element(const ConstraintPtr& con, const VariablePtr& var1, const VariablePtr& var2);
    void               initialize() {index_fs->initialize();};
    void               eval_constraints() {index_fs->eval_constraints();};
    void               eval_jacobian() {index_fs->eval_jacobian();};
    void               eval_hessian() {index_fs->eval_hessian();};
    void               print_variables(ostream& os = cout);
    VariablePtr        var(const string& name_) {return x_map.contains(name_) ? x_map[name_] : nullptr;};
    
    virtual bool get_nlp_info(
        Index&          n,
        Index&          m,
        Index&          nnz_jac_g,
        Index&          nnz_h_lag,
        IndexStyleEnum& index_style) override;

    virtual bool get_bounds_info(
        Index   n,
        Number* x_l,
        Number* x_u,
        Index   m,
        Number* g_l,
        Number* g_u) override;

    virtual bool get_starting_point(
        Index   n,
        bool    init_x,
        Number* x_init,
        bool    init_z,
        Number* z_L,
        Number* z_U,
        Index   m,
        bool    init_lambda,
        Number* lambda) override;
      
   virtual bool eval_f(
        Index         n,
        const Number* x_in,
        bool          new_x,
        Number&       obj_value) override;

   virtual bool eval_grad_f(
        Index         n,
        const Number* x_in,
        bool          new_x,
        Number*       grad_f) override;

   virtual bool eval_g(
        Index         n,
        const Number* x_in,
        bool          new_x,
        Index         m,
        Number*       g_values) override;
     
   virtual bool eval_jac_g(
        Index         n,
        const Number* x_in,
        bool          new_x,
        Index         m,
        Index         nele_jac,
        Index*        iRow,
        Index*        jCol,
        Number*       values) override;
   
   virtual bool eval_h(
        Index         n,
        const Number* x_in,
        bool          new_x,
        Number        obj_factor,
        Index         m,
        const Number* lambda,
        bool          new_lambda,
        Index         nele_hess,
        Index*        iRow,
        Index*        jCol,
        Number*       values) override;
  
    virtual void finalize_solution(
        SolverReturn               status,
        Index                      n,
        const Number*              x_final,
        const Number*              z_L,
        const Number*              z_U,
        Index                      m,
        const Number*              g_final,
        const Number*              lambda,
        Number                     obj_value,
        const IpoptData*           ip_data,
        IpoptCalculatedQuantities* ip_cq) override;

};

//---------------------------------------------------------

