---@diagnostic disable: undefined-global
function isapprox(x1, x2, tol)
    tol = tol or 1.0e-8
    return math.abs(x1 - x2) < tol
end

n_test = 1

-- set up the units for the model.
kinds = {
    massflow = { "kg/hr",    "kg/hr"    },  -- {base_unit_str, default_unit_str}
    massfrac = { "massfrac", "massfrac" },
    frac     = { "frac",     "frac"     },
    massval  = { "$/kg",     "$/kg"     },
    flowval  = { "$/hr",     "$/hr"     }
}
units = {
    massflow = {
        { "kg/hr", 1.0,          0.0 },     -- {unit_str, unit_ratio, unit_offset} unit_offset optional
        { "lb/hr", 1.0 / 2.20462     },
        { "t/hr" , 1000.0            }
    },
    massfrac = {
        { "massfrac", 1.0         },
        { "mass%",    1.0 / 100.0 }
    },
    frac = {
        { "frac", 1.0 },
        { "%",    1.0 }
    },
    massval = {
        { "$/kg", 1.0     },
        { "$/lb", 2.20462 }
    },
    flowval = {
        { "$/hr",  1.0  },
        { "$/min", 0.5 }
    }
}

-- test 1: Create a model.
M, FS = Model("test_obj", "index", kinds, units)
if M == nil or FS == nil then goto FAILED end
print("Test 1 passed")
n_test = n_test + 1

-- test 2: Add three streams.
N1_comps = {"H2", "O2" }
N2_comps = { "H2", "O2", "CO" }
OUT_comps = N2_comps

N1, N2, OUT = Streams(
    { "N1", N1_comps },
    { "N2", N2_comps },
    { "OUT", OUT_comps }
)
if N1 == nil or N2 == nil or OUT == nil then goto FAILED end
print("Test 2 passed")
n_test = n_test + 1

-- test 3: Create a Mixer block.
mix1 = Mixer("mix1", { N1, N2 }, { OUT })
if mix1 == nil then goto FAILED end
print("Test 3 passed")
n_test = n_test + 1

-- test 4: Get a unit.
u = Unit("$/kg")
if u == nil then goto FAILED end
print("Test 4 passed")
n_test = n_test + 1

-- test 5: Create some prices with that unit.
N1_price, N2_price, OUT_price = Prices( {"Prices.N1_kg", 0.50, u},
                                        {"Prices.N2_kg", 0.10, u},
                                        {"Prices.OUT_kg", 0.3, u})
if N1_price == nil or N2_price == nil or OUT_price == nil then goto FAILED end
print("Test 5 passed")
n_test = n_test + 1


-- test 6: Change the unit on a price.
val = ChangeUnit(N1_price, "$/lb")
if val == nil then goto FAILED end
print("Test 6 passed")
n_test = n_test + 1

--ShowPrices()

--val = ChangeUnit(N2_price, "$/kg")

-- test 7: Create an objective.
costs, N1_val = Objective( "costs", "$/min", -1.0,
    {"N1_val", "mix1.N1.mass", "Prices.N1_kg", "$/hr"}
)
if costs == nil or N1_val == nil then goto FAILED end
print("Test 7 passed")
n_test = n_test + 1

-- test 8: Add terms to the objective.
costs, N2_val = Objective( costs,
    {"N2_val", "mix1.N2.mass", "Prices.N2_kg", "$/hr"}
)
if costs == nil or N2_val == nil then goto FAILED end
print("Test 8 passed")
n_test = n_test + 1

sales, OUT_val = Objective( "sales", "$/hr",
    {"OUT_val", "mix1.OUT.mass", "Prices.OUT_kg", "$/hr"}
)

profit = Objective( "profit", "$/min",
    sales,
    costs)

SetObjective(profit)

Eval([[
    mix1.N1.mass_H2 = 1.0
    mix1.N1.mass_O2 = 1.0
    mix1.N2.mass_H2 = 1.0
    mix1.N2.mass_O2 = 1.0
    mix1.N2.mass_CO = 1.0
    mix1.N1.mass    > 0.5
    mix1.N1.mass    < 3.0
    mix1.N2.mass    > 0.5
    mix1.N2.mass    < 3.0
    mix1.OUT.mass   < 4.0
]])

Init()

EvalObjective()
ShowPrices()
ShowObjective()
EvalObjGrad()
ShowObjGrad()

Eval([[
    free mix1.N1.mass_H2
    fix  mix1.N1.massfrac_H2
    free mix1.N1.mass_O2
    free  mix1.N1.mass
    
    free mix1.N2.mass_H2
    fix  mix1.N2.massfrac_H2
    free mix1.N2.mass_O2
    fix  mix1.N2.massfrac_O2
    free mix1.N2.mass_CO
    free  mix1.N2.mass
]])

print("Before solve:\n")
ShowVariables()
ShowModel()
EvalObjective()
--EvalObjGrad()
ShowObjective()
--ShowObjGrad()

--do return end
SolverOption("hessian_approximation", "exact")
SolverOption("max_iter", 50)
SolverOption("derivative_test", "second-order");
SolverOption("tol", 1.0e-6)
SolverOption("obj_scaling_factor", -1.0)
SolverOption("grad_f_constant", "yes")

-- test 9: Initialize the solver.
status = InitSolver()
if status ~= 0 then goto FAILED end
print("Test 9 passed")
n_test = n_test + 1

-- test 10: Solve the problem.
status = Solve()
if status ~= 0 then goto FAILED end
print("Test 10 passed")
n_test = n_test + 1

print("After solve:\n")
ShowVariables()
ShowObjective()

print(string.format("\nAll %d tests passed\n", n_test - 1))
do return end

::FAILED::
print(string.format("\nTest %d failed\n", n_test))
do return end
