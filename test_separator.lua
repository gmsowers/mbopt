---@diagnostic disable: undefined-global
function isapprox(x1, x2, tol)
    tol = tol or 1.0e-8
    return math.abs(x1 - x2) < tol
end

n_test = 1

dofile("test_units.lua")

-- test 1: Create a model.
M = Model("test_separator", "index", unitset)
if M == nil then goto FAILED end
FS = M.index_fs
print("Test 1 passed")
n_test = n_test + 1

-- test 2: Add three streams.
IN_comps = {"H2", "O2" }
OUT1_comps = { "H2", "O2" }
OUT2_comps = { "O2" }

IN, OUT1, OUT2 = FS:Streams(
    { "IN", IN_comps },
    { "OUT1", OUT1_comps },
    { "OUT2", OUT2_comps }
)
if IN == nil or OUT1 == nil or OUT2 == nil then goto FAILED end
print("Test 2 passed")
n_test = n_test + 1

-- test 3: Create a Separator block.
sep1 = FS:Separator("sep1", { IN }, { OUT1, OUT2 })
if sep1 == nil then goto FAILED end
print("Test 3 passed")
n_test = n_test + 1

ok = M:eval([[
    sep1.IN.mass_H2 = 1.0
    sep1.IN.mass_O2 = 1.0
    sep1.OUT1.split_O2 = 0.3
    ]]
)

M:init()

ok = M:eval("sep1.IN.mass_H2 = 2.0")
print("Before solve:\n")
M:show_variables()

-- test 4: Set up to solve.
solver = Solver()
if solver == nil then goto FAILED end

solver:set_option("hessian_approximation", "exact")
solver:set_option("max_iter", 30)
solver:set_option("derivative_test", "second-order");
solver:set_option("tol", 1.0e-6)

status = solver:init()
if status ~= 0 then goto FAILED end
print("Test 4 passed")
n_test = n_test + 1

-- test 5: Solve the problem.
status = solver:solve(M)
if status ~= 0 then goto FAILED end
print("Test 5 passed")
n_test = n_test + 1

print("After solve:\n")
M:show_variables()

print(string.format("\nAll %d tests passed\n", n_test - 1))
do return end

::FAILED::
print(string.format("\nTest %d failed\n", n_test))
do return end
