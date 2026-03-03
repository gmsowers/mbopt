---@diagnostic disable: undefined-global
function isapprox(x1, x2, tol)
    tol = tol or 1.0e-8
    return math.abs(x1 - x2) < tol
end

n_test = 1

dofile("test_units.lua")

-- test 1: Create a model.
M, FS = Model("test_calc", "index", unitset)
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

-- test 4: Create a Calc.
calc1 = Calc("calc1")
if calc1 == nil then goto FAILED end
print("Test 4 passed")
n_test = n_test + 1

-- test 5: Get a unit.
u = Unit("mix1.N1.mass")
if u == nil then goto FAILED end
print("Test 5 passed")
n_test = n_test + 1

-- test 6: Create a variable with that unit.
inlet_mass_sum = Variables(calc1, {"inlet_mass_sum", u})
if inlet_mass_sum == nil then goto FAILED end
print("Test 6 passed")
n_test = n_test + 1
ShowVariables()

-- test 7: Create a constraint.
eq_sum_inlets = Constraints(calc1, "mix1_sum_inlets")
if eq_sum_inlets == nil then goto FAILED end
print("Test 7 passed")
n_test = n_test + 1
ShowConstraints()

-- test 8: Add Jacobian nonzeros.
-- The calc1 constraint is mix1.N1.mass + mix1.N2.mass - calc1.inlet_mass_sum == 0 
N1_mass = Var("mix1.N1.mass")
N2_mass = Var("mix1.N2.mass")
if N1_mass == nil or N2_mass == nill then goto FAILED end
JNZ1, JNZ2, JNZ3 = JacobianNZs(calc1,
        {eq_sum_inlets, N1_mass},
        {eq_sum_inlets, N2_mass},
        {eq_sum_inlets, inlet_mass_sum}
    )
if JNZ1 == nil or JNZ2 == nil or JNZ3 == nil then goto FAILED end
ShowJacobian()

Eval([[
    mix1.N1.mass_H2 = 1.0
    mix1.N1.mass_O2 = 1.0
    mix1.N2.mass_H2 = 1.0
    mix1.N2.mass_O2 = 1.0
    mix1.N2.mass_CO = 1.0
]])

function calc1_init()
    SetValue(inlet_mass_sum, BaseVal(N1_mass) + BaseVal(N2_mass))
end
function calc1_eval_constraints()
    SetValue(eq_sum_inlets, BaseVal(N1_mass) + BaseVal(N2_mass) - BaseVal(inlet_mass_sum))
end
function calc1_eval_jacobian()
    SetValue(JNZ1, 1.0)
    SetValue(JNZ2, 1.0)
    SetValue(JNZ3, -1.0)
end
function calc1_eval_hessian()
end

Init()
EvalConstraints()
EvalJacobian()
ShowConstraints(calc1)
ShowJacobian(calc1)

print("Test 8 passed")
n_test = n_test + 1

Eval("mix1.N1.mass_O2 = 2.0")
print("Before solve:\n")
ShowVariables()

SolverOption("hessian_approximation", "exact")
SolverOption("max_iter", 30)
SolverOption("derivative_test", "second-order");
SolverOption("tol", 1.0e-6)

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

print(string.format("\nAll %d tests passed\n", n_test - 1))
do return end

::FAILED::
print(string.format("\nTest %d failed\n", n_test))
do return end
