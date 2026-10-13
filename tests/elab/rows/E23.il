# generated: elab-check --write
attribute \top 1
module \e23_signed
  wire width 4 input 1 signed \a
  wire width 4 input 2 signed \b
  wire width 4 input 3 \u
  wire width 8 output 4 signed \y1
  wire width 8 output 5 signed \y2
  wire width 8 output 6 signed \y5
  wire width 8 output 7 signed \y6
  wire output 8 \y3
  wire width 8 output 9 \y4
  cell $add $c10
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y \y1
  end
  cell $add $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \u
    connect \Y \y2
  end
  cell $lt $c12
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y3
  end
  cell $sshr $c15
    parameter \A_SIGNED 1
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B 32'00000000000000000000000000000001
    connect \Y \y4
  end
  cell $add $c16
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 8
    connect \A 4'1000
    connect \B \a
    connect \Y \y5
  end
  cell $add $c17
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 8
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 8
    connect \A { \u [3] \u [3] \u [3] \u [3] \u }
    connect \B \a
    connect \Y \y6
  end
end
