#include <algorithm>
#include <cassert>
#include "Model.hpp"

vector<string> operator+(const vector<string>& c1, const vector<string>& c2)
{
    vector<string> u {c1};
    for (auto& c : c2) {
        if (std::ranges::find(u, c) == u.end())
            u.push_back(c);
    }
    return u;
}

vector<string>& operator+=(vector<string>& c1, const vector<string>& c2)
{
    for (auto& c : c2) {
        if (std::ranges::find(c1, c) == c1.end())
            c1.push_back(c);
    }
    return c1;
}

//---------------------------------------------------------

shared_ptr<Unit> UnitSet::add_unit(const string& unit_str,
                          shared_ptr<UnitKind>   unit_kind,
                          double        unit_ratio,
                          double        unit_offset) {
    auto unit_ptr = make_shared<Unit>(unit_str, unit_kind, unit_ratio, unit_offset);
    units[unit_str] = unit_ptr;
    if (unit_str == unit_kind->base_unit_str)
        unit_kind->base_unit = unit_ptr;
    if (unit_str == unit_kind->default_unit_str)
        unit_kind->default_unit = unit_ptr;
    return unit_ptr;
}

shared_ptr<UnitKind> UnitSet::add_kind(const string& unit_kind_str,
                              const string& base_unit_str,
                              const string& default_unit_str) {
    auto unit_kind_ptr = make_shared<UnitKind>(unit_kind_str, base_unit_str,
         default_unit_str.empty() ? base_unit_str : default_unit_str);
    kinds[unit_kind_str] = unit_kind_ptr;
    return unit_kind_ptr;
}

//---------------------------------------------------------

Ndouble Variable::change_unit(ModelPtr m, const string& new_unit_str) {
    if (new_unit_str == unit->str) return std::nullopt;
    if (!m->unit_set.units.contains(new_unit_str)) {
        cerr << "Error in ChangeUnit(\"" << name << "\", \"" << new_unit_str << "\"). The new unit \"" << new_unit_str << "\" is not in the unit set.\n";
        return std::nullopt;
    }
    auto new_unit = m->unit_set.units[new_unit_str];
    if (new_unit->kind != unit->kind) {
        cerr << "Error in ChangeUnit(\"" << name << "\", \"" << new_unit_str << "\"). The new unit \"" << new_unit_str << "\" is the wrong kind.\n";
        cerr << "      \"" << name << "\" has kind \"" << unit->kind->str << "\", but \"" << new_unit_str << "\" has kind \"" << new_unit->kind->str << ".\"\n";
        return std::nullopt;
    }
    auto old_unit = unit;
    unit = new_unit;
    convert_and_set(value, old_unit);
    return value;
}

//---------------------------------------------------------

FlowsheetPtr Flowsheet::add_child(string_view name_) {
    auto parent = shared_from_this();
    auto fs = make_shared<Flowsheet>(name_, parent->m, parent);
    parent->children.push_back(fs);
    return fs;
}

StreamPtr Flowsheet::add_stream(const string& name_, const vector<string>& comps) {
    auto fs = shared_from_this();
    auto strm = make_shared<Stream>(name_, fs, comps);
    fs->streams[name_] = strm;
    return strm;
}

char const* var_header = R"(
              Name               Fix      Value          Lower          Upper      Units
--------------------------------|---|--------------|--------------|--------------|--------|
)";

//---------------------------------------------------------

Block::Block(string_view              name_,
             FlowsheetPtr             fs_,
             const vector<StreamPtr>& inlets_,
             const vector<StreamPtr>& outlets_) :
    name    {name_},
    fs      {fs_},
    inlets  {inlets_},
    outlets {outlets_}
{
    prefix = (fs->name != "index" ? fs->name + "." : "") + name + ".";
    make_all_stream_variables();
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
    const string s_prefix = strm->fs->prefix + name + "." + strm->name + ".";

    StreamVars strm_vars {};

    auto u_massflow = m->unit_set.get_default_unit("massflow");
    auto u_massfrac = m->unit_set.get_default_unit("massfrac");

    x.push_back(strm_vars.total_mass = m->add_variable(s_prefix + "mass", u_massflow));

    string c_prefix = s_prefix + "mass_";
    for (const auto& c : strm->comps)
        x.push_back(strm_vars.mass[c] = m->add_variable(c_prefix + c, u_massflow));

    c_prefix = s_prefix + "massfrac_";
    for (const auto& c : strm->comps)
        x.push_back(strm_vars.massfrac[c] = m->add_variable(c_prefix + c, u_massfrac));

    x_strm[strm] = strm_vars;
}

void Block::make_all_stream_variables() {
    for (const auto& strm : inlets)  make_stream_variables(strm);
    for (const auto& strm : outlets) make_stream_variables(strm);
}

void Block::show_variables(ostream& os) {
    os << var_header;
    for (const auto var : x)
        os << *var << '\n';
}

//---------------------------------------------------------

Variable* Model::add_variable(string_view name_, const shared_ptr<Unit>& unit)
{
    x_vec.push_back(make_unique<Variable>(name_, unit));
    auto v = x_vec.back().get();
    v->ix = x_vec.size() - 1;
    x_map[v->name] = v;
    return v;
}

Constraint* Model::add_constraint(string_view name_)
{
    g_vec.push_back(make_unique<Constraint>(name_));
    auto con = g_vec.back().get();
    con->ix = g_vec.size() - 1;
    g_map[con->name] = con;
    return con;
}

JacobianElement* Model::add_jacobian_element(Constraint* const con, Variable* const var) {
    J.push_back(make_unique<JacobianElement>(con, var));
    return J.back().get();
}

HessianElement* Model::add_hessian_element(Constraint* const con,
                                             Variable* const   var1,
                                             Variable* const   var2) {
    auto row_col = std::make_pair(std::max(var1->ix, var2->ix), std::min(var1->ix, var2->ix));
    H[row_col].push_back(make_unique<HessianElement>(con, var1, var2));
    return H[row_col].back().get();
}

//---------------------------------------------------------

void Model::show_variables(ostream& os) {
    os << var_header;
    for (const auto& var : x_vec)
        os << *var << '\n';
}

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
    for (Index i = 0; const auto& var : x_vec) {
        if (var->spec == VariableSpec::Fixed)
            x_l[i] = x_u[i] = *var;
        else {
            x_l[i] = var->lower.has_value() ? var->convert_to_base(var->lower.value()) : -NO_BOUND;
            x_u[i] = var->upper.has_value() ? var->convert_to_base(var->upper.value()) :  NO_BOUND;
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

    for (Index i = 0; const auto& var : x_vec)
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
    for (Index i = 0; const auto& var : x_vec) {
        var->convert_and_set(x_in[i++]);
    }
    eval_constraints();
    for (Index i = 0; const auto& con : g_vec) {
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
        for (Index i = 0; const auto& elem : J) {
            iRow[i] = elem->con->ix;
            jCol[i] = elem->var->ix;
            i++;
        }
    else {
        for (Index i = 0; const auto& var : x_vec)
            var->convert_and_set(x_in[i++]);
        eval_jacobian();
        for (Index i = 0; const auto& elem : J)
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
        for (Index i = 0; const auto& var : x_vec)
            var->convert_and_set(x_in[i++]);
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
    for (Index i = 0; const auto& var : x_vec)
        var->convert_and_set(x_final[i++]);
}

//---------------------------------------------------------

string str(double d) {
    return format("{:14.7g}", d);
}

string str(Ndouble nd) {
    return nd.has_value() ? format("{:14.7g}", nd.value()) : format("{:14}", "");
}

string str(VariableSpec spec) {
    return spec == VariableSpec::Fixed ? " F " : "   ";
}

ostream& operator<<(ostream& os, const Variable& var) {
    return os << var.to_str();
}
