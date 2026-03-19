---@diagnostic disable: undefined-global

kinds = {
    massflow = { "kg/hr",    "kg/hr"    },  -- {base_unit_str, default_unit_str}
    massfrac = { "massfrac", "massfrac" },
    moleflow = { "kmol/hr",  "kmol/hr"  },
    molewt   = { "kg/kmol",  "kg/kmol"  },
    frac     = { "frac",     "frac"     },
    massval  = { "$/kg",     "$/kg"     },
    flowval  = { "$/hr",     "$/hr"     },
    count    = { "#",        "#"        }
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
    moleflow = {
        { "kmol/hr", 1.0,        0.0 }
    },
    molewt = {
        { "kg/kmol", 1.0,        0.0 }
    },
    frac = {
        { "frac", 1.0         },
        { "%",    1.0 / 100.0 }
    },
    massval = {
        { "$/kg", 1.0     },
        { "$/lb", 2.20462 }
    },
    flowval = {
        { "$/hr",  1.0        },
        { "$/day", 1.0 / 24.0 }
    },
    count = {
        { "#", 1.0}
    }
}
us = UnitSet(kinds, units)
