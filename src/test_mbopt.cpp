#include <iostream>
#include "IpIpoptApplication.hpp"
#include "Model.hpp"
#include "Mixer.hpp"

using Ipopt::SmartPtr;
using Ipopt::IpoptApplication;
using Ipopt::ApplicationReturnStatus;
using Ipopt::Solve_Succeeded;

int main()
{
    std::unordered_map<std::string, Unit> unit_set {};

    unit_set["massflow"] = {"kg/hr", "kg/hr", 1.0};
    unit_set["massfrac"] = {"frac", "frac", 1.0};

    SmartPtr<Model> M = new Model("mixer_test", unit_set);

    auto fs = M->index_fs;

    Comps c {"H2", "O2"};
    auto sin1 = fs->add_stream("N1", c);

    Comps c2 {"H2", "O2", "CO"};
    auto sin2 = fs->add_stream("N2", c2);

    Comps cmix {c + c2};
    auto sout = fs->add_stream("OUT", cmix);

    auto mix1 = fs->add_block<Mixer>("mix1", {sin1, sin2}, {sout});

    M->var("mix1.N1.mass_O2")->value = 1.0;
    M->var("mix1.N1.mass_H2")->value = 1.0;
    M->var("mix1.N2.mass_H2")->value = 1.0;
    M->var("mix1.N2.mass_O2")->value = 1.0;
    M->var("mix1.N2.mass_CO")->value = 1.0;
    M->var("mix1.N2.mass_CO")->lower = 0.0;
    M->var("mix1.N2.mass_CO")->upper = 1000.0;

    M->print_variables();
#ifdef UNDEF    
    for (Index i = 0; const auto var : M->x_vec)
        std::cout << i++ << " " << var->name << " = " << var->value << "    " << (var->spec == VariableSpec::Fixed ? "Fixed" : "") << '\n';

    M->initialize();

    M->eval_constraints();
    std::cout << std::endl;
    for (Index i = 0; const auto& con : M->g_vec)
        std::cout << i++ << " " << con->name << " = " << con->value << '\n';
    for (const auto var : M->g_vec)
        std::cout << var->name << " = " << var->value << '\n';

    M->eval_jacobian();
    std::cout << std::endl;
    for (Index i = 0; const auto elem : M->J)
        std::cout << i++ << " " << elem->con->ix << "," << elem->var->ix << " " << elem->con->name << " wrt " << elem->var->name << " = " << elem->value << '\n';

    M->x_map["mix1.N1.mass_H2"]->value = 2.0;
    
    SmartPtr<IpoptApplication> solver = IpoptApplicationFactory();
    solver->Options()->SetNumericValue("tol", 1.0e-6);
    solver->Options()->SetStringValue("hessian_approximation", "exact");
    solver->Options()->SetIntegerValue("max_iter", 50);
    //solver->Options()->SetIntegerValue("print_level", 8);
    solver->Options()->SetStringValue("derivative_test", "second-order");

    ApplicationReturnStatus status = solver->Initialize();
    if (status != Solve_Succeeded) {
        std::cout << "Initialization failed, status = " << status << '\n';
        return 1;
    }

    status = solver->OptimizeTNLP(M);
    if (status != Solve_Succeeded) {
        std::cout << "Solver failed, status= " << status << '\n';
        return 1;
    }
#endif
    return 0;
}