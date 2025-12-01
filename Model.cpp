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

VariablePtr Model::add_variable(const std::string& name_, const Unit& unit)
{
    auto v = std::make_shared<Variable>(name_, unit);
    x.push_back(v);
    x_map[name_] = x.back();
    return v;
}

void Block::make_stream_variables(const StreamPtr& strm)
{
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

void Block::make_stream_variables(const std::vector<StreamPtr>& strms1, const std::vector<StreamPtr>& strms2) {
    for (const auto& strm : strms1)
        make_stream_variables(strm);
    for (const auto& strm : strms2)
        make_stream_variables(strm);
}

ConstraintPtr Model::add_constraint(const std::string& name_)
{
    auto con = std::make_shared<Constraint>(name_);
    g.push_back(con);
    g_map[name_] = g.back();
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

FlowsheetPtr Model::add_flowsheet(const std::string& name_, FlowsheetPtr parent_) const
{
    auto fs = std::make_shared<Flowsheet>(name_, parent_);
    parent_->children.push_back(fs);
    return fs;
}

StreamPtr Model::add_stream(const std::string& name_, FlowsheetPtr fs,  Comps& comps) const
{
    auto strm = make_shared<Stream>(name_, fs, comps);
    fs->streams[name_] = strm;
    return strm;
}

void finish_block(const BlockPtr blk)
{
    blk->fs->blocks[blk->name] = blk;
    for (const auto& sin : blk->inlets)
        sin->to = blk;
    for (const auto& sout : blk->outlets)
        sout->from = blk;
}

void Block::eval_constraints() {}

//std::string makePrefix(const string& fsName, const string& objName) {
//    return (fsName.length() > 0 ? fsName + "." : "") + objName + ".";
//}

