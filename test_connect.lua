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
        { "frac", 1.0 },
        { "%",    1.0 }
    }
}

-- test 1: Create a model.
M, FS = Model("test_calc", "index", kinds, units)
if M == nil or FS == nil then goto FAILED end
print("Test 1 passed")
n_test = n_test + 1

-- test 2: Add some streams.
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

-- test 4: Create a Flowsheet under the index flowsheet..
fs_sub = Flowsheet("sub")
if fs_sub == nil then goto FAILED end
print("Test 4 passed")
n_test = n_test + 1

-- test 5: Create streams and a Splitter block in the sub flowsheet.
in1, out1, out2 = Streams(
    { "in",   OUT_comps },
    { "out1", OUT_comps },
    { "out2", OUT_comps }
)
spl1 = Splitter("spl1", { in1 }, { out1, out2 })
if spl1 == nil then goto FAILED end
print("Test 5 passed")
n_test = n_test + 1

ShowVariables()

-- test 6: Bridge the mix1 outlet and spl1 inlet streams.
ok = Bridge(OUT, in1)
if not ok then goto FAILED end
print("Test 6 passed")
n_test = n_test + 1

ok = Eval([[
    mix1.N1.mass_H2 = 1.0
    mix1.N1.mass_O2 = 1.0
    mix1.N2.mass_H2 = 1.0
    mix1.N2.mass_O2 = 1.0
    mix1.N2.mass_CO = 1.0
    sub.spl1.in.mass_H2 = 1.0
    sub.spl1.in.mass_O2 = 1.0
    sub.spl1.in.mass_CO = 1.0
    sub.spl1.out1.splitfrac = 0.5
]])
if not ok then goto FAILED end

Init()
ShowVariables()

Eval("mix1.N2.mass_H2 = 2.0")
print("Before solve:\n")
ShowVariables()

SolverOption("hessian_approximation", "exact")
SolverOption("max_iter", 30)
SolverOption("derivative_test", "second-order");
SolverOption("tol", 1.0e-6)

-- test 7: Initialize the solver.
status = InitSolver()
if status ~= 0 then goto FAILED end
print("Test 7 passed")
n_test = n_test + 1

-- test 8: Solve the problem.
status = Solve()
if status ~= 0 then goto FAILED end
print("Test 8 passed")
n_test = n_test + 1

print("After solve:\n")
ShowVariables()

print(string.format("\nAll %d tests passed\n", n_test - 1))
do return end

::FAILED::
print(string.format("\nTest %d failed\n", n_test))
do return end
