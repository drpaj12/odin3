# generated: elab-check --write
attribute \top 1
module \q5_gate_arrays
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 output 3 \y
  wire width 4 output 4 \z
  cell $and $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [3]
    connect \B \b [3]
    connect \Y \y [3]
  end
  cell $and $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [2]
    connect \B \b [2]
    connect \Y \y [2]
  end
  cell $and $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [1]
    connect \B \b [1]
    connect \Y \y [1]
  end
  cell $and $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [0]
    connect \B \b [0]
    connect \Y \y [0]
  end
  cell $not $c9
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [0]
    connect \Y \z [0]
  end
  cell $not $c10
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [1]
    connect \Y \z [1]
  end
  cell $not $c11
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [2]
    connect \Y \z [2]
  end
  cell $not $c12
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [3]
    connect \Y \z [3]
  end
end
