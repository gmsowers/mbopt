---@diagnostic disable: undefined-global
function isapprox(x1, x2, tol)
    tol = tol or 1.0e-8
    return math.abs(x1 - x2) < tol
end

n_test = 1

dofile("test_units.lua")

-- test 1: Create a model.
M = Model("test_stoic", "index", unitset)
FS = M.index_fs
if M == nil or FS == nil then goto FAILED end
print("Test 1 passed")
n_test = n_test + 1

-- test 2: Add two streams.
in1, out1 = FS:Streams(
    { "in1", {"H2", "C2H2"} },
    { "out1", {"H2", "C2H4", "C2H6"} }
)
if in1 == nil or out1 == nil then goto FAILED end
print("Test 2 passed")
n_test = n_test + 1

mw = {
    H2   = {2.0, "kg/kmol"},
    C2H2 = 26.0,
    C2H4 = 28.0,
    C2H6 = 30.0
}

stoic_coef = Reactions([[
    C2H2 + H2  -> C2H4
    C2H2 + 2H2 -> C2H6
]])

--stoic_coef = {
--    { {"C2H2", -1.0}, {"H2", -1.0}, {"C2H4", 1.0} },
--    { {"C2H2", -1.0}, {"H2", -2.0}, {"C2H6", 1.0} }
--}

conversion_keys = {"C2H2", "C2H2"}

-- test 3: Create a StoicReactor block.
arx = FS:StoicReactor("arx", { in1 }, { out1 }, mw, stoic_coef, conversion_keys)
if arx == nil then goto FAILED end
print("Test 3 passed")
n_test = n_test + 1

M:eval([[
    arx.in1.mass_H2    = 100.0
    arx.in1.mass_C2H2  = 50.0
    arx.conv_C2H2_rx_1 = 0.8
    arx.conv_C2H2_rx_2 = 0.2
]])

M:init()
M:eval("free arx.conv_C2H2_rx_2")

print("Before solve:\n")
M:show_variables()

solver = Solver()
if solver == nil then goto FAILED end

solver:set_option("hessian_approximation", "exact")
solver:set_option("max_iter", 30)
solver:set_option("derivative_test", "second-order");
solver:set_option("tol", 1.0e-6)

-- test 4: Solve the problem.
status = solver:init()
if status ~= 0 then goto FAILED end
status = solver:solve(M)
if status ~= 0 then goto FAILED end
print("Test 4 passed")
n_test = n_test + 1

print("After solve:\n")
M:show_variables()

print(string.format("\nAll %d tests passed\n", n_test - 1))
do return end

::FAILED::
print(string.format("\nTest %d failed\n", n_test))
do return end
