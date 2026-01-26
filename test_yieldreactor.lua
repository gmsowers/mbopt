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
    frac     = { "frac",     "frac"     }
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
        { "frac", 1.0         },
        { "%",    1.0 / 100.0 }
    }
}

-- test 1: Create a model.
M, FS = Model("test_yieldreactor", "index", kinds, units)
if M == nil or FS == nil then goto FAILED end
print("Test 1 passed")
n_test = n_test + 1

-- test 2: Add two streams.
IN_comps = {"C2H6"}
OUT_comps = { "H2", "C2H4", "C2H6" }

IN, OUT = Streams(
    { "IN", IN_comps },
    { "OUT", OUT_comps }
)
if IN == nil or OUT == nil then goto FAILED end
print("Test 2 passed")
n_test = n_test + 1

-- test 3: Create a YieldReactor block.
rx1 = YieldReactor("rx1", { IN }, { OUT })
if rx1 == nil then goto FAILED end
print("Test 3 passed")
n_test = n_test + 1

ok = Eval([[
    rx1.IN.mass_C2H6     = 1.0
    rx1.y_H2_from_C2H6   = 0.1
    rx1.y_C2H4_from_C2H6 = 0.55
    ]]
)
Init()
ShowVariables()
EvalConstraints()
ShowConstraints()
EvalJacobian()
ShowJacobian()
EvalHessian()
ShowHessian()

ok = Eval("rx1.IN.mass_C2H6 = 2.0")
print("Before solve:\n")
ShowVariables()

SolverOption("hessian_approximation", "exact")
SolverOption("max_iter", 30)
SolverOption("derivative_test", "second-order");
SolverOption("tol", 1.0e-6)

-- test 4: Initialize the solver.
status = InitSolver()
if status ~= 0 then goto FAILED end
print("Test 4 passed")
n_test = n_test + 1

-- test 5: Solve the problem.
status = Solve()
if status ~= 0 then goto FAILED end
print("Test 5 passed")
n_test = n_test + 1

print("After solve:\n")
ShowVariables()

print(string.format("\nAll %d tests passed\n", n_test - 1))
do return end

::FAILED::
print(string.format("\nTest %d failed\n", n_test))
do return end
