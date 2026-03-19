---@diagnostic disable: undefined-global
function isapprox(x1, x2, tol)
    tol = tol or 1.0e-8
    return math.abs(x1 - x2) < tol
end

n_test = 1

dofile("test_units.lua")

-- test 1: Create a model.
M = Model("test_calc", "index", unitset)
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
print("Test 2 passed")
n_test = n_test + 1

-- test 3: Create a Mixer block.
mix1 = FS:Mixer("mix1", { N1, N2 }, { OUT })
if mix1 == nil then goto FAILED end
print("Test 3 passed")
n_test = n_test + 1

-- test 4: Create a Calc.
calc1 = FS:Calc("calc1")
if calc1 == nil then goto FAILED end
print("Test 4 passed")
n_test = n_test + 1

-- test 5: Get a unit.
u = M:get("mix1.N1.mass").unit
if u == nil then goto FAILED end
print("Test 5 passed")
n_test = n_test + 1

-- test 6: Create a variable with that unit and assign a value to it.
inlet_mass_sum = calc1:add_variables({"inlet_mass_sum", u})
if inlet_mass_sum == nil then goto FAILED end
inlet_mass_sum.value = 1.0
print("Test 6 passed")
n_test = n_test + 1
calc1:show_variables()
M:show_variables()

-- test 7: Create a constraint.
eq_sum_inlets = calc1:add_constraints("mix1_sum_inlets")
if eq_sum_inlets == nil then goto FAILED end
print("Test 7 passed")
n_test = n_test + 1
calc1:show_constraints()
M:show_constraints()

-- test 8: Add Jacobian nonzeros.
-- The calc1 constraint is mix1.N1.mass + mix1.N2.mass - calc1.inlet_mass_sum == 0 
N1_mass = M:get("mix1.N1.mass")
N2_mass = M:get("mix1.N2.mass")
if N1_mass == nil or N2_mass == nil then goto FAILED end
JNZ1, JNZ2, JNZ3 = calc1:add_jacobian_nzs(
        {eq_sum_inlets, N1_mass},
        {eq_sum_inlets, N2_mass},
        {eq_sum_inlets, inlet_mass_sum}
    )
if JNZ1 == nil or JNZ2 == nil or JNZ3 == nil then goto FAILED end
calc1:show_jacobian()

M:eval([[
    mix1.N1.mass_H2 = 1.0
    mix1.N1.mass_O2 = 1.0
    mix1.N2.mass_H2 = 1.0
    mix1.N2.mass_O2 = 1.0
    mix1.N2.mass_CO = 1.0
]])

function calc1_initialize()
    inlet_mass_sum.v = N1_mass.bv + N2_mass.bv
end
print(calc1)
M:init()
M:show_variables()

function calc1_eval_constraints()
    eq_sum_inlets.v = N1_mass.bv + N2_mass.bv - inlet_mass_sum.bv
end
M:eval_constraints()
M:show_constraints()

function calc1_eval_jacobian()
    JNZ1.v = 1.0
    JNZ2.v = 1.0
    JNZ3.v = -1.0
end
M:eval_jacobian()
M:show_jacobian()

function calc1_eval_hessian()
end
print("Test 8 passed")
n_test = n_test + 1

M:eval("mix1.N1.mass_O2 = 2.0")
print("Before solve:\n")
M:show_variables()

solver = Solver()
if solver == nil then goto FAILED end

solver:set_option("hessian_approximation", "exact")
solver:set_option("max_iter", 30)
solver:set_option("derivative_test", "second-order");
solver:set_option("tol", 1.0e-6)

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

print(string.format("\nAll %d tests passed\n", n_test - 1))
do return end

::FAILED::
print(string.format("\nTest %d failed\n", n_test))
do return end
