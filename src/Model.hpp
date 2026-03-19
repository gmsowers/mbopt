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
#include <variant>
#include <algorithm>
#include "IpIpoptApplication.hpp"
#include "IpJournalist.hpp"

constexpr double NO_BOUND = 1.0e20;

using std::string;
using std::string_view;
using std::vector;
using std::unordered_map;
using std::unique_ptr;
using std::make_unique;
using std::format;
using std::ostream;
using std::ofstream;
using std::cout;
using std::cerr;

using Ipopt::TNLP;
using Ipopt::Index;
using Ipopt::Number;
using Ipopt::IpoptData;
using Ipopt::IpoptCalculatedQuantities;
using Ipopt::SolverReturn;
using Ipopt::IpoptApplication;

template<typename T>
concept implements_to_str = requires(const T& obj) {
    obj.to_str();
};

template<implements_to_str T>
ostream& operator<<(ostream& os, const T& obj) {
    return os << obj.to_str();
}

struct UnitKind;

string str(double d);

struct Unit
{
    string    str    {};
    UnitKind* kind   {};
    double    ratio  {1.0};
    double    offset {0.0};

    Unit() = default;
    Unit(string_view str_,
         UnitKind*   kind_,
         double      ratio_  = 1.0,
         double      offset_ = 0.0) :
        str    {str_},
        kind   {kind_},
        ratio  {ratio_},
        offset {offset_}
    {}

    string to_str() const;
};

struct UnitKind
{
    string str              {};
    string base_unit_str    {};
    string default_unit_str {};
    Unit*  base_unit        {};
    Unit*  default_unit     {};

    string to_str() const {
        return format("kind={}, base Unit={}, default Unit={}", str, base_unit_str, default_unit_str);
    }

};

struct UnitSet
{
    std::map<string, unique_ptr<UnitKind>> kinds {};
    std::map<string, unique_ptr<Unit>>     units {};
 
    UnitSet() = default;

    Unit* add_unit(const string& unit_str,
                   UnitKind*     unit_kind,
                   double        unit_ratio  = 1.0,
                   double        unit_offset = 0.0);

    Unit* add_unit(const string& unit_str,
                   const string& unit_kind_str,
                   double        unit_ratio  = 1.0,
                   double        unit_offset = 0.0) {
        return (kinds.contains(unit_kind_str) ? 
                add_unit(unit_str, kinds[unit_kind_str].get(), unit_ratio, unit_offset) :
                nullptr);
    }

    UnitKind* add_kind(const string& unit_kind_str,
                       const string& base_unit_str,
                       const string& default_unit_str = "") {
        auto uk = (kinds[unit_kind_str] = make_unique<UnitKind>(unit_kind_str, base_unit_str,
            default_unit_str.empty() ? base_unit_str : default_unit_str)).get();
        if (!units.contains(base_unit_str))
            add_unit(base_unit_str, unit_kind_str);
        return uk;
    }

    Unit* get_default_unit(const string& unit_kind_str) {
        return (kinds.contains(unit_kind_str) ? kinds[unit_kind_str]->default_unit : nullptr);
    }

    void show_units(ostream& os = cout) const;
};

//---------------------------------------------------------

class Model;

enum class VariableSpec { Fixed, Free };
using Ndouble = std::optional<double>;

string str(Index i);
string str(Ndouble nd);
string str(VariableSpec spec);

class Quantity {
public:
    virtual ~Quantity() = default;

    string name  {};
    double value {0.0};
    Unit*  unit  {};

    Quantity() = default;
    Quantity(string_view name_,
             Unit*       unit_) :
        name {name_},
        unit {unit_}
    {}
    Quantity(string_view name_,
             double      value_,
             Unit*       unit_) :
        name  {name_},
        value {value_},
        unit  {unit_}
    {}
    Quantity(double      value_,
             Unit*       unit_) :
        value {value_},
        unit  {unit_}
    {}

    virtual double convert_to_base() const
        {return value * unit->ratio + unit->offset;}
    virtual double convert_to_base(double value_) const
        {return value_ * unit->ratio + unit->offset;}
    virtual double convert_to_base(double value_, const Unit* u) const
        {return value_ * u->ratio + u->offset;}
    virtual double convert_from_base(double base_value) const
        {return (base_value - unit->offset) / unit->ratio;}
    virtual double convert_from_base(double base_value, const Unit* u) const
        {return (base_value - u->offset) / u->ratio;}
    virtual void   convert_and_set(double base_value)
        {value = (base_value - unit->offset) / unit->ratio;}
    virtual void   convert_and_set(double value_, const Unit* u)
        {value = convert_from_base(convert_to_base(value_, u));}
    virtual double convert(double value_, const Unit* u) const
        {return (u == unit ? value_ : convert_from_base(convert_to_base(value_, u)));}
    virtual double convert(const Unit* u) const
        {return (u == unit ? value : convert_from_base(convert_to_base(), u));}
    virtual void change_unit(Unit* new_unit);

    virtual operator double() const
        {return convert_to_base();}

    string to_str_2() const {
        return (str(value) + "_" + unit->str);
    }
    
     string to_str() const {
         return format("│{:32}│{}│{:8}│", name, str(value), unit->str);
     }

};

class Variable : public Quantity
{
public:
    Index        ix    {};
    Ndouble      lower {};
    Ndouble      upper {};
    VariableSpec spec  {VariableSpec::Free};

    Variable() = default;
    Variable(string_view name_,
             Unit*       unit_) :
        Quantity {name_, unit_}
    {}

    void fix()  {spec = VariableSpec::Fixed;}
    void free() {spec = VariableSpec::Free;}

    bool is_fixed() const {return (spec == VariableSpec::Fixed);}
    bool is_free()  const {return (spec == VariableSpec::Free);}

    string to_str() const {
        return format("│{}│{:32}│{}│{}│{}│{}│{:8}│", str(ix), name, str(spec), str(value),
            str(lower), str(upper), unit->str);
    }

};

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

    string to_str() const {
        return format("│{}│{:32}│{}│", str(ix), name, str(value));
    }

};

//---------------------------------------------------------

struct JacobianNZ
{
    Index        ix    {};
    Constraint*  con   {};
    Variable*    var   {};
    double       value {};

    JacobianNZ(Constraint*     con_ = nullptr,
               Variable* const var_ = nullptr) :
        con {con_},
        var {var_}
    {}

    string to_str() const {
        return format("│{}│{}│{}│{:32}│{:32}│{}│", str(ix), str(con->ix), str(var->ix),
            con->name, var->name, str(value));
    }

    JacobianNZ& operator=(const double& val) {value = val; return *this;}
};

//---------------------------------------------------------

struct HessianNZ
{
    Index        ix    {};
    Constraint*  con   {};
    Variable*    var1  {};
    Variable*    var2  {};
    double       value {};

    HessianNZ(Constraint* con_  = nullptr,
              Variable*   var1_ = nullptr,
              Variable*   var2_ = nullptr) :
        con  {con_},
        var1 {var1_},
        var2 {var2_} {}

    string to_str() const {
        return format("│{}│{}│{}│{}│{:32}│{:32}│{:32}│{}│", str(ix), str(con->ix), str(var1->ix),
            str(var2->ix), con->name, var1->name, var2->name, str(value));
    }

    HessianNZ& operator=(const double& val) {value = val; return *this;}
};

class Block;
class Flowsheet;

vector<string>  operator+(const vector<string>& c1, const vector<string>& c2);
vector<string>& operator+=(vector<string>& c1, const vector<string>& c2);

//---------------------------------------------------------

struct Connection;

struct Stream
{
    string         name;
    Flowsheet*     fs    {};
    vector<string> comps {};
    Block*         to    {};
    Block*         from  {};

    Stream() = default;
    Stream(string_view      name_,
           Flowsheet*       fs_,
           vector<string>&& comps_) noexcept :
        name  {name_},
        fs    {fs_},
        comps {std::move(comps_)}
    {}

    Connection* connect();
    bool has_comp(string_view c) const {
        return std::ranges::find(comps, c) != comps.end();
    }

    string to_str() const;
};

//---------------------------------------------------------

struct StreamVars
{
    Variable*                        total_mass {};
    unordered_map<string, Variable*> mass       {};
    unordered_map<string, Variable*> massfrac   {};
};

//---------------------------------------------------------

enum class BlockType {Mixer, Splitter, Separator, YieldReactor, MultiYieldReactor, StoicReactor};

class Block
{
public:
    string                             name     {};
    Flowsheet*                         fs       {};
    BlockType                          blk_type {};
    vector<Stream*>                    inlets   {};
    vector<Stream*>                    outlets  {};
    string                             prefix   {};
    vector<Variable*>                  x        {};
    unordered_map<Stream*, StreamVars> x_strm   {};
    vector<Constraint*>                g        {};
    vector<JacobianNZ*>                J        {};
    vector<HessianNZ*>                 H        {};

    Block() = default;
    Block(string_view       name_,
          Flowsheet*        fs_,
          BlockType         blk_type_,
          vector<Stream*>&& inlets_,
          vector<Stream*>&& outlets_) noexcept;
    virtual ~Block()                = default;
    virtual void initialize()       = 0;
    virtual void eval_constraints() = 0;
    virtual void eval_jacobian()    = 0;
    virtual void eval_hessian()     = 0;

    void show_variables(ostream& os = cout)   const;
    void show_constraints(ostream& os = cout) const;
    void show_jacobian(ostream& os = cout)    const;
    void show_hessian(ostream& os = cout)     const;
    void show_model(ostream& os = cout)       const {}

    string to_str() const;

private:
    void make_stream_variables(Stream* strm);
    void make_all_stream_variables();
    void set_inlet_stream_specs();
};

//---------------------------------------------------------

void call_lua_function(const string& func_name);

class Calc
{
public:
    string              name   {};
    Flowsheet*          fs     {};
    string              prefix {};
    vector<Variable*>   x      {};
    vector<Constraint*> g      {};
    vector<JacobianNZ*> J      {};
    vector<HessianNZ*>  H      {};

    Calc() = default;
    Calc(string_view name_,
         Flowsheet*  fs_);

    void show_variables(ostream& os = cout)   const;
    void show_constraints(ostream& os = cout) const;
    void show_jacobian(ostream& os = cout)    const;
    void show_hessian(ostream& os = cout)     const;
    void show_model(ostream& os = cout)       const {}

    void show_calc(ostream& os = cout) const;

private:
    string make_name(const string& suffix) const {
        string s = prefix + suffix;
        std::replace(s.begin(), s.end(), '.', '_');
        return s;
    }

public:
    void initialize() const {
        call_lua_function(make_name("initialize"));
    }
    void eval_constraints() const {
        call_lua_function(make_name("eval_constraints"));
    }
    void eval_jacobian() const {
        call_lua_function(make_name("eval_jacobian"));
    }
    void eval_hessian() const {
        if (H.empty()) return;
        call_lua_function(make_name("eval_hessian"));
    }


};

//---------------------------------------------------------

struct Connection
{
    Constraint* eq   {};
    Variable*   var1 {};
    Variable*   var2 {};   
    JacobianNZ* jnz1 {};
    JacobianNZ* jnz2 {};

    string to_str() const {
        return format("│{}│{}│{}│{:32}│{:32}│{}│", str(eq->ix), str(var1->ix),
            str(var2->ix), var1->name, var2->name, str(eq->value));
    }

};

struct Connections
{
    const string                   prefix   {"cnx."};
    vector<unique_ptr<Connection>> conn_vec {};

    void eval_constraints() const {
        for (const auto& conn_p : conn_vec)
            *conn_p->eq = *conn_p->var1 - *conn_p->var2;
    }

    void eval_jacobian() const {
        for (const auto& conn_p : conn_vec) {
            *conn_p->jnz1 = 1.0;
            *conn_p->jnz2 = -1.0;
        }
    }

};

//---------------------------------------------------------

class Flowsheet
{
public:
    string                                    name;
    Model*                                    m;
    string                                    path;
    string                                    prefix;
    Flowsheet*                                parent;
    vector<unique_ptr<Flowsheet>>             children;
    unordered_map<string, Flowsheet*>         child_map;
    vector<unique_ptr<Block>>                 blocks;
    unordered_map<string, Block*>             blocks_map;
    vector<unique_ptr<Calc>>                  calcs;
    unordered_map<string, Calc*>              calcs_map;
    unordered_map<string, unique_ptr<Stream>> streams;

    Flowsheet(string_view name_,
              Model*      m_,
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

    Flowsheet* add_flowsheet(string_view name_);
    Stream*    add_stream(const string&  name_,
                          vector<string> comps) noexcept;

    template<typename T, typename... blk_params_T>
    T* add_block(string_view       name_,
                 vector<Stream*>&& inlet_strms,
                 vector<Stream*>&& outlet_strms,
                 blk_params_T&     ...blk_params) noexcept {
             
        auto blk = make_unique<T>(name_,
                                  this,
                                  std::move(inlet_strms),
                                  std::move(outlet_strms),
                                  blk_params...);
        auto blk_p = blk.get();
        blocks_map[blk->name] = blk_p;
        for (const auto& sin : blk->inlets)   sin->to    = blk_p;
        for (const auto& sout : blk->outlets) sout->from = blk_p;
        blocks.push_back(std::move(blk));
        return blk_p;
    }

    Calc* add_calc(string_view name_) {
        auto calc = make_unique<Calc>(name_, this);
        auto calc_p = calc.get();
        calcs_map[calc->name] = calc_p;
        calcs.push_back(std::move(calc));
        return calc_p;
    }

    bool connect_streams() {
        for (const auto& [name_, strm] : streams)
            if (strm->to != nullptr && strm->from != nullptr) {
                if (!strm->connect())
                    return false;
            }
        for (const auto& fs : children) {
            if (!fs->connect_streams())
                return false;
        }

        return true;
    }

    void show_flowsheet(ostream& os = cout) const;

private:
    void eval(auto feval) {
        for (const auto& blk : blocks)   feval(blk);
        for (const auto& clc : calcs)    feval(clc);
        for (const auto& fs  : children) feval(fs);
    }

public:
    void initialize()       { eval([](const auto& ptr) { ptr->initialize();       }); }
    void eval_constraints() { eval([](const auto& ptr) { ptr->eval_constraints(); }); }
    void eval_jacobian()    { eval([](const auto& ptr) { ptr->eval_jacobian();    }); }
    void eval_hessian()     { eval([](const auto& ptr) { ptr->eval_hessian();     }); }

    void show_model(ostream& os = cout) const {}

};

//---------------------------------------------------------

using Price = Quantity;

// class Price : public Quantity
// {
// public:
//     Price() = default;
//     Price(string_view name_,
//           double      value_,
//           Unit*       unit_) :
//         Quantity {name_, value_, unit_}
//     {}

//     string to_str() const {
//         return format("│{:32}│{}│{:8}│", name, str(value), unit->str);
//     }

// };

class ObjTerm : public Quantity
{
public:
    Variable* var;
    Price*    price;
    double    scale {1.0};

    ObjTerm() = default;
    ObjTerm(string_view name_,
            Variable*   var_,
            Price*      price_,
            Unit*       unit_,
            double      scale_ = 1.0) :
        Quantity {name_, unit_},
        var      {var_},
        price    {price_},
        scale    {scale_}
    {}

    double eval() {
        value = scale * convert_from_base(*var * *price);
        return value;
    }

    double eval_grad() const {
        return scale * convert_from_base(*price);
    }

    string to_str() const {
        return format("│{:24}│{:32}│{:24}│{}│{:8}│",
            name, var->name, price->name, str(value), unit->str);
    }

};

class Objective : public Quantity 
{
public:
    Model*                                          m     {};
    double                                          scale {1.0};
    unordered_map<string,
                  std::variant<unique_ptr<ObjTerm>,
                               Objective*>>         terms {};
    vector<std::pair<Index, double>>                grad  {};
    
    Objective() = default;
    Objective(string_view name_,
              Model*      m_,
              Unit*       unit_,
              double      scale_ = 1.0) :
        Quantity {name_, unit_},
        m        {m_},
        scale    {scale_}
    {}

    Objective* add_objective(Objective* obj) {
        terms[obj->name] = obj;
        return obj;
    }
    
    ObjTerm* add_objterm(string_view name_,
                         Variable*   var_, 
                         Price*      price_,
                         Unit*       unit_,
                         double      scale_ = 1.0);

    double eval();
    void   eval_grad();

    string to_str() const {
        return format("│{:>24}│{:32}│{:24}│{}│{:8}│",
            name, "", "", str(value), unit->str);
    }

private:
    void eval_grad_rec(vector<std::pair<Index, double>>& grad_top);

};

//---------------------------------------------------------

class Model : public TNLP
{
public:
    string                             name;
    unique_ptr<Flowsheet>              index_fs;
    UnitSet                            unit_set;
    Connections                        cnx {};
    vector<unique_ptr<Variable>>       x_vec;
    unordered_map<string, Variable*>   x_map;
    vector<unique_ptr<Constraint>>     g_vec;
    unordered_map<string, Constraint*> g_map;
    vector<unique_ptr<JacobianNZ>>     J;
    vector<unique_ptr<HessianNZ>>      H_vec;
    std::map<std::pair<Index, Index>,
             vector<HessianNZ*>>       H;
    unordered_map<string,
        unique_ptr<Price>>             prices;
    unordered_map<string,
        unique_ptr<Objective>>         objectives;
    Objective*                         obj {nullptr};
    bool                               printiterate {true};

    Model() = default;
    
    Model(string_view name_,
          string_view index_fs_name,
          UnitSet&&   unit_set_) noexcept :
        name     {name_},
        unit_set {std::move(unit_set_)}
    {
        index_fs = make_unique<Flowsheet>(index_fs_name, this, nullptr);
    }

    Variable*   add_var(string_view name_, Unit* unit);
    Constraint* add_constraint(string_view name_);
    JacobianNZ* add_J_NZ(Constraint* con,
                         Variable*   var);
    HessianNZ*  add_H_NZ(Constraint* con,
                         Variable*   var1,
                         Variable*   var2);
    Connection* add_connection(Variable* var1,
                               Variable* var2);
    bool        add_bridge(Stream* sfrom,
                           Stream* sto);
    Price*      add_price(string_view name_,
                          double      value_,
                          Unit*       unit_);
    Objective*  add_objective(string_view name_,
                              Unit*       unit_,
                              double      scale_ = 1.0);
    void        initialize()       { index_fs->initialize();      };
    void        eval_constraints() { index_fs->eval_constraints();
                                     cnx.eval_constraints();      };
    void        eval_jacobian()    { index_fs->eval_jacobian();
                                     cnx.eval_jacobian();         };
    void        eval_hessian()     { index_fs->eval_hessian();    };
    double      eval_objective()   { return (obj == nullptr ?
                                         1.0 : obj->eval());      }; 
    void        eval_obj_grad()    { if (obj) obj->eval_grad();   }; 
    
    void        show_variables(ostream& os = cout)   const;
    void        show_constraints(ostream& os = cout) const;
    void        show_jacobian(ostream& os = cout)    const;
    void        show_hessian(ostream& os = cout)     const;
    void        show_connections(ostream& os = cout) const;
    void        show_model(ostream& os = cout)       const;
    void        show_prices(ostream& os = cout)      const;
    void        show_objective(Objective* obj_ = nullptr,
                               ostream&   os = cout) const;
    void        show_obj_grad(ostream& os = cout)    const;
    void        show_units(ostream& os = cout)       const {
        unit_set.show_units(os);
    }
    void        write_variables(ostream& os = cout)  const;

    Variable*   var(const string& name_) const {
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

private:
    void show_objective_rec(Objective* obj_, ostream& os) const;

};
