---@diagnostic disable: undefined-global
function isapprox(x1, x2, tol)
    tol = tol or 1.0e-8
    return math.abs(x1 - x2) < tol
end

n_test = 1

dofile("test_units.lua")

-- test 1: Create a model.
M, FS = Model("test_multi", "index", unitset)
if M == nil or FS == nil then goto FAILED end
print("Test 1 passed")
n_test = n_test + 1

-- test 2: Add four streams.
profeed, procgas, ethfeed, ethcgas = Streams(
    { "profeed", {"C3H8"} },
    { "procgas", {"CH4", "C2H4", "C3H8"} },
    { "ethfeed", {"C2H6" } },
    { "ethcgas", {"H2", "C2H4", "C2H6"} }
)
if profeed == nil or procgas == nil or ethfeed == nil or ethcgas == nil then goto FAILED end
print("Test 2 passed")
n_test = n_test + 1

-- test 3: Create a MultiYieldReactor block.
F1 = MultiYieldReactor("F1", { profeed, ethfeed }, { procgas, ethcgas }, "furn", "pro", "eth")
if F1 == nil then goto FAILED end
print("Test 3 passed")
n_test = n_test + 1

Eval([[
    F1.profeed.mass_C3H8    = 100.0
    F1.ethfeed.mass_C2H6    = 100.0
    F1.pro_y_CH4_from_C3H8  = 0.3
    F1.pro_y_C2H4_from_C3H8 = 0.5
    F1.eth_y_H2_from_C2H6   = 0.2
    F1.eth_y_C2H4_from_C2H6 = 0.5
    F1.pro_feed_rate        = 50.0
    F1.eth_feed_rate        = 50.0
]])

Init()
Eval("F1.profeed.mass_C3H8 = 200.0")

print("Before solve:\n")
ShowVariables()
ShowModel()

SolverOption("hessian_approximation", "exact")
SolverOption("max_iter", 50)
SolverOption("derivative_test", "second-order");
SolverOption("tol", 1.0e-6)
SolverOption("obj_scaling_factor", -1.0)
SolverOption("grad_f_constant", "yes")

status = InitSolver()
if status ~= 0 then goto FAILED end

-- test 4: Solve the problem.
status = Solve()
if status ~= 0 then goto FAILED end
print("Test 4 passed")
n_test = n_test + 1

print("After solve:\n")
ShowVariables()

print(string.format("\nAll %d tests passed\n", n_test - 1))
do return end

::FAILED::
print(string.format("\nTest %d failed\n", n_test))
do return end
