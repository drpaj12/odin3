# generated: elab-check --write
attribute \top 1
module \g2_loop_gen
  wire width 8 input 1 \a
  wire width 8 output 2 \y1
  wire width 8 output 3 \y2
  wire width 8 output 4 \y4
  wire width 16 output 5 \y3
  cell $and $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [0]
    connect \B \a [0]
    connect \Y \y3 [0]
  end
  cell $and $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [0]
    connect \B \a [1]
    connect \Y \y3 [1]
  end
  cell $and $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [0]
    connect \B \a [2]
    connect \Y \y3 [2]
  end
  cell $and $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [0]
    connect \B \a [3]
    connect \Y \y3 [3]
  end
  cell $and $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [1]
    connect \B \a [0]
    connect \Y \y3 [4]
  end
  cell $and $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [1]
    connect \B \a [1]
    connect \Y \y3 [5]
  end
  cell $and $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [1]
    connect \B \a [2]
    connect \Y \y3 [6]
  end
  cell $and $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [1]
    connect \B \a [3]
    connect \Y \y3 [7]
  end
  cell $and $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [2]
    connect \B \a [0]
    connect \Y \y3 [8]
  end
  cell $and $c15
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [2]
    connect \B \a [1]
    connect \Y \y3 [9]
  end
  cell $and $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [2]
    connect \B \a [2]
    connect \Y \y3 [10]
  end
  cell $and $c17
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [2]
    connect \B \a [3]
    connect \Y \y3 [11]
  end
  cell $and $c18
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [3]
    connect \B \a [0]
    connect \Y \y3 [12]
  end
  cell $and $c19
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [3]
    connect \B \a [1]
    connect \Y \y3 [13]
  end
  cell $and $c20
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [3]
    connect \B \a [2]
    connect \Y \y3 [14]
  end
  cell $and $c21
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [3]
    connect \B \a [3]
    connect \Y \y3 [15]
  end
  cell $not $c22
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [0]
    connect \Y \y4 [0]
  end
  cell $not $c23
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [1]
    connect \Y \y4 [1]
  end
  cell $not $c24
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [2]
    connect \Y \y4 [2]
  end
  cell $not $c25
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [3]
    connect \Y \y4 [3]
  end
  cell $not $c26
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [4]
    connect \Y \y4 [4]
  end
  cell $not $c27
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [5]
    connect \Y \y4 [5]
  end
  cell $not $c28
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [6]
    connect \Y \y4 [6]
  end
  cell $not $c29
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [7]
    connect \Y \y4 [7]
  end
  connect \y1 { \a [0] \a [1] \a [2] \a [3] \a [4] \a [5] \a [6] \a [7] }
  connect \y2 \a
end
