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
M, FS = Model("test_splitter", "index", kinds, units)
if M == nil or FS == nil then goto FAILED end
print("Test 1 passed")
n_test = n_test + 1

-- test 2: Add three streams.
IN_comps = {"H2", "O2" }
OUT1_comps = { "H2", "O2" }
OUT2_comps = OUT1_comps

IN, OUT1, OUT2 = Streams(
    { "IN", IN_comps },
    { "OUT1", OUT1_comps },
    { "OUT2", OUT2_comps }
)
if IN == nil or OUT1 == nil or OUT2 == nil then goto FAILED end
print("Test 2 passed")
n_test = n_test + 1

-- test 3: Create a Splitter block.
spl1 = Splitter("spl1", { IN }, { OUT1, OUT2 })
if spl1 == nil then goto FAILED end
print("Test 3 passed")
n_test = n_test + 1

ok = Eval([[
    spl1.IN.mass_H2 = 1.0
    spl1.IN.mass_O2 = 2.0
    spl1.OUT1.splitfrac = 0.3
    spl1.OUT2.splitfrac = 0.7
    ]]
)
ShowVariables()
Init()
ShowVariables(spl1)

EvalConstraints()
ShowConstraints()

EvalJacobian()
ShowJacobian()

EvalHessian()
ShowHessian()

ok = Eval("spl1.IN.mass_H2 = 2.0")
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
