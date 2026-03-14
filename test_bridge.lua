---@diagnostic disable: undefined-global
function isapprox(x1, x2, tol)
    tol = tol or 1.0e-8
    return math.abs(x1 - x2) < tol
end

n_test = 1

dofile("test_units.lua")

-- test 1: Create a model.
M = Model("test_calc", "index", unitset)
FS = M.index_fs;
if M == nil or FS == nil then goto FAILED end
print("Test 1 passed")
n_test = n_test + 1

-- test 2: Add some streams.
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

-- test 4: Create a Flowsheet under the index flowsheet.
fs_sub = FS:Flowsheet("sub")
if fs_sub == nil then goto FAILED end
print("Test 4 passed")
n_test = n_test + 1

-- test 5: Create streams and a Splitter block in the sub flowsheet.
in1, out1, out2 = fs_sub:Streams(
    { "in",   OUT_comps },
    { "out1", OUT_comps },
    { "out2", OUT_comps }
)
spl1 = fs_sub:Splitter("spl1", { in1 }, { out1, out2 })
if spl1 == nil then goto FAILED end
print("Test 5 passed")
n_test = n_test + 1

M:show_variables()

-- test 6: Bridge the mix1 outlet and spl1 inlet streams.
ok = M:add_bridge(OUT, in1)
if not ok then goto FAILED end
print("Test 6 passed")
n_test = n_test + 1

ok = M:eval([[
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

M:init()

M:eval("mix1.N2.mass_H2 = 2.0")
print("Before solve:\n")
M:show_variables()

solver = Solver()
if solver == nil then goto FAILED end

solver:set_option("hessian_approximation", "exact")
solver:set_option("max_iter", 30)
solver:set_option("derivative_test", "second-order");
solver:set_option("tol", 1.0e-6)

-- test 7: Initialize the solver.
status = solver:init()
if status ~= 0 then goto FAILED end
print("Test 7 passed")
n_test = n_test + 1

-- test 8: Solve the problem.
status = solver:solve(M)
if status ~= 0 then goto FAILED end
print("Test 8 passed")
n_test = n_test + 1

print("After solve:\n")
M:show_variables()

print(string.format("\nAll %d tests passed\n", n_test - 1))
do return end

::FAILED::
print(string.format("\nTest %d failed\n", n_test))
do return end
