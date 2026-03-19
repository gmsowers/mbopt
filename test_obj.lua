---@diagnostic disable: undefined-global
function isapprox(x1, x2, tol)
    tol = tol or 1.0e-8
    return math.abs(x1 - x2) < tol
end

n_test = 1

dofile("test_units.lua")

-- test 1: Create a model.
M = Model("test_obj", "index", unitset)
FS = M.index_fs
if M == nil or FS == nil then goto FAILED end
print("Test 1 passed")
n_test = n_test + 1

-- test 2: Add three streams.
N1_comps = {"H2", "O2" }
N2_comps = { "H2", "O2", "CO" }
OUT_comps = N2_comps

N1, N2, OUT = FS:Streams(
    { "N1", N1_comps },
    { "N2", N2_comps },
    { "OUT", OUT_comps }
)
if N1 == nil or N2 == nil or OUT == nil then goto FAILED end
print("Test 2 passed")
n_test = n_test + 1

-- test 3: Create a Mixer block.
mix1 = FS:Mixer("mix1", { N1, N2 }, { OUT })
if mix1 == nil then goto FAILED end
print("Test 3 passed")
n_test = n_test + 1

-- test 4: Get a unit.
u = M.unitset.units["$/kg"]
if u == nil then goto FAILED end
print("Test 4 passed")
n_test = n_test + 1

-- test 5: Create some prices with that unit.
N1_price, N2_price, OUT_price = M:Prices( {"Prices.N1_kg", 0.50, u},
                                          {"Prices.N2_kg", 0.10, u},
                                          {"Prices.OUT_kg", 0.3, u} )
if N1_price == nil or N2_price == nil or OUT_price == nil then goto FAILED end
print("Test 5 passed")
n_test = n_test + 1

-- test 6: Change the unit on a price.
N1_price:change_unit(M.unitset.units["$/lb"])
if not isapprox(N1_price.v, 0.5/2.20462) then goto FAILED end
print("Test 6 passed")
n_test = n_test + 1

M:show_prices()

-- test 7: Create an objective.
costs, N1_val = M:add_objective( "costs", "$/min", -1.0,
    {"N1_val", "mix1.N1.mass", "Prices.N1_kg", "$/hr"}
)
if costs == nil or N1_val == nil then goto FAILED end
print("Test 7 passed")
n_test = n_test + 1

-- test 8: Add terms to the objective.
N2_val = costs:add_terms(
    {"N2_val", "mix1.N2.mass", "Prices.N2_kg", "$/hr"}
)
if N2_val == nil then goto FAILED end
print("Test 8 passed")
n_test = n_test + 1

sales, OUT_val = M:add_objective( "sales", "$/hr",
    {"OUT_val", "mix1.OUT.mass", "Prices.OUT_kg", "$/hr"}
)

profit = M:add_objective( "profit", "$/min",
    sales,
    costs)

M.objective = profit

M:eval([[
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

M:init()

M:eval_objective()
M:show_prices()
M:show_objective()
M:eval_objgrad()
M:show_objgrad()

M:eval([[
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

print(M)
print("Before solve:\n")
M:show_variables()
M:eval_objective()
M:show_objective()

solver = Solver()
if solver == nil then goto FAILED end

solver:set_option("hessian_approximation", "exact")
solver:set_option("max_iter", 30)
solver:set_option("derivative_test", "second-order");
solver:set_option("tol", 1.0e-6)
solver:set_option("obj_scaling_factor", -1.0)
solver:set_option("grad_f_constant", "yes")

-- test 9: Initialize the solver.
status = solver:init()
if status ~= 0 then goto FAILED end
print("Test 9 passed")
n_test = n_test + 1

-- test 10: Solve the problem.
status = solver:solve(M)
if status ~= 0 then goto FAILED end
print("Test 10 passed")
n_test = n_test + 1

print("After solve:\n")
M:show_variables()
M:show_objective()

print(string.format("\nAll %d tests passed\n", n_test - 1))
do return end

::FAILED::
print(string.format("\nTest %d failed\n", n_test))
do return end
