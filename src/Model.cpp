#include <algorithm>
#include <cassert>
#include <iostream>
#include "Model.hpp"

Comps operator+(const Comps& c1, const Comps& c2)
{
    Comps u {c1};
    for (auto& c : c2) {
        if (std::find(u.begin(), u.end(), c) == u.end())
            u.push_back(c);
    }
    return u;
}

Comps& operator+=(Comps& c1, const Comps& c2)
{
    for (auto& c : c2) {
        if (std::find(c1.begin(), c1.end(), c) == c1.end())
            c1.push_back(c);
    }
    return c1;
}

//---------------------------------------------------------

FlowsheetPtr Flowsheet::add_child(const std::string& name_)
{
    auto parent = shared_from_this();
    auto fs = std::make_shared<Flowsheet>(name_, parent->m, parent);
    parent->children.push_back(fs);
    return fs;
}

StreamPtr Flowsheet::add_stream(const std::string& name_, Comps& comps)
{
    auto fs = shared_from_this();
    auto strm = make_shared<Stream>(name_, fs, comps);
    fs->streams[name_] = strm;
    return strm;
}

void Flowsheet::initialize() {
    for (auto blk : blocks)
        blk->initialize();
    for (auto fs : children)
        fs->initialize();
}

void Flowsheet::eval_constraints() {
    for (auto blk : blocks)
        blk->eval_constraints();
    for (auto fs : children)
        fs->eval_constraints();
}

void Flowsheet::eval_jacobian() {
    for (auto blk : blocks)
        blk->eval_jacobian();
    for (auto fs : children)
        fs->eval_jacobian();
}

void Flowsheet::eval_hessian() {
    for (auto blk : blocks)
        blk->eval_hessian();
    for (auto fs : children)
        fs->eval_hessian();
}

//---------------------------------------------------------

Block::Block(const std::string&     name_,
             FlowsheetPtr           fs_,
             const std::vector<StreamPtr>& inlets_,
             const std::vector<StreamPtr>& outlets_) :
    name {name_},
    fs {fs_},
    inlets {inlets_},
    outlets {outlets_}
{
    prefix = (fs_->name != "index" ? fs_->name + "." : "") + name_ + ".";
    make_stream_variables();
    set_inlet_stream_specs();
}

void Block::set_inlet_stream_specs() {
    for (const auto& sin : inlets)
        for (const auto& compID : sin->comps)
            x_strm[sin].mass[compID]->fix();
}

void Block::make_stream_variables(const StreamPtr& strm)
{
    auto m = strm->fs->m;
    const std::string s_prefix = strm->fs->prefix + name + "." + strm->name + ".";
    StreamVars strm_vars {};
    strm_vars.total_mass = m->add_variable(s_prefix + "mass", m->unit_set["massflow"]);
    std::string c_prefix = s_prefix + "mass_";
    for (const auto& c : strm->comps)
        strm_vars.mass[c] = m->add_variable(c_prefix + c, m->unit_set["massflow"]);
    c_prefix = s_prefix + "massfrac_";
    for (const auto& c : strm->comps)
        strm_vars.massfrac[c] = m->add_variable(c_prefix + c, m->unit_set["massfrac"]);
    x_strm[strm] = strm_vars;
}

void Block::make_stream_variables() {
    for (const auto& strm : inlets)
        make_stream_variables(strm);
    for (const auto& strm : outlets)
        make_stream_variables(strm);
}

//---------------------------------------------------------

VariablePtr Model::add_variable(const std::string& name_, const Unit& unit)
{
    auto v = std::make_shared<Variable>(name_, unit);
    v->ix = x_vec.size();
    x_vec.push_back(v);
    x_map[name_] = v;
    return v;
}

ConstraintPtr Model::add_constraint(const std::string& name_)
{
    auto con = std::make_shared<Constraint>(name_);
    con->ix = g_vec.size();
    g_vec.push_back(con);
    g_map[name_] = con;
    return con;
}

JacobianElementPtr Model::add_jacobian_element(const ConstraintPtr& con, const VariablePtr& var) {
    auto j = std::make_shared<JacobianElement>(con, var);
    J.push_back(j);
    return j;
}

HessianElementPtr Model::add_hessian_element(const ConstraintPtr& con, const VariablePtr& var1, const VariablePtr& var2) {
    auto h = std::make_shared<HessianElement>(con, var1, var2);
    auto row_col = std::make_pair(std::max(var1->ix, var2->ix), std::min(var1->ix, var2->ix));
    H[row_col].push_back(h);
    return h;
}

//---------------------------------------------------------

bool Model::get_nlp_info(
    Index&          n,
    Index&          m,
    Index&          nnz_jac_g,
    Index&          nnz_h_lag,
    IndexStyleEnum& index_style)
{
    n = x_vec.size();
    m = g_vec.size();
    nnz_jac_g = J.size();
    nnz_h_lag = H.size();
    index_style = TNLP::C_STYLE;

    return true;
}

bool Model::get_bounds_info(
    Index   n,
    Number* x_l,
    Number* x_u,
    Index   m,
    Number* g_l,
    Number* g_u)
{
    for (Index i = 0; const auto var : x_vec) {
        if (var->spec == VariableSpec::Fixed)
            x_l[i] = x_u[i] = *var;
        else {
            x_l[i] = var->lower.has_value() ? var->to_base(var->lower.value()) : -NO_BOUND;
            x_u[i] = var->upper.has_value() ? var->to_base(var->upper.value()) :  NO_BOUND;
        }
        i++;
    }
    
    for (Index i = 0; i < m; i++)
        g_l[i] = g_u[i] = 0.0;

    return true;
}

bool Model::get_starting_point(
    Index   n,
    bool    init_x,
    Number* x_init,
    bool    init_z,
    Number* z_L,
    Number* z_U,
    Index   m,
    bool    init_lambda,
    Number* lambda)
{
    assert(init_z == false);
    assert(init_lambda == false);

    for (Index i = 0; const auto var : x_vec)
        x_init[i++] = *var;

    return true;    
}

bool Model::eval_f(
    Index         n,
    const Number* x_in,
    bool          new_x,
    Number&       obj_value)
{
    obj_value = 1.0;
    return true;
}

bool Model::eval_grad_f(
    Index         n,
    const Number* x_in,
    bool          new_x,
    Number*       grad_f)
{
    for (Index i = 0; i < n; i++)
        grad_f[i] = 0.0;
        
    return true;
}

bool Model::eval_g(
    Index         n,
    const Number* x_in,
    bool          new_x,
    Index         m,
    Number*       g_values)
{
    for (Index i = 0; const auto var : x_vec) {
        var->from_base(x_in[i++]);
    }
    eval_constraints();
    for (Index i = 0; const auto con : g_vec) {
        g_values[i++] = con->value;
    }
    return true;
}

bool Model::eval_jac_g(
    Index         n,
    const Number* x_in,
    bool          new_x,
    Index         m,
    Index         nele_jac,
    Index*        iRow,
    Index*        jCol,
    Number*       values)
{
    if (values == nullptr)
        for (Index i = 0; const auto elem : J) {
            iRow[i] = elem->con->ix;
            jCol[i] = elem->var->ix;
            i++;
        }
    else {
        for (Index i = 0; const auto var : x_vec)
            var->from_base(x_in[i++]);
        eval_jacobian();
        for (Index i = 0; const auto elem : J)
            values[i++] = elem->value;
    }

    return true;
}

bool Model::eval_h(
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
    Number*       values)
{
    if (values == nullptr)
        for (Index i = 0; const auto& val : H) {
            auto [row, col] = val.first;
            iRow[i] = row;
            jCol[i] = col;
            i++;
        }
    else {
        for (Index i = 0; const auto var : x_vec)
            var->from_base(x_in[i++]);
        eval_hessian();
        for (Index i = 0; const auto& [idx, elems] : H) {
            values[i] = 0.0;
            for (const auto& elem : elems) {
                values[i] += lambda[elem->con->ix] * elem->value;
            }
            i++;
        }
    }
        
    return true;
}

void Model::finalize_solution(
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
    IpoptCalculatedQuantities* ip_cq)
{
    for (Index i = 0; const auto var : x_vec)
        var->from_base(x_final[i++]);
}