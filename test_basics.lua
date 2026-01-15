---@diagnostic disable: undefined-global
function isapprox(x1, x2, tol)
    tol = tol or 1.0e-8
    return math.abs(x1 - x2) < tol
end

n_test = 1

-- set up the units for the model.
kinds = {
    massflow = { "kg/hr", "kg/hr"       },  -- {base_unit_str, default_unit_str}
    massfrac = { "massfrac", "massfrac" }
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
    }
}

-- test 1: Create a model.
M, FS = Model("test_basics", "index", kinds, units)
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
mix1 = Mixer("mix1", { N1, N2 }, OUT)
if mix1 == nil then goto FAILED end
print("Test 3 passed")
n_test = n_test + 1

-- test 4: Evaluate expressions.
ok = Eval([[
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

-- test 5: Get a variable value by name.
val, u = Val("mix1.N2.mass_CO")
if val == nil or u == nil then goto FAILED end
if not isapprox(val, 1.0/2.20462) then goto FAILED end
if u ~= "kg/hr" then goto FAILED end
print("Test 5 passed")
n_test = n_test + 1

--ShowVars(mix1, M)

print(string.format("\nAll %d tests passed\n", n_test - 1))
do return end

::FAILED::
print(string.format("\nTest %d failed\n", n_test))
do return end
