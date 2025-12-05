#include <algorithm>
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

void Block::eval_constraints() {}

//---------------------------------------------------------

VariablePtr Model::add_variable(const std::string& name_, const Unit& unit)
{
    auto v = std::make_shared<Variable>(name_, unit);
    x.push_back(v);
    x_map[name_] = v;
    return v;
}

ConstraintPtr Model::add_constraint(const std::string& name_)
{
    auto con = std::make_shared<Constraint>(name_);
    g.push_back(con);
    g_map[name_] = con;
    return con;
}

JacobianElementPtr Model::add_jacobian_element(const ConstraintPtr& con_, const VariablePtr& var_) {
    auto j = std::make_shared<JacobianElement>(con_, var_);
    J.push_back(j);
    return j;
}

HessianElementPtr Model::add_hessian_element(const ConstraintPtr& con_, const VariablePtr& var1_, const VariablePtr& var2_) {
    auto h = std::make_shared<HessianElement>(con_, var1_, var2_);
    H.push_back(h);
    return h;
}
