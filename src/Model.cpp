#include <algorithm>
#include <cassert>
#include <ranges>
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

char const* units_header = R"(
┌────────────┬────────────┬──────────────┬──────────────┬──────┬─────────┐
│    Unit    │    Kind    │     Ratio    │    Offset    │ Base │ Default │
├────────────┼────────────┼──────────────┼──────────────┼──────┼─────────┤
)";
char const* units_footer =
R"(└────────────┴────────────┴──────────────┴──────────────┴──────┴─────────┘
)";

string Unit::to_str() const {
    return format("│{:12}│{:12}│{}│{}│{}│{}│", str, kind->str, ::str(ratio), ::str(offset),
            (kind->base_unit == this ? "   ✓  " : "      "),
            (kind->default_unit == this ? "    ✓    " : "         "));
}

//---------------------------------------------------------

Unit* UnitSet::add_unit(const string& unit_str,
                        UnitKind*     unit_kind,
                        double        unit_ratio,
                        double        unit_offset) {
    auto unit = make_unique<Unit>(unit_str, unit_kind, unit_ratio, unit_offset);
    auto unit_p = unit.get();
    if (unit_str == unit_kind->base_unit_str)
        unit_kind->base_unit = unit_p;
    if (unit_str == unit_kind->default_unit_str)
        unit_kind->default_unit = unit_p;
    units[unit_str] = std::move(unit);
    return unit_p;
}

void UnitSet::show_units(ostream& os) const {
    int count {0};
    std::map<string, vector<Unit*>> units_by_kind;
    for (const auto& u : std::views::values(units))
        units_by_kind[u->kind->str].push_back(u.get());

    os << units_header;
    for (const auto& uvec : std::views::values((units_by_kind)))
        for (const auto& u : uvec) {
            os << *u << '\n';
            count++;
        }
    os << units_footer;
    os << count << " Unit" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

//---------------------------------------------------------

void Quantity::change_unit(Unit* new_unit) {
    if (new_unit->kind == unit->kind) {
        auto old_unit = unit;
        unit = new_unit;
        convert_and_set(value, old_unit);
    }
}

//---------------------------------------------------------

Connection* Stream::connect() {
    if (!to || !from) return nullptr;
    auto M = fs->m;
    auto to_sv = to->x_strm[this];
    auto from_sv = from->x_strm[this];
    Connection* conn_p {};
    for (const auto& c : comps) {
        conn_p = M->add_connection(to_sv.mass[c], from_sv.mass[c]);
        if (!conn_p) return nullptr;
        to_sv.mass[c]->convert_and_set(*from_sv.mass[c]);
    }
    return conn_p;
}

//---------------------------------------------------------

Flowsheet* Flowsheet::add_flowsheet(string_view name_) {
    auto fs = make_unique<Flowsheet>(name_, this->m, this);
    auto fs_p = fs.get();
    children.push_back(std::move(fs));
    child_map[fs_p->name] = fs_p;
    return fs_p;
}

Stream* Flowsheet::add_stream(const string&  name_,
                              vector<string> comps) noexcept {
    auto strm = make_unique<Stream>(name_, this, std::move(comps));
    auto strm_p = strm.get();
    this->streams[name_] = std::move(strm);
    return strm_p;
}

void Flowsheet::show_flowsheet(ostream& os) const {
    os << "Flowsheet: " << name << '\n';
    for (const auto& blk : blocks) {
        os << format("  {:12} in= ", blk->name);
        for (const auto& sin : blk->inlets)
            os << sin->name << ' ';
        os << '\n' << format("              out= ");
        for (const auto& sout : blk->outlets)
            os << sout->name;
        os << '\n';
    }
    for (const auto& clc : calcs)
        os << format("  {:12}", clc->name);
    os << '\n' << std::flush;
}

//---------------------------------------------------------

char const* var_header = R"(
┌─────┬────────────────────────────────┬───┬──────────────┬──────────────┬──────────────┬────────┐
│Index│              Name              │Fix│     Value    │     Lower    │     Upper    │  Unit  │
├─────┼────────────────────────────────┼───┼──────────────┼──────────────┼──────────────┼────────┤
)";
char const* var_footer =
R"(└─────┴────────────────────────────────┴───┴──────────────┴──────────────┴──────────────┴────────┘
)";

char const* con_header = R"(
┌─────┬────────────────────────────────┬──────────────┐
│Index│              Name              │     Value    │
├─────┼────────────────────────────────┼──────────────┤
)";
char const* con_footer =
R"(└─────┴────────────────────────────────┴──────────────┘
)";

char const* jac_header = R"(
┌─────┬─────┬─────┬────────────────────────────────┬────────────────────────────────┬──────────────┐
│Index│  Eq │ Var │             Equation           │            Variable            │    Value     │
├─────┼─────┼─────┼────────────────────────────────┼────────────────────────────────┼──────────────┤
)";
char const* jac_footer =
R"(└─────┴─────┴─────┴────────────────────────────────┴────────────────────────────────┴──────────────┘
)";

char const* hess_header = R"(
┌─────┬─────┬─────┬─────┬────────────────────────────────┬────────────────────────────────┬────────────────────────────────┬──────────────┐
│Index│  Eq │ Var1│ Var2│             Equation           │            Variable 1          │            Variable 2          │    Value     │
├─────┼─────┼─────┼─────┼────────────────────────────────┼────────────────────────────────┼────────────────────────────────┼──────────────┤
)";
char const* hess_footer =
R"(└─────┴─────┴─────┴─────┴────────────────────────────────┴────────────────────────────────┴────────────────────────────────┴──────────────┘
)";

char const* conn_header = R"(
┌─────┬─────┬─────┬────────────────────────────────┬────────────────────────────────┬──────────────┐
│Index│ Var1│ Var2│            Variable 1          │            Variable 2          │    Value     │
├─────┼─────┼─────┼────────────────────────────────┼────────────────────────────────┼──────────────┤
)";
char const* conn_footer =
R"(└─────┴─────┴─────┴────────────────────────────────┴────────────────────────────────┴──────────────┘
)";

char const* price_header = R"(
┌────────────────────────────────┬──────────────┬────────┐
│              Name              │     Value    │  Unit  │
├────────────────────────────────┼──────────────┼────────┤
)";
char const* price_footer =
R"(└────────────────────────────────┴──────────────┴────────┘
)";

char const* obj_header = R"(
┌────────────────────────┬────────────────────────────────┬────────────────────────┬──────────────┬────────┐
│          Term          │            Variable            │          Price         │    Value     │  Unit  │
├────────────────────────┼────────────────────────────────┼────────────────────────┼──────────────┼────────┤
)";
char const* obj_footer =
R"(└────────────────────────┴────────────────────────────────┴────────────────────────┴──────────────┴────────┘
)";

char const* obj_grad_header = R"(
┌─────┬────────────────────────────────┬──────────────┐
│Index│            Variable            │    Value     │
├─────┼────────────────────────────────┼──────────────┤
)";
char const* obj_grad_footer =
R"(└─────┴────────────────────────────────┴──────────────┘
)";

//---------------------------------------------------------

Block::Block(string_view       name_,
             Flowsheet*        fs_,
             vector<Stream*>&& inlets_,
             vector<Stream*>&& outlets_) noexcept :
    name    {name_},
    fs      {fs_},
    inlets  {std::move(inlets_)},
    outlets {std::move(outlets_)}
{
    prefix = (fs->name != "index" ? fs->name + "." : "") + name + ".";
    make_all_stream_variables();
    set_inlet_stream_specs();
}

void Block::set_inlet_stream_specs() {
    for (const auto& sin : inlets)
        for (const auto& c : sin->comps)
            x_strm[sin].mass[c]->fix();
}

void Block::make_stream_variables(Stream* strm)
{
    auto m = strm->fs->m;
    const string s_prefix = strm->fs->prefix + name + "." + strm->name + ".";

    StreamVars strm_vars {};

    auto u_massflow = m->unit_set.get_default_unit("massflow");
    auto u_massfrac = m->unit_set.get_default_unit("massfrac");

    x.push_back(strm_vars.total_mass = m->add_var(s_prefix + "mass", u_massflow));

    string c_prefix = s_prefix + "mass_";
    for (const auto& c : strm->comps)
        x.push_back(strm_vars.mass[c] = m->add_var(c_prefix + c, u_massflow));

    c_prefix = s_prefix + "massfrac_";
    for (const auto& c : strm->comps)
        x.push_back(strm_vars.massfrac[c] = m->add_var(c_prefix + c, u_massfrac));

    x_strm[strm] = std::move(strm_vars);
}

void Block::make_all_stream_variables() {
    for (const auto& strm : inlets)  make_stream_variables(strm);
    for (const auto& strm : outlets) make_stream_variables(strm);
}

void Block::show_variables(ostream& os) const {
    int count {0};
    os << var_header;
    for (const auto var : x) {
        os << *var << '\n';
        count++;
    }
    os << var_footer;
    os << count << " Variable" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

void Block::show_constraints(ostream& os) const {
    int count {0};
    os << con_header;
    for (const auto con : g) {
        os << *con << '\n';
        count++;
    }
    os << con_footer;
    os << count << " Constraint" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

void Block::show_jacobian(ostream& os) const {
    int count {0};
    os << jac_header;
    for (const auto jnz : J) {
        os << *jnz << '\n';
        count++;
    }
    os << jac_footer;
    os << count << " Jacobian NZ" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

void Block::show_hessian(ostream& os) const {
    int count {0};
    os << hess_header;
    for (const auto hnz : H) {
        os << *hnz << '\n';
        count++;
    }
    os << hess_footer;
    os << count << " Hessian NZ" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

//---------------------------------------------------------

Calc::Calc(string_view name_,
           Flowsheet*  fs_) :
    name {name_},
    fs   {fs_}
{
    prefix = (fs->name != "index" ? fs->name + "." : "") + name + ".";
}

void Calc::show_variables(ostream& os) const {
    int count {0};
    os << var_header;
    for (const auto var : x) {
        os << *var << '\n';
        count++;
    }
    os << var_footer;
    os << count << " Variable" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

void Calc::show_constraints(ostream& os) const {
    int count {0};
    os << con_header;
    for (const auto& con : g) {
        os << *con << '\n';
        count++;
    }
    os << con_footer;
    os << count << " Constraint" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

void Calc::show_jacobian(ostream& os) const {
    int count {0};
    os << jac_header;
    for (const auto& jnz : J) {
        os << *jnz << '\n';
        count++;
    }
    os << jac_footer;
    os << count << " Jacobian NZ" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

void Calc::show_hessian(ostream& os) const {
    int count {0};
    os << hess_header;
    for (const auto& hnz : H) {
        os << *hnz << '\n';
        count++;
    }
    os << hess_footer;
    os << count << " Hessian NZ" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

//---------------------------------------------------------

ObjTerm* Objective::add_objterm(string_view name_,
                                Variable*   var_,
                                Price*      price_,
                                Unit*       unit_,
                                double      scale_) {
    auto objterm = make_unique<ObjTerm>(name_, var_, price_, unit_, scale_);
    auto objterm_p = objterm.get();
    terms[string(name_)] = std::move(objterm);
    return objterm_p;
}

double Objective::eval() {
    value = 0.0;
    for (const auto& term : std::views::values(terms)) {
        if (std::holds_alternative<unique_ptr<ObjTerm>>(term)) {
            auto objterm = std::get<unique_ptr<ObjTerm>>(term).get();
            value += convert_to_base(objterm->eval(), objterm->unit);
        }
        else {
            auto obj = std::get<Objective*>(term);
            value += convert_to_base(obj->eval(), obj->unit);
        }
    }
    value = scale * convert_from_base(value);
    return value;
}

void Objective::eval_grad_rec(vector<std::pair<Index, double>>& grad_top) {
    for (const auto& term : std::views::values(terms)) {
        if (std::holds_alternative<unique_ptr<ObjTerm>>(term)) {
            auto objterm = std::get<unique_ptr<ObjTerm>>(term).get();
            grad_top.push_back({objterm->var->ix, scale * convert_to_base(objterm->eval_grad(), objterm->unit)});
        }
        else
            std::get<Objective*>(term)->eval_grad_rec(grad_top);
    }
}

void Objective::eval_grad() {
    grad.clear();
    eval_grad_rec(grad);
}
    
//---------------------------------------------------------

Variable* Model::add_var(string_view name_, Unit* unit)
{
    x_vec.push_back(make_unique<Variable>(name_, unit));
    auto v_p = x_vec.back().get();
    v_p->ix = x_vec.size() - 1;
    x_map[v_p->name] = v_p;
    return v_p;
}

Constraint* Model::add_constraint(string_view name_)
{
    g_vec.push_back(make_unique<Constraint>(name_));
    auto con_p = g_vec.back().get();
    con_p->ix = g_vec.size() - 1;
    g_map[con_p->name] = con_p;
    return con_p;
}

JacobianNZ* Model::add_J_NZ(Constraint* con,
                            Variable*   var) {
    J.push_back(make_unique<JacobianNZ>(con, var));
    auto jnz_p = J.back().get();
    jnz_p->ix = J.size() - 1;
    return jnz_p;
}

HessianNZ* Model::add_H_NZ(Constraint* con,
                           Variable*   var1,
                           Variable*   var2) {
    auto row_col = std::make_pair(std::max(var1->ix, var2->ix),
                                  std::min(var1->ix, var2->ix));
    H_vec.push_back(make_unique<HessianNZ>(con, var1, var2));
    auto hnz_p = H_vec.back().get();
    H[row_col].push_back(hnz_p);
    hnz_p->ix = H_vec.size() - 1;
    return hnz_p;
}

Connection* Model::add_connection(Variable* var1,
                                  Variable* var2) {
    if (var1->is_free() && var2->is_free()) return nullptr;
    if (var1->unit->kind != var2->unit->kind) return nullptr;

    if (var1->is_fixed()) {
        var1->free();
    } else {
        var2->free();
    }

    // Constraint is var1 - var2 == 0.
    auto eq = add_constraint(format("{}{}=={}", cnx.prefix, var1->ix, var2->ix));

    cnx.conn_vec.push_back(make_unique<Connection>(
        eq,
        var1,
        var2,
        add_J_NZ(eq, var1),
        add_J_NZ(eq, var2)
    ));

    return cnx.conn_vec.back().get();
}

bool Model::add_bridge(Stream* sfrom, Stream* sto) {
    if (sfrom->fs->m != sto->fs->m) return false;
    if (sfrom->comps != sto->comps) return false;
    auto M = sfrom->fs->m;
    auto to_sv = sto->to->x_strm[sto];
    auto from_sv = sfrom->from->x_strm[sfrom];
    for (const auto& c : sfrom->comps) {
        auto conn_p = M->add_connection(to_sv.mass[c], from_sv.mass[c]);
        if (!conn_p) return false;
    }
    return true;
}

Price* Model::add_price(string_view name_, double value_, Unit* unit_) {
    auto price = make_unique<Price>(name_, value_, unit_);
    auto price_p = price.get();
    prices[string(name_)] = std::move(price);
    return price_p;
}

Objective* Model::add_objective(string_view name_, Unit* unit_, double scale_) {
    auto obj_ = make_unique<Objective>(name_, this, unit_, scale_);
    auto obj_p = obj_.get();
    objectives[string(name_)] = std::move(obj_);
    return obj_p;
}

void Model::show_variables(ostream& os) const {
    int count {0};
    os << var_header;
    for (const auto& var : x_vec) {
        os << *var << '\n';
        count++;
    }
    os << var_footer;
    os << count << " Variable" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

void Model::show_constraints(ostream& os) const {
    int count {0};
    os << con_header;
    for (const auto& con : g_vec) {
        count++;
        os << *con << '\n';
    }
    os << con_footer;
    os << count << " Constraint" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

void Model::show_jacobian(ostream& os) const {
    int count {0};
    os << jac_header;
    for (const auto& jnz : J) {
        os << *jnz << '\n';
        count++;
    }
    os << jac_footer;
    os << count << " Jacobian NZ" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

void Model::show_hessian(ostream& os) const {
    int count {0};
    os << hess_header;
    for (const auto& hnz : H_vec) {
        os << *hnz << '\n';
        count++;
    }
    os << hess_footer;
    os << count << " Hessian NZ" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

void Model::show_connections(ostream& os) const {
    int count {0};
    os << conn_header;
    for (const auto& conn : cnx.conn_vec) {
        os << *conn << '\n';
        count++;
    }
    os << conn_footer;
    os << count << " connection" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

void Model::show_prices(ostream& os) const {
    int count {0};
    os << price_header;
    for (const auto& price : std::views::values(prices)) {
        os << *price << '\n';
        count++;
    }
    os << price_footer;
    os << count << " price" << (count == 1 ? "" : "s") << " shown\n\n";
    os << std::flush;
}

void Model::show_objective_rec(Objective* obj_, ostream& os) const {
    for (const auto& term : std::views::values(obj_->terms)) {
        if (std::holds_alternative<unique_ptr<ObjTerm>>(term)) {
            const auto& objterm = std::get<unique_ptr<ObjTerm>>(term);
            os << *objterm << '\n';
        }
        else
            show_objective_rec(std::get<Objective*>(term), os);
    }
    os << *obj_ << '\n';
    os << std::flush;
} 

void Model::show_objective(Objective* obj_, ostream& os) const {
    if (obj_ == nullptr) obj_ = obj;
    os << "Objective: " << obj_->name << '\n';
    os << obj_header;
    show_objective_rec(obj_, os);
    os << obj_footer << '\n';
    os << std::flush;
}

void Model::show_obj_grad(ostream& os) const {
    os << "Gradient of objective: " << obj->name << '\n';
    os << obj_grad_header;
    for (const auto& g : obj->grad) {
        auto i   = std::get<0>(g);
        auto val = std::get<1>(g);
        os << format("|{}|{:32}|{}|\n", str(i), x_vec[i]->name, str(val));
    }
    os << obj_grad_footer << '\n';
    os << std::flush;
}

void Model::show_model(ostream& os) const {
    auto nx = x_vec.size();
    auto mg = g_vec.size();
    int nfixed = 0;
    for (int i = 0; i < nx; i++)
        if (x_vec[i]->is_fixed()) nfixed++;
    int nfree = nx - nfixed;
    int nDOF = nfree - mg;
    int njnz = J.size();
    int nhnz = H.size();
    os << "Model: " << name << '\n';
    os << "  Number of equations          = " << mg      << '\n';
    os << "  Number of free variables     = " << nfree   << '\n';
    os << "  Number of fixed variables    = " << nfixed  << '\n';
    os << "  Number of variables          = " << nx      << '\n';
    os << "  Number of Jacobian non-zeros = " << njnz    << '\n';
    os << "  Number of Hessian non-zeros  = " << nhnz    << '\n';
    if (nDOF == 0)
        os << "Model is square\n\n";
    else
        os << format("Model has {} degree{} of freedom\n\n", nDOF, (nDOF == 1 ? "" : "s"));
    os << std::flush;

}

void Model::write_variables(ostream& os) const {
    os << "Eval([[\n";
    for (const auto& var : x_vec)
        os << format("    {:32} = {}_{}\n", var->name, str(var->value), var->unit->str);
    os << "]])\n";
    os << std::flush;
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
    if (obj == nullptr)
        obj_value = 1.0;
    else {
        for (Index i = 0; const auto& var : x_vec)
            var->convert_and_set(x_in[i++]);
        obj_value = eval_objective();
    }
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
    if (obj == nullptr) return true;
    for (Index i = 0; const auto& var : x_vec)
        var->convert_and_set(x_in[i++]);
    eval_obj_grad();
    for (const auto& g : obj->grad) {
        auto [i, val] = g;
        grad_f[i] += obj->convert_from_base(val);
    }
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
        for (Index i = 0; const auto& key : std::views::keys(H)) {
            auto [row, col] = key;
            iRow[i] = row;
            jCol[i] = col;
            i++;
        }
    else {
        for (Index i = 0; const auto& var : x_vec)
            var->convert_and_set(x_in[i++]);
        eval_hessian();
        for (Index i = 0; const auto& elems : std::views::values(H)) {
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

string str(Index i) {
    return format("{:>5}", i);
}

string str(double d) {
    return format("{:14.7g}", d);
}

string str(Ndouble nd) {
    return nd.has_value() ? format("{:14.7g}", nd.value()) : format("{:14}", "");
}

string str(VariableSpec spec) {
    return spec == VariableSpec::Fixed ? " = " : "   ";
}
