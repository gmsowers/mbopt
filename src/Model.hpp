#pragma once
#include <iostream>
#include <string>
#include <string_view>
#include <format>
#include <vector>
#include <unordered_map>
#include <map>
#include <utility>
#include <optional>
#include <memory>
#include "IpIpoptApplication.hpp"

#define NO_BOUND 1.0e20

using std::string;
using std::string_view;
using std::vector;
using std::unordered_map;
using std::shared_ptr;
using std::make_shared;
using std::unique_ptr;
using std::make_unique;
using std::format;
using std::ostream;
using std::cout;
using std::cerr;

using Ipopt::TNLP;
using Ipopt::Index;
using Ipopt::Number;
using Ipopt::IpoptData;
using Ipopt::IpoptCalculatedQuantities;
using Ipopt::SolverReturn;
using Ipopt::IpoptApplication;

struct UnitKind;

struct Unit
{
    string str     {};
    UnitKind* kind {};
    double ratio   {1.0};
    double offset  {0.0};

    Unit() = default;
    Unit(string_view str_,
         UnitKind*   kind_,
         double      ratio_    = 1.0,
         double      offset_   = 0.0) :
        str      {str_},
        kind     {kind_},
        ratio    {ratio_},
        offset   {offset_}
    {}
};

struct UnitKind
{
    string  str              {};
    string  base_unit_str    {};
    string  default_unit_str {};
    Unit* base_unit          {};
    Unit* default_unit       {};
};

struct UnitSet
{
    unordered_map<string, unique_ptr<UnitKind>> kinds {};
    unordered_map<string, unique_ptr<Unit>>     units {};

    UnitSet() = default;

    Unit* add_unit(const string& unit_str,
                     UnitKind*   unit_kind,
                     double      unit_ratio = 1.0,
                     double      unit_offset = 0.0);

    Unit* add_unit(const string& unit_str,
                   const string& unit_kind_str,
                   double        unit_ratio = 1.0,
                   double        unit_offset = 0.0) {
        return add_unit(unit_str, kinds[unit_kind_str].get(), unit_ratio, unit_offset);
    }

    UnitKind* add_kind(const string& unit_kind_str,
                       const string& base_unit_str,
                       const string& default_unit_str = "");

    Unit* get_default_unit(const string& unit_kind_str) {
        return kinds[unit_kind_str]->default_unit;
    }
};

//---------------------------------------------------------

class Model;
using ModelPtr = Model*;

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
    Unit*        unit  {};
    VariableSpec spec  {VariableSpec::Free};

    Variable() = default;
    Variable(string_view name_,
             Unit*       unit_) :
        name {name_},
        unit {unit_}
    {}

    void   fix()
        {spec = VariableSpec::Fixed;}
    void   free()
        {spec = VariableSpec::Free;}

    double convert_to_base() const
        {return value * unit->ratio + unit->offset;}
    double convert_to_base(double value_) const
        {return value_ * unit->ratio + unit->offset;}
    double convert_to_base(double value_, Unit* u) const
        {return value_ * u->ratio + u->offset;}
    double convert_from_base(double base_value) const
        {return (base_value - unit->offset) / unit->ratio;}
    void   convert_and_set(double base_value)
        {value = (base_value - unit->offset) / unit->ratio;}
    void   convert_and_set(double value_, Unit* u)
        {value = convert_from_base(convert_to_base(value_, u));}
    double convert(double value_, Unit* u) const
        {return (u == unit ? value_ : convert_from_base(convert_to_base(value_, u)));}

    Ndouble change_unit(ModelPtr m, const string& new_unit_str);

    string to_str() const
        {return format("{:32}|{}|{}|{}|{}|{:8}|", name, str(spec), str(value),
            str(lower), str(upper), unit->str);}

    Variable& operator=(const double& val)
        {value = val; return *this;}

    operator double() const
        {return convert_to_base();}
};

ostream& operator<<(ostream& os, const Variable& var);

//---------------------------------------------------------

struct Constraint
{
    Index  ix    {};
    string name  {};
    double value {0.0};

    Constraint(string_view name_ = "") :
        name {name_}
    {}

    Constraint& operator=(const double& val)  {value = val;  return *this;}
    Constraint& operator+=(const double& val) {value += val; return *this;}
    Constraint& operator-=(const double& val) {value -= val; return *this;}
};

//---------------------------------------------------------

struct JacobianElement
{
    Constraint*  con;
    Variable*    var;
    double value {};

    JacobianElement(Constraint*     con_ = nullptr,
                    Variable* const var_ = nullptr) :
        con {con_},
        var {var_}
    {}

    JacobianElement& operator=(const double& val) {value = val; return *this;}
};

//---------------------------------------------------------

struct HessianElement
{
    Constraint*  con;
    Variable*    var1;
    Variable*    var2;
    double value {};

    HessianElement(Constraint* con_  = nullptr,
                   Variable*   var1_ = nullptr,
                   Variable*   var2_ = nullptr) :
        con  {con_},
        var1 {var1_},
        var2 {var2_}
    {}

    HessianElement& operator=(const double& val) {value = val; return *this;}
};

class Block;
class Flowsheet;

using BlockPtr = shared_ptr<Block>;

vector<string> operator+(const vector<string>& c1, const vector<string>& c2);
vector<string>& operator+=(vector<string>& c1, const vector<string>& c2);

//---------------------------------------------------------

struct Stream
{
    string         name;
    Flowsheet*   fs;
    vector<string> comps {};
    BlockPtr       to    {};
    BlockPtr       from  {};

    Stream() = default;
    Stream(string_view            name_,
           Flowsheet*           fs_,
           const vector<string>&  comps_) :
        name  {name_},
        fs    {fs_},
        comps {comps_}
    {}

    bool has_comp(string_view compID) const
    {
        return std::ranges::find(comps, compID) != comps.end();
    }
};

using StreamPtr = shared_ptr<Stream>;

//---------------------------------------------------------

struct StreamVars
{
    Variable*                        total_mass {};
    unordered_map<string, Variable*> mass       {};
    unordered_map<string, Variable*> massfrac   {};
};

//---------------------------------------------------------

class Block
{
public:
    string                               name    {};
    Flowsheet*                         fs      {};
    vector<StreamPtr>                    inlets  {};
    vector<StreamPtr>                    outlets {};
    string                               prefix  {};
    vector<Variable*>                  x       {};
    unordered_map<StreamPtr, StreamVars> x_strm  {};
    vector<Constraint*>                g       {};
    vector<JacobianElement*>           J       {};
    vector<HessianElement*>            H       {};

    Block() = default;
    Block(string_view              name_,
          Flowsheet*             fs_,
          const vector<StreamPtr>& inlets_,
          const vector<StreamPtr>& outlets_);
    virtual ~Block()                = default;
    virtual void initialize()       = 0;
    virtual void eval_constraints() = 0;
    virtual void eval_jacobian()    = 0;
    virtual void eval_hessian()     = 0;

    void show_variables(ostream& os = cout);

private:
    void make_stream_variables(const StreamPtr& strm);
    void make_all_stream_variables();
    void set_inlet_stream_specs();
};

//---------------------------------------------------------

class Flowsheet
{
public:
    string                           name;
    ModelPtr                         m;
    string                           path;
    string                           prefix;
    Flowsheet*                       parent;
    vector<unique_ptr<Flowsheet>>    children;
    vector<BlockPtr>                 blocks;
    unordered_map<string, BlockPtr>  blocks_map;
    unordered_map<string, StreamPtr> streams;

    Flowsheet(string_view   name_,
              ModelPtr      m_,
              Flowsheet*  parent_ = nullptr) :
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

    Flowsheet* add_child(string_view name_);
    StreamPtr    add_stream(const string&          name_,
                            const vector<string>&  comps);

    template<typename T, typename... blk_params_T>
    shared_ptr<T> add_block(string_view              name_,
                            const vector<StreamPtr>& inlet_strms,
                            const vector<StreamPtr>& outlet_strms,
                            blk_params_T&            ...blk_params) {
        auto blk = make_shared<T>(name_, this, inlet_strms, outlet_strms, blk_params...);
        blocks.push_back(blk);
        blocks_map[blk->name] = blk;
        for (const auto& sin : blk->inlets)   sin->to    = blk;
        for (const auto& sout : blk->outlets) sout->from = blk;
        return blk;
    }

private:
    template <typename T>
    void eval(T feval) {
        for (const auto& blk : blocks)   feval(blk);
        for (const auto& fs  : children) feval(fs);
    }

public:
    void initialize()       { eval([](const auto& ptr) { ptr->initialize(); }); }
    void eval_constraints() { eval([](const auto& ptr) { ptr->eval_constraints(); }); }
    void eval_jacobian()    { eval([](const auto& ptr) { ptr->eval_jacobian(); }); }
    void eval_hessian()     { eval([](const auto& ptr) { ptr->eval_hessian(); }); }
};


//---------------------------------------------------------

using IpoptApplicationPtr = IpoptApplication*;

class Model : public TNLP
{
public:
    string                               name;
    unique_ptr<Flowsheet>                index_fs;
    UnitSet                              unit_set;
    vector<unique_ptr<Variable>>         x_vec;
    unordered_map<string, Variable*>   x_map;
    vector<unique_ptr<Constraint>>                g_vec;
    unordered_map<string, Constraint*> g_map;
    vector<unique_ptr<JacobianElement>>           J;
    std::map<std::pair<Index, Index>,
             vector<unique_ptr<HessianElement>>>  H;
    unique_ptr<IpoptApplication>         solver;
    bool                                 printiterate {true};

    Model(string_view name_,
          string_view index_fs_name,
          UnitSet&&   unit_set_) :
        name     {name_},
        unit_set {std::move(unit_set_)}
    {
        index_fs = make_unique<Flowsheet>(index_fs_name, this, nullptr);
        solver = make_unique<IpoptApplication>();
    }

    Variable*        add_variable(string_view name_, Unit* unit);
    Constraint*      add_constraint(string_view name_);
    JacobianElement* add_jacobian_element(Constraint* const con,
                                            Variable* const  var);
    HessianElement*  add_hessian_element(Constraint* const con,
                                           Variable* const   var1,
                                           Variable* const   var2);
    void               initialize()       { index_fs->initialize();       };
    void               eval_constraints() { index_fs->eval_constraints(); };
    void               eval_jacobian()    { index_fs->eval_jacobian();    };
    void               eval_hessian()     { index_fs->eval_hessian();     };
    void               show_variables(ostream& os = cout);
    Variable*        var(const string& name_) const {
        return x_map.contains(name_) ? x_map.at(name_) : nullptr;
    };

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
