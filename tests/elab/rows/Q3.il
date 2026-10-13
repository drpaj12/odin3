# generated: elab-check --write
attribute \top 1
module \q3_tristate_gates
  wire input 1 \d
  wire input 2 \en
  wire output 3 \y1
  wire output 4 \y2
  wire output 5 \y3
  wire output 6 \y4
  wire $n7
  wire $n8
  wire $n9
  wire $n10
  cell $not $c7
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \en
    connect \Y $n7
  end
  cell $tribuf $c8
    parameter \WIDTH 1
    connect \A \d
    connect \EN $n7
    connect \Y \y1
  end
  cell $tribuf $c9
    parameter \WIDTH 1
    connect \A \d
    connect \EN \en
    connect \Y \y2
  end
  cell $not $c10
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \d
    connect \Y $n8
  end
  cell $not $c11
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \en
    connect \Y $n9
  end
  cell $tribuf $c12
    parameter \WIDTH 1
    connect \A $n8
    connect \EN $n9
    connect \Y \y3
  end
  cell $not $c13
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \d
    connect \Y $n10
  end
  cell $tribuf $c14
    parameter \WIDTH 1
    connect \A $n10
    connect \EN \en
    connect \Y \y4
  end
end
