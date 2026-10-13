# generated: elab-check --write
attribute \top 1
module \n6_genvar
  wire width 4 input 1 \a
  wire width 4 output 2 \y
  wire width 4 output 3 \z
  cell $not $c4
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [0]
    connect \Y \z [0]
  end
  cell $not $c5
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [1]
    connect \Y \z [1]
  end
  cell $not $c6
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [2]
    connect \Y \z [2]
  end
  cell $not $c7
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [3]
    connect \Y \z [3]
  end
  connect \y \a
end
