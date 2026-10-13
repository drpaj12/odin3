# generated: elab-check --write
attribute \top 1
module \l13_define
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 output 3 \y1
  wire width 4 output 4 \y2
  wire width 4 output 5 \y3
  wire width 4 output 6 \y4
  cell $and $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \y1
  end
  cell $or $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B 4'0000
    connect \Y \y2
  end
  cell $and $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \y3
  end
  connect \y4 4'1000
end
