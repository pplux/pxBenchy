-- Demo 2.1 none: no grid draw (clear + floor only), baseline for the other demo2_* modes. Setup in lib/demo2.lua.
-- Runs --frames-warmup + --frames frames (the app resets timers after warm-up and prints the report).
local d = require("lib/demo2")
d.run(d.make_frame("none"))
