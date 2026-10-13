# generated: elab-check --write
attribute \top 1
module \m7_inout_tristate
  wire inout 1 \io
  wire width 4 inout 2 \bus
  wire inout 3 \pin
  wire input 4 \oe
  wire input 5 \d
  wire width 4 input 6 \dv
  wire output 7 \y1
  wire width 4 output 8 \y2
  cell $tribuf $c9
    parameter \WIDTH 1
    connect \A \d
    connect \EN \oe
    connect \Y \io
  end
  cell $tribuf $c10
    parameter \WIDTH 4
    connect \A \dv
    connect \EN \oe
    connect \Y \bus
  end
  cell $tribuf $c11
    parameter \WIDTH 1
    connect \A \d
    connect \EN \oe
    connect \Y \pin
  end
  connect \y1 \io
  connect \y2 \bus
end
