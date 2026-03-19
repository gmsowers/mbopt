---@diagnostic disable: undefined-global
function isapprox(x1, x2, tol)
    tol = tol or 1.0e-7
    return math.abs(x1 - x2) < tol
end

n_test = 1

dofile("test_units.lua")

-- test 1: Create a model.
M = Model("test_basics", "index", unitset)
if M == nil then goto FAILED end
print("Test 1 passed")
n_test = n_test + 1
FS = M.index_fs

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
if N2 ~= FS:get("N2") then goto FAILED end
print("Test 2 passed")
n_test = n_test + 1

-- test 3: Create a Mixer block.
mix1 = FS:Mixer("mix1", { N1, N2 }, { OUT })
if mix1 == nil then goto FAILED end
if mix1 ~= FS:get("mix1") then goto FAILED end
print("Test 3 passed")
n_test = n_test + 1

-- test 4: Evaluate expressions.
ok = M:eval([[
    mix1.N1.mass_O2 = 1.0    
    mix1.N1.mass_H2 = 1.0
    mix1.N2.mass_H2 = 1.0
    mix1.N2.mass_O2 = 1.0
    mix1.N2.mass_CO = 1.0_lb/hr
    mix1.N2.mass_CO < 1000.0
    free mix1.N1.mass_O2    
    fix  mix1.N1.massfrac_O2
]])
if not ok then goto FAILED end
print("Test 4 passed")
n_test = n_test + 1

-- test 5: Get a variable value by name, and check its spec and value.
var1 = M:get("mix1.N2.mass_CO")
val = var1.v;
u = var1.u;
if val == nil or u == nil then goto FAILED end
if not isapprox(val, 1.0/2.20462) then goto FAILED end
if var1.spec ~= "fixed" then goto FAILED end
if u.str ~= "kg/hr" then goto FAILED end

ok = M:eval([[
    fix  mix1.N1.mass_O2    
    free mix1.N1.massfrac_O2
]])
if not ok then goto FAILED end
print("Test 5 passed")
n_test = n_test + 1

-- test 6: Change a variable's unit.
unew = M.unitset.units["lb/hr"]
if unew == nil then goto FAILED end
var2 = M:get("mix1.N2.mass_O2")
var2:change_unit(unew)
if not isapprox(var2.v, 2.20462) then goto FAILED end
if var2.u.str ~= "lb/hr" then goto FAILED end
print("Test 6 passed")
n_test = n_test + 1

-- test 7: Get a variable's upper bound.
ub = var1.ub
if not isapprox(ub, 1000.0) then goto FAILED end
print("Test 7 passed")
n_test = n_test + 1

-- test 8: Make sure an unset lower bound is nil.
lb = var1.lb
if lb ~= nil then goto FAILED end
print("Test 8 passed")
n_test = n_test + 1

print("Before init:\n")
M:eval_constraints()
M:show_constraints()
M:init()
print("After init:\n")
mix1:eval_constraints()
mix1:show_constraints()

-- test 9: Create a Solver and set some options.
solver = Solver()
if solver == nil then goto FAILED end

solver:set_option("hessian_approximation", "exact")
solver:set_option("max_iter", 30)
solver:set_option("derivative_test", "second-order");
solver:set_option("tol", 1.0e-6)
print("Test 9 passed")
n_test = n_test + 1

-- test 10: Initialize the solver.
status = solver:init()
if status ~= 0 then goto FAILED end
print("Test 10 passed")
n_test = n_test + 1

-- test 11: Solve the problem.
M:eval("mix1.N1.mass_O2 = 2.0")
print("Before solve:\n")
M:show_variables()

status = solver:solve(M)
if status ~= 0 then goto FAILED end
print("Test 11 passed")
n_test = n_test + 1

print("After solve:\n")
M:show_variables()

print(string.format("\nAll %d tests passed\n", n_test - 1))
do return end

::FAILED::
print(string.format("\nTest %d failed\n", n_test))
do return end
